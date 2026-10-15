#requires -Version 5
<#
Regression test for the "current" steamId keyword on PICKED-UP weapons -- i.e. a player
holding a weapon entity that is not their own (weapon econ OriginalOwnerXuid != holder's
steamId). This is a distinct scenario from verify-cosmetics-weapon-moments.ps1:

  * That script drives "cosmetics player <steamId> weapon ..." with a steamId ALREADY
    resolved in PowerShell (Resolve-WeaponPaintSteamId), so it never exercises the
    literal "current" token or CosmeticCommands.cpp's TokenIs(steamToken, "current")
    branch at all.
  * The bug this guards against: "cosmetics player current weapon <def> paint <id>"
    while spectating the HOLDER of a picked-up weapon used to resolve "current" to the
    spectated player's OWN steamId (CurrentSpectatedSteamId()). ApplyMatchedWeapons
    matches weapon-slot overrides by the WEAPON ENTITY's real econ owner
    (OriginalOwnerXuid), not the holder -- so the override got filed under the wrong
    key and silently never rendered on the weapon the user was actually looking at.
    Fixed by CosmeticOverrideSystem::CurrentActiveWeaponOwnerSteamId() (resolves
    "current" to the HELD weapon's real owner for the weapon slot only).

This script pulls straight from a scan's RAW moments (scan_raw.json, not the trimmed
moments.json/verification_run.json) because the general selection pipeline caps at one
pickup case per weapon type -- too few to trust as a regression suite for this specific
codepath. Run scan-cosmetics-weapon-moments.ps1 first (or use run-cosmetics-pickup-current.ps1,
which chains both).
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$ScanRawFile = "",
    [string]$OutDir = "",
    [int]$MaxCases = 12,
    [string]$WeaponCrop = "760,560,1500,1180",
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$NoLaunch,
    [switch]$LeaveCs2Open
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

if (-not $Thorough -and -not $PSBoundParameters.ContainsKey('Fast')) { $Fast = $true }
$readDiag = if ($Fast) { 0.7 } else { 1.5 }
$readApply = if ($Fast) { 1.5 } else { 2.5 }
$sleepApply = if ($Fast) { 350 } else { 600 }

function Resolve-ScanRawFile {
    if ($ScanRawFile) {
        if ([IO.Path]::IsPathRooted($ScanRawFile)) { return (Resolve-Path -LiteralPath $ScanRawFile).Path }
        return (Resolve-Path -LiteralPath (Join-Path $root $ScanRawFile)).Path
    }
    $cand = Get-ChildItem (Join-Path $root 'Automation\output\cosmetics_weapon_moments') -Recurse -Filter 'scan_raw.json' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $cand) {
        throw "No scan_raw.json found under Automation\output\cosmetics_weapon_moments -- run scan-cosmetics-weapon-moments.ps1 first (or use run-cosmetics-pickup-current.ps1)."
    }
    return $cand.FullName
}

function Get-OwnershipType($m) {
    $owner = "$($m.weaponOwnerSteamId)"; if ([string]::IsNullOrWhiteSpace($owner) -or $owner -eq '0') { $owner = "$($m.ownerSteamId)" }
    $holder = "$($m.playerSteamId)"; if ([string]::IsNullOrWhiteSpace($holder)) { $holder = "$($m.holderSteamId)" }
    if ($owner -and $holder -and $owner -ne '0' -and $holder -ne '0' -and $owner -ne $holder) { return 'pickup' }
    if ($m.ownershipType) { return "$($m.ownershipType)" }
    return 'owned'
}

$scanRawPath = Resolve-ScanRawFile
$scanDir = Split-Path -Parent $scanRawPath
$scan = Get-Content -LiteralPath $scanRawPath -Raw | ConvertFrom-Json
$demoPlay = if ($scan.demo) { $scan.demo } else { "replays/demo" }

$outAbs = if ([string]::IsNullOrWhiteSpace($OutDir)) { Join-Path $scanDir 'verify_pickup_current' } elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'verify.log'
$proofFile = Join-Path $outAbs 'proof.json'
"=== verify-pickup-current $(Get-Date -Format o) fast=$Fast scan=$scanRawPath ===" | Set-Content -LiteralPath $logFile

$allMoments = @($scan.moments)
$pickups = @($allMoments | Where-Object { (Get-OwnershipType $_) -eq 'pickup' -and [int]$_.weaponDefIndex -gt 0 })
# Keep the demo's actual pickup diversity (one case per (def, holder) pair, not per
# def) -- this script's whole point is coverage of the pickup codepath, unlike the
# general pipeline which caps at one pickup per weapon type.
$seen = @{}
$dedup = [System.Collections.Generic.List[object]]::new()
foreach ($m in ($pickups | Sort-Object { [int]$_.tick })) {
    $key = "$($m.weaponDefIndex)|$($m.playerSteamId)"
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $dedup.Add($m)
}
$testPlan = @($dedup | Select-Object -First $MaxCases)

