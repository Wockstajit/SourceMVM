#requires -Version 5
<#
Execute verification_run.json - apply legacy/modern paints, screenshots, flicker + crash.veh.

-Fast (default): one full skin/mesh test per unique weapon type (not per moment);
  single flicker check at end; fewer seeks; keeps CS2 session when chained after scan.

-Thorough: full test per moment + per-moment flicker + stress seeks.
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$RunFile = "",
    [string]$OutDir = "",
    [int]$MaxMoments = 0,
    [int]$MaxWeaponsToTest = 40,
    [string]$WeaponCrop = "760,560,1500,1180",
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$NoLaunch,
    [switch]$PreviewOnly,
    [switch]$SkipFlicker
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\CosmeticsSpectate.ps1')
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

if (-not $Thorough -and -not $PSBoundParameters.ContainsKey('Fast')) { $Fast = $true }
if ($Fast -and $MaxMoments -le 0) { $MaxMoments = 40 }
if (-not $Fast -and $MaxMoments -le 0) { $MaxMoments = 48 }
if ($Fast -and -not $PSBoundParameters.ContainsKey('SkipFlicker')) { $SkipFlicker = $false }

$readDiag = if ($Fast) { 0.7 } else { 1.5 }
$readApply = if ($Fast) { 1.5 } else { 2.5 }
$sleepApply = if ($Fast) { 350 } else { 600 }

if ([string]::IsNullOrWhiteSpace($RunFile)) {
    $cand = Get-ChildItem (Join-Path $root 'Automation\output\cosmetics_weapon_moments') -Recurse -Filter 'verification_run.json' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $cand) { throw "No verification_run.json - run scan first" }
    $RunFile = $cand.FullName
}
$runPath = Resolve-Path -LiteralPath $RunFile
$run = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
$scanDir = if ($run.scanFile) { Split-Path -Parent $run.scanFile } else { Split-Path -Parent $runPath.Path }
$outAbs = if ([string]::IsNullOrWhiteSpace($OutDir)) { Join-Path $scanDir 'verify' } elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'verify.log'
$proofFile = Join-Path $outAbs 'proof.json'
"=== verify $(Get-Date -Format o) fast=$Fast ===" | Set-Content -LiteralPath $logFile

$momentsJson = Join-Path $scanDir 'moments.json'
$demoPlay = if ($run.demo) { $run.demo } else { "replays/demo" }
if (Test-Path $momentsJson) {
    $mj = Get-Content $momentsJson -Raw | ConvertFrom-Json
    if ($mj.demo) { $demoPlay = $mj.demo }
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function NC([string[]]$cmds, [double]$read = 1.0, [string]$tag = '') {
    if (-not (Wait-Cs2Netcon -Port $Port)) {
        $r = Export-CrashVehReport -Tag $tag -OutDir $outAbs -Port $Port
        if ($r.cs2ProcessAlive -and $r.crashVehCount -eq 0) {
            throw "netcon timeout ($tag) - CS2 still running (not a crash)"
        }
        throw "cs2/netcon down ($tag) cs2=$($r.cs2ProcessAlive) crash.veh=$($r.crashVehCount)"
    }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -EA SilentlyContinue
    return $t
}

function New-ErrCase([string]$mid, [object]$m, [int]$def, [string]$errMsg) {
    return [ordered]@{
        id = $mid
        def = $def
        weapon = $m.weaponName
        demoName = $m.demoName
        tick = $m.tick
        playerName = $m.playerName
        team = $m.team
        holderSteam = "$($m.playerSteamId)"
        ownerSteam = $m.weaponOwnerSteamId
        originalOwnerEntityId = $m.ownerEntityId
        ownershipType = $m.ownershipType
        weaponEntityId = $m.weaponEntityId
        originalSkinName = $m.skinName
        originalPaintIndex = $m.paintIndex
        originalExpectedModelType = $m.expectedModelType
        activityState = $m.animationState
        pass = $false
        errors = @($errMsg)
        error = $errMsg
    }
}

function Capture([string]$name) {
    $path = Join-Path $outAbs $name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}

function Diff-Mean([string]$a, [string]$b) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $WeaponCrop
    return [double]((LastMatch $json '"mean":\s*([0-9.]+)'))
}