Write-Host "Pickup-current verify: $($testPlan.Count) case(s) from $($pickups.Count) raw pickup moment(s) (scan: $scanRawPath)" -ForegroundColor Cyan
if ($testPlan.Count -eq 0) {
    Write-Host "No pickup moments in this scan -- nothing to verify. Try scan-cosmetics-weapon-moments.ps1 -Thorough -ScanTransitions on a demo with more weapon pickups." -ForegroundColor Yellow
    [ordered]@{ runAt = (Get-Date -Format o); scanFile = $scanRawPath; cases = @(); overall_pass = $true; note = 'no pickup moments found' } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    exit 0
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
    if ($tag) { Add-Content -LiteralPath $logFile -Value "--- $tag ---`n$t" }
    return $t
}

function Read-Diag {
    $t = NC @('mirv_filmmaker cosmetics visualdiag') $readDiag 'diag'
    return Parse-VisualDiagFields $t
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

function Seek-Tick([int]$tick, [ref]$loaded) {
    if ($tick -eq $loaded.Value) { return }
    $seekRead = if ($Fast) { 3.0 } else { 6.0 }
    $delta = [Math]::Abs($tick - $loaded.Value)
    if ($delta -gt 20000) { $seekRead = if ($Fast) { 8.0 } else { 12.0 } }
    NC @("demo_gototick $tick", 'demo_pause') $seekRead "seek$tick" | Out-Null
    Start-Sleep -Milliseconds 200
    $landed = Wait-DemoTickLanded -TargetTick $tick -Netcon ${function:NC}
    if (-not $landed.landed) {
        Write-Host "  WARN: seek to $tick did not settle (actual=$($landed.actual)) after extra wait" -ForegroundColor Yellow
    }
    $loaded.Value = $tick
}

# The exact bug signature: a "status" profile block keyed to $SteamId containing
# "weapon[$Def] ... paint=$PaintId" (see CosmeticDebug.cpp Cosmetics_PrintStatus /
# PrintItemLine: "  <steamId> '<name>':" header, then indented "weapon[N] def=N paint=N ...").
function Test-StatusProfileHasWeaponPaint {
    param([string]$StatusText, [string]$SteamId, [int]$Def, [int]$PaintId)
    if (-not $StatusText -or -not $SteamId -or $SteamId -eq '0') { return $false }
    $blockPattern = "(?ms)^\s*$([regex]::Escape($SteamId))\s+'[^']*':\s*\r?\n(.*?)(?=^\s*\d{5,}\s+'|\z)"
    $m = [regex]::Match($StatusText, $blockPattern)
    if (-not $m.Success) { return $false }
    return [bool]([regex]::IsMatch($m.Groups[1].Value, "weapon\[$Def\][^\r\n]*paint=$PaintId\b"))
}

function Test-BenignCrashVeh([int]$Delta, [bool]$PaintOk, [bool]$Cs2Alive) {
    return ($Delta -gt 0) -and $PaintOk -and $Cs2Alive
}

$defList = @($testPlan | ForEach-Object { [int]$_.weaponDefIndex } | Select-Object -Unique)
$paintsJson = & python (Join-Path $automationRoot 'tools\export_test_paints.py') @($defList | ForEach-Object { "$_" })
$paintsMap = $paintsJson | ConvertFrom-Json

$proof = [ordered]@{ runAt = (Get-Date -Format o); fast = [bool]$Fast; demo = $demoPlay; scanFile = $scanRawPath; cases = @(); crashVeh = 0 }

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
    # "cosmetics clear" wipes any profile left over from a PRIOR run/session (the store persists to
    # disk at %APPDATA%\HLAE\cosmetic_profiles.json across CS2 restarts). Without this, re-running the
    # same case against an already-painted weapon shows a false "diff too low" failure -- not because
    # the fix stopped working, but because applying the same paint twice produces no visual delta.
    NC @('mirv_filmmaker follow stop', 'mirv_filmmaker follow clear', 'mirv_filmmaker cosmetics ticknudge off',
        'mirv_filmmaker cosmetics mesh auto', 'mirv_filmmaker cosmetics clear', 'mirv_filmmaker cosmetics enabled 1') 1.2 'setup' | Out-Null

    $loadedTick = -1
    $dummyLoaded = -1
    # Cold-start warm-up seek; the first packet-replay seek is often slower than later seeks.
    Seek-Tick 500 ([ref]$dummyLoaded)

    foreach ($m in $testPlan) {
        $tick = [int]$m.tick
        $def = [int]$m.weaponDefIndex
        $holder = "$($m.playerSteamId)"
        $mid = "p{0:D3}_{1}" -f $tick, $def
        Write-Host "`n[$mid] $($m.weaponName) tick=$tick holder=$holder scanOwner=$($m.weaponOwnerSteamId)" -ForegroundColor Yellow
        $caseCrashBaseline = Get-CrashVehCount

        $case = [ordered]@{
            id = $mid; tick = $tick; def = $def; weapon = $m.weaponName
            holderSteam = $holder; holderName = $m.playerName; team = $m.team
            scanOwnerSteam = "$($m.weaponOwnerSteamId)"
            pass = $false; skipped = $null; errors = @()
        }

        try {
            Seek-Tick $tick ([ref]$loadedTick)
            $spec = Invoke-CosmeticsSpectate -SteamId $holder -Netcon ${function:NC}
            if ($spec.dead) { $case.skipped = 'holder dead at tick'; $proof.cases += $case; continue }
            if (-not $spec.ok) { $case.skipped = 'spectate failed'; $proof.cases += $case; continue }
            Start-Sleep -Milliseconds 150

            $specSb = { param($s) Invoke-CosmeticsSpectate -SteamId $s -Netcon ${function:NC} | Out-Null }
            $readSb = { Read-Diag }
            $preDiag = Read-WeaponDiag -ExpectedDef $def -SpectateSteam $holder -DiagReader $readSb -SpectateFn $specSb
            if (Test-PlayerDeadFromDiag $preDiag) { $case.skipped = 'holder dead at tick (diag)'; $proof.cases += $case; continue }

            $liveOwner = "$($preDiag.ownerXuid)"
            $case.liveOwnerSteam = $liveOwner
            if (-not $liveOwner -or $liveOwner -eq '0' -or $liveOwner -eq $holder) {
                $case.skipped = "not a live pickup at this tick (ownerXuid='$liveOwner' holder='$holder' -- ticks may have drifted since scan)"
                $proof.cases += $case
                continue
            }

            $wp = $paintsMap."$def"
            if (-not $wp) { $case.skipped = 'no test paint pair in skin DB for this weapon'; $proof.cases += $case; continue }
            $paintId = [int]$wp.modernPaint.id
            $paintName = $wp.modernPaint.name
            $case.paintId = $paintId; $case.paintName = $paintName

            NC @('mirv_filmmaker follow stop', 'demo_pause') 0.8 'fp' | Out-Null
            $baselineShot = Capture "${mid}_baseline.png"

            # THE actual regression exercise: literal "current" while spectating the HOLDER, not a
            # pre-resolved steamId. This is the exact command a user (or the Panorama netcon console)
            # would type -- CosmeticCommands.cpp must resolve "current" to $liveOwner here, not $holder.
            NC @(
                'mirv_filmmaker cosmetics mesh auto',
                "mirv_filmmaker cosmetics player current weapon $def paint $paintId wear 0.05 seed 0"
            ) $readApply "apply_$mid" | Out-Null
            Start-Sleep -Milliseconds $sleepApply

            $postDiag = Read-WeaponDiag -ExpectedDef $def -SpectateSteam $holder -DiagReader $readSb -SpectateFn $specSb
            $afterShot = Capture "${mid}_after.png"
            $diffMean = Diff-Mean $baselineShot $afterShot

            $meshSkipped = -not $preDiag.hasNetworkedPaint
            $expMask = Get-ExpectedMeshMaskForPaint -PaintId $paintId -Wp $wp
            $meshOk = if ($meshSkipped) { $diffMean -gt 0.5 } elseif ($expMask) { Test-MeshMaskOk -Diag $postDiag -ExpectedMask $expMask } else { $true }
            $paintOk = ("$($postDiag.paint)" -eq "$paintId")
            $renderedOnHolder = $paintOk -and $meshOk -and ($diffMean -gt 0.5)

            $statusText = NC @('mirv_filmmaker cosmetics status') 1.2 "status_$mid"
            $filedUnderOwner = Test-StatusProfileHasWeaponPaint -StatusText $statusText -SteamId $liveOwner -Def $def -PaintId $paintId
            $filedUnderHolder = Test-StatusProfileHasWeaponPaint -StatusText $statusText -SteamId $holder -Def $def -PaintId $paintId

            $crashSnap = Export-CrashVehReport -Tag "case_$mid" -OutDir $outAbs -Port $Port -BaselineCount $caseCrashBaseline
            $cs2Up = (Test-Cs2Netcon $Port) -and $crashSnap.cs2ProcessAlive
            $crashBenign = Test-BenignCrashVeh -Delta $crashSnap.crashVehDelta -PaintOk $paintOk -Cs2Alive $cs2Up
            $crashOk = ($crashSnap.crashVehDelta -eq 0) -or $crashBenign

            $errors = [System.Collections.Generic.List[string]]::new()
            if (-not $paintOk) { $errors.Add("holder paint readback mismatch: want=$paintId got=$($postDiag.paint)") }
            if (-not $meshOk) { $errors.Add("mesh/visual check failed: expectedMask=$expMask world=$($postDiag.worldMeshMask) vm=$($postDiag.vmMeshMask) diffMean=$diffMean") }
            if ($diffMean -le 0.5) { $errors.Add("baseline/after screenshot diff too low ($([math]::Round($diffMean,3))) - weapon may not have visually changed in the holder's hand") }
            if (-not $filedUnderOwner) { $errors.Add("cosmetics status: no profile entry for real owner steamId=$liveOwner with weapon[$def] paint=$paintId -- 'current' did not resolve to the weapon's actual econ owner") }
            if ($filedUnderHolder) { $errors.Add("cosmetics status: override incorrectly filed under HOLDER steamId=$holder (the historical bug) instead of only the real owner=$liveOwner") }
            if (-not $cs2Up) { $errors.Add('CS2/netcon not responding after test') }
            if (-not $crashOk) { $errors.Add("crash.veh delta=$($crashSnap.crashVehDelta) (not benign)") }

            $case.liveOwnerSteam = $liveOwner
            $case.paintGot = $postDiag.paint
            $case.diffMean = $diffMean
            $case.meshSkipped = $meshSkipped
            $case.renderedOnHolder = $renderedOnHolder
            $case.filedUnderOwner = $filedUnderOwner
            $case.filedUnderHolder = $filedUnderHolder
            $case.crashVehDelta = $crashSnap.crashVehDelta
            $case.pass = ($errors.Count -eq 0)
            $case.errors = @($errors)
            $proof.cases += $case

            Write-Host "  owner(live)=$liveOwner paint want=$paintId got=$($postDiag.paint) diff=$([math]::Round($diffMean,2)) filedOwner=$filedUnderOwner filedHolder=$filedUnderHolder pass=$($case.pass)" `
                -ForegroundColor $(if ($case.pass) { 'Green' } else { 'Red' })

            if (-not $cs2Up) { break }
            if (-not $crashOk) { break }
        }
        catch {
            $case.errors = @("unexpected error: $($_.Exception.Message)")
            $case.pass = $false
            $proof.cases += $case
            Write-Host "  ERROR: $($_.Exception.Message)" -ForegroundColor Red
            if (-not (Test-Cs2Netcon $Port)) { break }
        }
    }

    $finalCrash = Export-CrashVehReport -Tag 'verify_end' -OutDir $outAbs -Port $Port -BaselineCount $crashBaseline
    $proof.crashVeh = $finalCrash.crashVehCount
    $proof.crashVehDelta = $finalCrash.crashVehDelta
    $proof.cs2Alive = Test-Cs2Netcon $Port
    $evaluated = @($proof.cases | Where-Object { -not $_.skipped })
    $failed = @($evaluated | Where-Object { -not $_.pass })
    $proof.evaluatedCount = $evaluated.Count
    $proof.skippedCount = @($proof.cases | Where-Object { $_.skipped }).Count
    $proof.overall_pass = ($failed.Count -eq 0) -and $proof.cs2Alive -and ($evaluated.Count -gt 0)

    $proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    if (-not $LeaveCs2Open -and -not $NoLaunch) {
        # Leave CS2 running by default like the other verify scripts (cheap to keep for a follow-up run);
        # nothing to clean up here.
    }

    Write-Host ""
    Write-Host "Evaluated $($evaluated.Count) pickup case(s), $($proof.skippedCount) skipped, $($failed.Count) failed." -ForegroundColor Cyan
    if (-not $proof.overall_pass) { Write-Host "FAIL: $proofFile" -ForegroundColor Red; exit 1 }
    Write-Host "PASS $proofFile" -ForegroundColor Green
    exit 0
}
catch {
    $proof.error = $_.Exception.Message
    try { Export-CrashVehReport -Tag 'verify_catch' -OutDir $outAbs -Port $Port | Out-Null } catch {}
    $proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