function Get-MvmTail([int]$after) {
    $log = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $log) { return @{ lines = @(); total = $after } }
    $all = Get-Content $log.FullName
    $slice = if ($after -lt $all.Count) { $all[$after..($all.Count - 1)] } else { @() }
    return @{ lines = $slice; total = $all.Count }
}

function Read-Diag {
    $t = NC @('mirv_filmmaker cosmetics visualdiag') $readDiag 'diag'
    $d = Parse-VisualDiagFields $t
    return $d
}

function Apply-WeaponPaint {
    param(
        [string]$PaintSteam,
        [int]$Def,
        [int]$PaintId
    )
    NC @(
        'mirv_filmmaker cosmetics mesh auto',
        "mirv_filmmaker cosmetics player $PaintSteam weapon $Def paint $PaintId wear 0.05 seed 0"
    ) $readApply "paint_$PaintId" | Out-Null
    Start-Sleep -Milliseconds $sleepApply
}

function Test-MeshStep {
    param(
        [string]$Tag,
        [string]$PaintSteam,
        [string]$HolderSteam,
        [int]$Def,
        [int]$PaintId,
        # Untyped on purpose -- see Get-ExpectedMeshMaskForPaint in CosmeticsSpectate.ps1.
        $Wp,
        [scriptblock]$ReadSb,
        [scriptblock]$SpecSb
    )
    Apply-WeaponPaint -PaintSteam $PaintSteam -Def $Def -PaintId $PaintId
    $d = Read-WeaponDiag -ExpectedDef $Def -SpectateSteam $HolderSteam -DiagReader $ReadSb -SpectateFn $SpecSb
    $expMask = Get-ExpectedMeshMaskForPaint -PaintId $PaintId -Wp $Wp
    $meshOk = if ($expMask) { Test-MeshMaskOk -Diag $d -ExpectedMask $expMask } else { $true }
    $shot = Capture "${Tag}.png"
    return @{
        paintWant = $PaintId
        paintGot = $d.paint
        paintOk = ("$($d.paint)" -eq "$PaintId")
        expectedMeshMask = $expMask
        worldMeshMask = $d.worldMeshMask
        vmMeshMask = $d.vmMeshMask
        paintLegacy = $d.paintLegacy
        meshOk = $meshOk
        worldModel = $d.worldModel
        screenshot = (Split-Path -Leaf $shot)
    }
}

function Test-BenignCrashVeh {
    param(
        [int]$Delta,
        [bool]$MeshSkipped,
        [bool]$PaintOk,
        [bool]$Cs2Alive
    )
    return ($Delta -gt 0) -and $MeshSkipped -and $PaintOk -and $Cs2Alive
}

function Test-WeaponCrashOk {
    param(
        [int]$Delta,
        [bool]$MeshSkipped,
        [bool]$PaintOk,
        [bool]$Cs2Alive
    )
    if ($Delta -eq 0) { return $true }
    return (Test-BenignCrashVeh -Delta $Delta -MeshSkipped $MeshSkipped -PaintOk $PaintOk -Cs2Alive $Cs2Alive)
}

function Assert-Cs2Alive([string]$tag) {
    if (Wait-Cs2Netcon -Port $Port) { return }
    $r = Export-CrashVehReport -Tag $tag -OutDir $outAbs -Port $Port
    if ($r.cs2ProcessAlive -and $r.crashVehCount -eq 0) {
        throw "netcon timeout ($tag) - CS2 still running (not a crash)"
    }
    throw "cs2/netcon down ($tag) cs2=$($r.cs2ProcessAlive) crash.veh=$($r.crashVehCount)"
}

function Seek-Tick([int]$tick, [ref]$loaded) {
    if ($tick -eq $loaded.Value) { return }
    $seekRead = if ($Fast) { 3.0 } else { 6.0 }
    $delta = [Math]::Abs($tick - $loaded.Value)
    if ($delta -gt 20000) { $seekRead = if ($Fast) { 8.0 } else { 12.0 } }
    NC @("demo_gototick $tick", 'demo_pause') $seekRead "seek$tick" | Out-Null
    Start-Sleep -Milliseconds 200
    # Don't trust the fixed read window above as proof the seek finished -- a
    # cold/long seek can still be mid-replay when it expires (confirmed live:
    # the engine landed ~138 ticks past target on the first seek of a session).
    # Poll the engine's own tick counter until it actually matches.
    $landed = Wait-DemoTickLanded -TargetTick $tick -Netcon ${function:NC}
    if (-not $landed.landed) {
        Write-Host "  WARN: seek to $tick did not settle (actual=$($landed.actual)) after extra wait" -ForegroundColor Yellow
    }
    $loaded.Value = $tick
}

# Test plan comes from Python case selection (owned + pickup per weapon type).
$sourceMoments = @($run.moments)
if ($MaxMoments -gt 0) { $sourceMoments = @($sourceMoments | Select-Object -First $MaxMoments) }
if ($MaxWeaponsToTest -gt 0 -and $sourceMoments.Count -gt $MaxWeaponsToTest) {
    $sourceMoments = @($sourceMoments | Select-Object -First $MaxWeaponsToTest)
}
$testPlan = $sourceMoments
Write-Host "Verify: $($testPlan.Count) mesh cases (owned + pickup per weapon type)" -ForegroundColor Cyan

# Batch-load test paints (one Python call)
$defList = @($testPlan | ForEach-Object { [int]$_.weaponDefIndex } | Select-Object -Unique)
$paintsJson = & python (Join-Path $automationRoot 'tools\export_test_paints.py') @($defList | ForEach-Object { "$_" })
$paintsMap = $paintsJson | ConvertFrom-Json

$proof = [ordered]@{ runAt = (Get-Date -Format o); fast = [bool]$Fast; demo = $demoPlay; cases = @(); weapons = @(); crashVeh = 0 }

try {
    if (-not $NoLaunch -and -not (Wait-Cs2Netcon -Port $Port -Retries 3)) {
        Get-Process -Name cs2,hlae -EA SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 -OutDir (Join-Path $outAbs 'launch')
    }

    NC @('mvm_debug start') 1.2 'mvm' | Out-Null
    $crashBaseline = Get-CrashVehCount
    NC @("playdemo `"$demoPlay`"") 7.0 'playdemo' | Out-Null
    Start-Sleep -Seconds 1
    NC @('mirv_filmmaker follow stop', 'mirv_filmmaker follow clear', 'mirv_filmmaker cosmetics ticknudge off',
        'mirv_filmmaker cosmetics mesh auto', 'mirv_filmmaker cosmetics enabled 1') 1.2 'setup' | Out-Null

    $loadedTick = -1
    $flickerTick = 0
    $flickerDef = 0
    $flickerPaintSteam = ''

    # Cold-start warm-up: the FIRST demo_gototick after playdemo is unreliable --
    # confirmed live, it can land 100+ ticks past the requested target and get
    # stuck there (Wait-DemoTickLanded's poll never converges, even after 6s).
    # The SECOND seek in a session lands exactly on target (the engine's own
    # skip log confirms "finished at tick N" for it). Absorb the cold-start
    # penalty with one cheap, disposable seek before trusting the real one.
    $dummyLoaded = -1
    Seek-Tick 500 ([ref]$dummyLoaded)

    # Warm-up: right after a fresh demo load, the very first spectate call can
    # silently echo stale data for the wrong player for one read (confirmed by
    # direct probing) -- absorb that here with a throwaway seek+spectate+diag
    # before trusting any real case's data. Seek-Tick below records the tick as
    # loaded, so the real loop's first seek for this same tick is a cheap no-op.
    if ($testPlan.Count -gt 0) {
        $firstTick = [int]$testPlan[0].tick
        $firstSteam = "$($testPlan[0].playerSteamId)"
        Seek-Tick $firstTick ([ref]$loadedTick)
        Invoke-CosmeticsSpectate -SteamId $firstSteam -Netcon ${function:NC} | Out-Null
        Start-Sleep -Milliseconds 300
        NC @('mirv_filmmaker cosmetics visualdiag') 0.8 'warmup' | Out-Null
    }

    foreach ($m in $testPlan) {
        $mid = $m.id
        $tick = [int]$m.tick
        $def = [int]$m.weaponDefIndex
        $steam = "$($m.playerSteamId)"
        $key = "$def"
        Write-Host "`n[$mid] $($m.weaponName) tick=$tick own=$($m.ownershipType)" -ForegroundColor Yellow
        $weaponCrashBaseline = Get-CrashVehCount

        # Any unexpected script-level failure below (type mismatch, null deref, an
        # engine sentinel value that doesn't fit an assumed type, etc.) for ONE case
        # must not abort every other weapon's coverage -- catch it, record it, and
        # move on to the next case unless CS2 itself is actually down.
        try {
        Seek-Tick $tick ([ref]$loadedTick)
        $actualTickAfterSeek = Get-ActualDemoTick -Netcon ${function:NC}
        Write-Host "  [ticktap] requested=$tick actual=$actualTickAfterSeek" -ForegroundColor Magenta
        Assert-Cs2Alive "pre_lock_$mid"
        $spec = Invoke-CosmeticsSpectate -SteamId $steam -Netcon ${function:NC}
        if ($spec.dead) {
            $err = New-ErrCase $mid $m $def 'player dead at tick'
            $proof.cases += $err; $proof.weapons += $err
            continue
        }
        if (-not $spec.ok) {
            $err = New-ErrCase $mid $m $def 'cosmetics spectate failed (spectating, dormant, or invalid pawn)'
            $proof.cases += $err; $proof.weapons += $err
            continue
        }
        Start-Sleep -Milliseconds 150
        $specSb = { param($s) Invoke-CosmeticsSpectate -SteamId $s -Netcon ${function:NC} | Out-Null }
        $readSb = { Read-Diag }
        $preDiag = Read-WeaponDiag -ExpectedDef $def -SpectateSteam $steam -DiagReader $readSb -SpectateFn $specSb
        if (Test-PlayerDeadFromDiag $preDiag) {
            $err = New-ErrCase $mid $m $def 'player dead at tick'
            $proof.cases += $err; $proof.weapons += $err
            continue
        }
        $paintTarget = Resolve-WeaponPaintSteamId -Diag $preDiag -SpectateSteam $steam
        $paintSteam = $paintTarget.paintSteam
        if ($paintTarget.pickup) {
            Write-Host "  pickup: weapon owner=$($paintTarget.ownerSteam) (spectating $steam)" -ForegroundColor Yellow
        }

        NC @('mirv_filmmaker follow stop', 'demo_pause') 0.8 'fp' | Out-Null
        Assert-Cs2Alive "pre_baseline_$mid"
        Capture "${mid}_baseline.png" | Out-Null

        if ($PreviewOnly) {
            $proof.weapons += [ordered]@{ id = $mid; def = $def; pass = $true; previewOnly = $true }
            continue
        }

        $wp = $paintsMap.$key
        if (-not $wp) {
            $caseRec = New-ErrCase $mid $m $def 'no legacy/cs2 test-paint pair in skin DB for this weapon'
            $caseRec.pass = $true
            $caseRec.errors = @()
            $caseRec.error = $null
            $caseRec.skipped = 'no test paints'
            $proof.cases += $caseRec
            $proof.weapons += $caseRec
            continue
        }

        $legId = [int]$wp.legacyPaint.id
        $modId = [int]$wp.modernPaint.id
        $legName = $wp.legacyPaint.name
        $modName = $wp.modernPaint.name
        $flickerTick = $tick
        $flickerDef = $def
        $flickerPaintSteam = $paintSteam
        $meshSkipped = -not $preDiag.hasNetworkedPaint

        # Direction A: legacy skin -> CS2 skin (model should follow)
        $stepLeg = Test-MeshStep -Tag "${mid}_legacy" -PaintSteam $paintSteam -HolderSteam $steam -Def $def -PaintId $legId -Wp $wp -ReadSb $readSb -SpecSb $specSb
        $stepMod = Test-MeshStep -Tag "${mid}_legacy_to_cs2" -PaintSteam $paintSteam -HolderSteam $steam -Def $def -PaintId $modId -Wp $wp -ReadSb $readSb -SpecSb $specSb
        $legToCs2Diff = Diff-Mean (Join-Path $outAbs $stepLeg.screenshot) (Join-Path $outAbs $stepMod.screenshot)

        # Direction B: CS2 skin -> legacy skin
        $stepMod2 = Test-MeshStep -Tag "${mid}_modern" -PaintSteam $paintSteam -HolderSteam $steam -Def $def -PaintId $modId -Wp $wp -ReadSb $readSb -SpecSb $specSb
        $stepLeg2 = Test-MeshStep -Tag "${mid}_cs2_to_legacy" -PaintSteam $paintSteam -HolderSteam $steam -Def $def -PaintId $legId -Wp $wp -ReadSb $readSb -SpecSb $specSb
        $cs2ToLegDiff = Diff-Mean (Join-Path $outAbs $stepMod2.screenshot) (Join-Path $outAbs $stepLeg2.screenshot)

        $crashSnap = Export-CrashVehReport -Tag "weapon_$mid" -OutDir $outAbs -Port $Port -BaselineCount $weaponCrashBaseline

        $paintOk = $stepLeg.paintOk -and $stepMod.paintOk -and $stepMod2.paintOk -and $stepLeg2.paintOk
        $meshOk = if ($meshSkipped) { ($legToCs2Diff -gt 0.5) -or ($cs2ToLegDiff -gt 0.5) } else {
            $stepLeg.meshOk -and $stepMod.meshOk -and $stepMod2.meshOk -and $stepLeg2.meshOk
        }
        $cs2Up = (Test-Cs2Netcon $Port) -and $crashSnap.cs2ProcessAlive
        $crashDelta = $crashSnap.crashVehDelta
        $crashBenign = Test-BenignCrashVeh -Delta $crashDelta -MeshSkipped $meshSkipped -PaintOk $paintOk -Cs2Alive $cs2Up
        $crashOk = Test-WeaponCrashOk -Delta $crashDelta -MeshSkipped $meshSkipped -PaintOk $paintOk -Cs2Alive $cs2Up
        $pass = $paintOk -and $meshOk -and $cs2Up -and $crashOk

        $legToCs2Pass = ($stepLeg.paintOk -and $stepMod.paintOk -and ($(if ($meshSkipped) { $legToCs2Diff -gt 0.5 } else { $stepMod.meshOk })))
        $cs2ToLegPass = ($stepMod2.paintOk -and $stepLeg2.paintOk -and ($(if ($meshSkipped) { $cs2ToLegDiff -gt 0.5 } else { $stepLeg2.meshOk })))

        $errors = [System.Collections.Generic.List[string]]::new()
        if (-not $stepLeg.paintOk) { $errors.Add("legacy paint readback mismatch: want=$legId got=$($stepLeg.paintGot)") }
        if (-not $stepMod.paintOk) { $errors.Add("legacy-to-cs2 paint readback mismatch: want=$modId got=$($stepMod.paintGot)") }
        if (-not $stepMod2.paintOk) { $errors.Add("cs2 paint readback mismatch: want=$modId got=$($stepMod2.paintGot)") }
        if (-not $stepLeg2.paintOk) { $errors.Add("cs2-to-legacy paint readback mismatch: want=$legId got=$($stepLeg2.paintGot)") }
        if ($meshSkipped) {
            if ($legToCs2Diff -le 0.5) { $errors.Add("legacy-to-cs2 screenshot diff too low ($([math]::Round($legToCs2Diff,3))) - model may not have visually changed") }
            if ($cs2ToLegDiff -le 0.5) { $errors.Add("cs2-to-legacy screenshot diff too low ($([math]::Round($cs2ToLegDiff,3))) - model may not have visually changed") }
        } else {
            if (-not $stepMod.meshOk) { $errors.Add("legacy-to-cs2 mesh mask mismatch: expected=$($stepMod.expectedMeshMask) world=$($stepMod.worldMeshMask) vm=$($stepMod.vmMeshMask)") }
            if (-not $stepLeg2.meshOk) { $errors.Add("cs2-to-legacy mesh mask mismatch: expected=$($stepLeg2.expectedMeshMask) world=$($stepLeg2.worldMeshMask) vm=$($stepLeg2.vmMeshMask)") }
        }
        if (-not $cs2Up) { $errors.Add('CS2/netcon not responding after test') }
        if (($crashDelta -gt 0) -and -not $crashBenign) { $errors.Add("crash.veh delta=$crashDelta (not benign)") }

        $caseRec = [ordered]@{
            id = $mid
            def = $def
            weapon = $m.weaponName
            demoName = $m.demoName
            tick = $tick
            playerName = $m.playerName
            team = $m.team
            holderSteam = $steam
            holderEntityId = $preDiag.pawn
            ownerSteam = $paintTarget.ownerSteam
            originalOwnerEntityId = $m.ownerEntityId
            paintSteam = $paintSteam
            pickup = [bool]$paintTarget.pickup
            ownershipType = if ($m.ownershipType) { $m.ownershipType } elseif ($paintTarget.pickup) { 'pickup' } else { 'owned' }
            weaponEntityId = $preDiag.weaponEntityId
            originalSkinName = $m.skinName
            originalPaintIndex = $m.paintIndex
            originalExpectedModelType = $m.expectedModelType
            activityState = $m.animationState
            legacySkin = @{ id = $legId; name = $legName }
            modernSkin = @{ id = $modId; name = $modName }
            meshSkipped = $meshSkipped
            legacyToCs2 = @{
                legacyStep = $stepLeg
                modernStep = $stepMod
                screenshotDiff = $legToCs2Diff
                pass = $legToCs2Pass
            }
            cs2ToLegacy = @{
                modernStep = $stepMod2
                legacyStep = $stepLeg2
                screenshotDiff = $cs2ToLegDiff
                pass = $cs2ToLegPass
            }
            switchDirectionsTested = @('legacy_to_cs2', 'cs2_to_legacy')
            crashVehDelta = $crashDelta
            crashVehBenign = $crashBenign
            paintOk = $paintOk
            meshOk = $meshOk
            pass = $pass
            errors = @($errors)
        }
        $proof.cases += $caseRec
        $proof.weapons += $caseRec

        $ownLabel = $caseRec.ownershipType
        $actualTickAtEnd = Get-ActualDemoTick -Netcon ${function:NC}
        Write-Host "  [ticktap-end] requested=$tick actual=$actualTickAtEnd drift=$($actualTickAtEnd - $tick)" -ForegroundColor Magenta
        Write-Host "  [$ownLabel] legacy=$($stepLeg.paintGot) cs2=$($stepMod.paintGot) meshL2C=$([math]::Round($legToCs2Diff,2)) meshC2L=$([math]::Round($cs2ToLegDiff,2)) crash+=$crashDelta pass=$pass" -ForegroundColor $(if ($pass) { 'Green' } else { 'Red' })
        if (-not $cs2Up) { break }
        if (($crashDelta -gt 0) -and -not $crashBenign) { break }
        }
        catch {
            $errMsg = "unexpected error: $($_.Exception.Message)"
            Write-Host "  ERROR: $errMsg" -ForegroundColor Red
            $err = New-ErrCase $mid $m $def $errMsg
            $proof.cases += $err; $proof.weapons += $err
            if (-not (Test-Cs2Netcon $Port)) { break }
        }
    }

    $flickerMeshSkipped = $false
    if ($flickerDef -gt 0) {
        $lastW = @($proof.weapons | Where-Object { $_.def -eq $flickerDef } | Select-Object -Last 1)
        if ($lastW -and $lastW.meshSkipped) { $flickerMeshSkipped = $true }
    }

    # One flicker check (not per weapon); skip fallback-only weapons (rebuild VEH noise)
    if (-not $SkipFlicker -and $flickerTick -gt 0 -and $flickerDef -gt 0 -and -not $flickerMeshSkipped -and (Test-Cs2Netcon $Port)) {
        $fKey = "$flickerDef"
        $fWp = $paintsMap.$fKey
        $fModId = if ($fWp) { [int]$fWp.modernPaint.id } else { 0 }
        if ($fModId -gt 0) {
        Write-Host "`nFlicker smoke (once)..." -ForegroundColor Cyan
        $mark = (Get-MvmTail 0).total
        Seek-Tick $flickerTick ([ref]$loadedTick)
        NC @("mirv_filmmaker cosmetics player $flickerPaintSteam weapon $flickerDef paint $fModId wear 0.05 seed 0") $readApply 'fprep' | Out-Null
        $back = [Math]::Max(1, $flickerTick - 32)
        NC @("demo_gototick $back", 'demo_pause') 2.0 'fback' | Out-Null
        $loadedTick = $back
        NC @('demo_resume') 0.4 'fres' | Out-Null
        Start-Sleep -Seconds $(if ($Fast) { 1 } else { 2 })
        NC @('demo_pause') 0.6 'fpau' | Out-Null
        $tail = Get-MvmTail $mark
        $nudge = @($tail.lines | Where-Object { $_ -match 'cosmetics\.nudge.*START' }).Count
        $proof.flicker = @{ nudgeStarts = $nudge; pass = ($nudge -eq 0) }
        Write-Host "  nudgeStarts=$nudge" -ForegroundColor $(if ($nudge -eq 0) { 'Green' } else { 'Red' })
        }
    } elseif ($flickerMeshSkipped) {
        Write-Host "`nFlicker smoke: skipped (fallback-only weapon)" -ForegroundColor DarkGray
        $proof.flicker = @{ skipped = $true; pass = $true; reason = 'fallback-only weapon' }
    }

    if (-not $Fast) {
        foreach ($st in @(4000, 16000, 32000)) {
            if (-not (Wait-Cs2Netcon -Port $Port -Retries 3)) { break }
            NC @("demo_gototick $st", 'demo_pause') 2.5 "stress$st" | Out-Null
            NC @('mirv_filmmaker cosmetics skinlog') 1.0 "log$st" | Out-Null
        }
    }

    $finalCrash = Export-CrashVehReport -Tag 'verify_end' -OutDir $outAbs -Port $Port -BaselineCount $crashBaseline
    $proof.crashVeh = $finalCrash.crashVehCount
    $proof.crashVehDelta = $finalCrash.crashVehDelta
    $proof.crashVehLines = @($finalCrash.crashVehLines)
    $proof.cs2Alive = Test-Cs2Netcon $Port
    $failed = @($proof.cases | Where-Object { -not $_.pass })
    $flickOk = (-not $proof.flicker) -or $proof.flicker.pass
    $benignDelta = (@($proof.cases | Where-Object { $_.crashVehBenign } | ForEach-Object { [int]$_.crashVehDelta }) | Measure-Object -Sum).Sum
    if (-not $benignDelta) { $benignDelta = 0 }
    $benignOnly = ($finalCrash.crashVehDelta -gt 0) -and ($failed.Count -eq 0) -and ($benignDelta -eq $finalCrash.crashVehDelta)
    $proof.crashVehBenignDelta = $benignDelta
    $proof.overall_pass = ($failed.Count -eq 0) -and $proof.cs2Alive -and (($finalCrash.crashVehDelta -eq 0) -or $benignOnly) -and $flickOk -and ($proof.cases.Count -gt 0)

    $proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    & python (Join-Path $automationRoot 'tools\make_moment_preview.py') $outAbs --pattern '*_baseline.png' 2>$null

    if (-not $proof.overall_pass) { exit 1 }
    Write-Host "`nPASS $proofFile" -ForegroundColor Green
    exit 0
}
catch {
    $proof.error = $_.Exception.Message
    try { Export-CrashVehReport -Tag 'verify_catch' -OutDir $outAbs -Port $Port | Out-Null } catch {}
    $proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
