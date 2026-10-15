#requires -Version 5
<#
Fast SCAN: find ticks where a player holds a primary/secondary (gun in hand).
Does NOT apply skins - verify-cosmetics-weapon-moments.ps1 does that.

Speed tips (default -Fast):
  * Few probe ticks from fmjson gun pickups only (not 20 uniform seeks)
  * Known players from fmjson - cosmetics spectate by steam (never spec_player 1..10)
  * Skips dead players offline (fmjson weapon_drop) and live (spectateHealth=0)
  * One demo load, ticks visited in ascending order
  * Skin metadata enriched in one Python batch at the end

Use -Thorough to disable fast defaults (transition scan, more ticks).
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "",
    [string]$OutDir = "",
    [int]$MaxProbeTicks = 0,
    [int]$MaxSpectateTries = 8,
    [int]$MaxGunHoldersPerTick = 2,
    [int]$StopAfterUniqueWeapons = 6,
    [int]$TransitionWindowTicks = 48,
    [int]$TransitionStep = 16,
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$ScanTransitions,
    [switch]$NoLaunch,
    [switch]$SkipOfflinePlan,
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
if ($Fast) {
    if ($MaxProbeTicks -le 0) { $MaxProbeTicks = 30 }
    if (-not $PSBoundParameters.ContainsKey('MaxGunHoldersPerTick')) { $MaxGunHoldersPerTick = 4 }
    if (-not $PSBoundParameters.ContainsKey('StopAfterUniqueWeapons')) { $StopAfterUniqueWeapons = 24 }
} elseif ($Thorough) {
    if ($MaxProbeTicks -le 0) { $MaxProbeTicks = 0 }
    if (-not $PSBoundParameters.ContainsKey('StopAfterUniqueWeapons')) { $StopAfterUniqueWeapons = 48 }
} elseif ($MaxProbeTicks -le 0) {
    $MaxProbeTicks = 20
}

$Script:PrimaryDefs = @(7,8,9,10,11,13,14,16,17,19,23,24,25,26,27,28,29,33,34,35,38,39,40,60)
$Script:SecondaryDefs = @(1,2,3,4,30,32,36,61,63,64)
$Script:KnifeDefs = @(42,59,500,503,505,506,507,508,509,512,514,515,516,517,518,519,520,521,522,523,525,526)
$Script:DiagReadSec = if ($Fast) { 0.65 } else { 1.2 }
$Script:SeekReadSec = if ($Fast) { 2.0 } else { 4.5 }

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(400)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

function Resolve-DemoPath {
    $replayDir = Join-Path $Cs2Dir 'game\csgo\replays'
    if ($Demo) {
        foreach ($c in @($Demo, (Join-Path $root $Demo), (Join-Path $Cs2Dir "game\csgo\$Demo"), (Join-Path $replayDir ($Demo -replace '^replays/', '')))) {
            $dem = if ($c -match '\.dem$') { $c } else { "$c.dem" }
            if (Test-Path -LiteralPath $dem) { return (Resolve-Path -LiteralPath $dem).Path }
        }
    }
    $pref = Join-Path $replayDir 'match730_003827583940474962026_0709708846_125.dem'
    if (Test-Path -LiteralPath $pref) { return (Resolve-Path -LiteralPath $pref).Path }
    $any = Get-ChildItem -LiteralPath $replayDir -Filter '*.dem' -EA SilentlyContinue | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if ($any) { return $any.FullName }
    throw "No demo found under $replayDir"
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function Get-SkipReason([int]$def) {
    if ($def -le 0) { return 'no active weapon' }
    if ($Script:KnifeDefs -contains $def) { return 'knife out' }
    if ($def -eq 43 -or $def -in @(44,45,46,47,48)) { return 'grenade out' }
    if ($def -eq 49) { return 'c4 out' }
    if ($def -eq 31) { return 'zeus out' }
    if ($def -ge 5027 -and $def -le 5040) { return 'gloves' }
    if (($Script:PrimaryDefs -notcontains $def) -and ($Script:SecondaryDefs -notcontains $def)) { return "not a gun (def $def)" }
    return $null
}

function Parse-VisualDiag([string]$text) {
    $d = Parse-VisualDiagFields $text
    return $d
}

function New-RawMoment(
    [hashtable]$diag, [int]$tick, [string]$anim, [string]$reason, [string]$ownershipHint = 'unknown',
    [string]$playerName = '', [object]$team = $null, [bool]$settled = $true, [string]$ownerEntityId = ''
) {
    $holder = "$($diag.steam)"
    $owner = if ($diag.ownerXuid) { "$($diag.ownerXuid)" } else { $holder }
    $pickup = ($owner -and $holder -and $owner -ne '0' -and $holder -ne '0' -and $owner -ne $holder)
    $otype = if ($pickup) { 'pickup' } elseif ($ownershipHint -in @('pickup', 'owned')) { $ownershipHint } else { 'owned' }
    return [ordered]@{
        tick = $tick
        playerSteamId = $holder
        holderSteamId = $holder
        playerEntityId = $diag.pawn
        playerName = $playerName
        team = $team
        weaponEntityId = $diag.weaponEntityId
        weaponOwnerSteamId = $owner
        ownerSteamId = $owner
        ownerEntityId = $(if ($pickup) { $ownerEntityId } else { $diag.pawn })
        ownershipType = $otype
        ownershipHint = $ownershipHint
        weaponDefIndex = [int]$diag.defIndex
        weaponClass = $diag.weaponClass
        paintIndex = if ($diag.paint) { [int]$diag.paint } else { 0 }
        hasNetworkedPaint = [bool]$diag.hasNetworkedPaint
        worldMeshMask = $diag.worldMeshMask
        vmMeshMask = $diag.vmMeshMask
        animationState = $anim
        settled = [bool]$settled
        reason = $reason
        activeWeapon = $true
    }
}

$demoPath = Resolve-DemoPath
$script:demoName = [IO.Path]::GetFileNameWithoutExtension($demoPath)
$demoPlay = "replays/$script:demoName"
$outAbs = if ([string]::IsNullOrWhiteSpace($OutDir)) {
    Join-Path $root "Automation\output\cosmetics_weapon_moments\$script:demoName"
} elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'scan.log'
$scanRawFile = Join-Path $outAbs 'scan_raw.json'
$enumFile = Join-Path $outAbs 'enumeration.log'
"=== scan $(Get-Date -Format o) fast=$Fast ===" | Set-Content -LiteralPath $logFile

function NC([string[]]$cmds, [double]$read = 1.0, [string]$tag = '') {
    if (-not (Wait-Cs2Netcon -Port $Port)) {
        $cs2 = Get-Cs2ProcessAlive
        if ($cs2) { throw "netcon timeout ($tag) - CS2 still running (not a crash)" }
        throw "netcon dead ($tag)"
    }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -EA SilentlyContinue
    if ($tag) { Add-Content -LiteralPath $logFile -Value "--- $tag ---`n$t" }
    return $t
}

function Read-Diag {
    return Parse-VisualDiag (NC @('mirv_filmmaker cosmetics visualdiag') $Script:DiagReadSec 'diag')
}

function Test-MomentSettled([hashtable]$d0) {
    <#
    A candidate found at the seek tick might be mid weapon-draw/switch -- the model
    hasn't fully appeared yet (exactly what the spec says to avoid). Nudge a few
    ticks forward with mirv_skip (cheap paused-frame stepping, not a packet-replay
    seek) and require the def index to hold steady. Also
    catches a reload edge via the m_nCustomEconReloadEventId counter so a genuinely
    reloading moment can be tagged instead of mislabeled 'idle'.
    #>
    NC @('mirv_skip tick 8') 0.6 'settle' | Out-Null
    Start-Sleep -Milliseconds 120
    $d1 = Read-Diag
    $defStable = ("$($d1.defIndex)" -eq "$($d0.defIndex)") -and $d1.defIndex
    $reloadEdge = $false
    if ($null -ne $d0.reloadEvent -and $null -ne $d1.reloadEvent -and "$($d0.reloadEvent)" -ne "$($d1.reloadEvent)") {
        $reloadEdge = $true
    }
    return @{ diag = $d1; settled = [bool]$defStable; reloadEdge = $reloadEdge }
}

function Resolve-OwnerEntityId([string]$ownerSteam, [string]$holderSteam) {
    # Best-effort: briefly spectate the weapon's owner to log their pawn entity id
    # ("original owner player id"), then hand spectate back to the holder.
    if (-not $ownerSteam -or $ownerSteam -eq '0' -or $ownerSteam -eq $holderSteam) { return '' }
    $spec = Invoke-CosmeticsSpectate -SteamId $ownerSteam -Netcon ${function:NC}
    $entId = ''
    if ($spec.ok -and -not $spec.dead) {
        Start-Sleep -Milliseconds 100
        $od = Read-Diag
        if ($od.pawn) { $entId = "$($od.pawn)" }
    }
    Invoke-CosmeticsSpectate -SteamId $holderSteam -Netcon ${function:NC} | Out-Null
    Start-Sleep -Milliseconds 100
    return $entId
}

function Seek-Tick([int]$tick) {
    NC @("demo_gototick $tick", 'demo_pause') $Script:SeekReadSec "seek_$tick" | Out-Null
    Start-Sleep -Milliseconds $(if ($Fast) { 200 } else { 500 })
    # Don't trust the fixed read window above as proof the seek finished -- a
    # cold/long seek can still be mid-replay when it expires (confirmed live in
    # the verify script: the engine landed ~138 ticks past target on the first
    # seek of a session). Poll the engine's own tick counter until it matches.
    $landed = Wait-DemoTickLanded -TargetTick $tick -Netcon ${function:NC}
    if (-not $landed.landed) {
        Write-Host "  WARN: seek to $tick did not settle (actual=$($landed.actual)) after extra wait" -ForegroundColor Yellow
    }
}

function Find-GunHoldersAtTick([int]$tick, [object[]]$targets) {
    $found = [System.Collections.Generic.List[object]]::new()
    Add-Content -LiteralPath $enumFile -Value "`n--- tick $tick ---"
    Write-Host "Tick $tick :" -NoNewline -ForegroundColor Cyan

    $tickTargets = @($targets | Where-Object { [int]$_.probeTick -eq $tick })
    if ($tickTargets.Count -eq 0) {
        Write-Host " no targets" -ForegroundColor DarkGray
        return @()
    }

    # Test-MomentSettled's mirv_skip nudge genuinely advances the demo clock (it's
    # not a re-seek), so the recorded tick for each moment must track where we
    # actually are, not the original probe tick -- otherwise VERIFY re-seeks to a
    # tick that no longer matches the diag data that was stored (e.g. the player
    # has since drawn a knife), and its retry logic burns through retries forever.
    $curTick = $tick

    foreach ($t in $tickTargets) {
        if ($found.Count -ge $MaxGunHoldersPerTick) { break }
        $steam = "$($t.steamId)"
        Write-Host " [$($t.playerName)]" -NoNewline -ForegroundColor DarkCyan
        if ($t.PSObject.Properties.Match('aliveAtProbe').Count -gt 0 -and $t.aliveAtProbe -eq $false) {
            Write-Host " .dead(off)" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam offline-dead"
            continue
        }
        # One netcon round trip instead of two (spectate + diag in the same call) --
        # a knife/grenade/dead candidate is a skip either way, so don't pay for a
        # separate powershell.exe + TCP connect just to find that out.
        $snap = Invoke-CosmeticsSpectateAndDiag -SteamId $steam -Netcon ${function:NC} -ReadSeconds $Script:DiagReadSec
        if ($snap.dead) {
            Write-Host " .dead" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam live-dead"
            continue
        }
        if (-not $snap.ok) {
            Write-Host " no-spec" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam spectate failed"
            continue
        }
        $d = $snap.diag
        if (Test-PlayerDeadFromDiag $d) {
            Write-Host " .dead" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam diag-dead"
            continue
        }
        $def = if ($d.defIndex) { [int]$d.defIndex } else { 0 }
        $skip = Get-SkipReason $def
        if ($skip) {
            Write-Host " .$skip" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam skip=$skip def=$def"
            continue
        }
        $hint = if ($t.PSObject.Properties.Match('ownershipHint').Count -gt 0) { "$($t.ownershipHint)" } else { 'unknown' }
        $preNudgeTick = $curTick
        $check = Test-MomentSettled $d
        $curTick = $preNudgeTick + 8   # mirv_skip physically advanced the demo this far
        $anim = if ($check.reloadEdge) { 'reload' } else { 'idle' }
        $srcDiag = if ($check.settled) { $check.diag } else { $d }
        $momentTick = if ($check.settled) { $curTick } else { $preNudgeTick }

        $ownerXuid = if ($srcDiag.ownerXuid) { "$($srcDiag.ownerXuid)" } else { '' }
        $ownerEnt = ''
        if ($ownerXuid -and $ownerXuid -ne '0' -and $ownerXuid -ne $steam) {
            $ownerEnt = Resolve-OwnerEntityId $ownerXuid $steam
        }

        $pName = if ($t.PSObject.Properties.Match('playerName').Count -gt 0) { "$($t.playerName)" } else { '' }
        $pTeam = if ($t.PSObject.Properties.Match('team').Count -gt 0) { $t.team } else { $null }
        $found.Add((New-RawMoment $srcDiag $momentTick $anim "targeted player steam $steam at tick $momentTick (probe $tick)" $hint $pName $pTeam $check.settled $ownerEnt))
        $settleTag = if ($check.settled) { '' } else { '~unsettled' }
        Write-Host " def$def$settleTag" -NoNewline -ForegroundColor Green
        Add-Content -LiteralPath $enumFile -Value "  steam=$steam def=$def anim=$anim settled=$($check.settled) tick=$momentTick ok"
    }
    Write-Host ""
    return @($found)
}

$planFile = Join-Path $outAbs 'plan.json'
if (-not $SkipOfflinePlan) {
    & python (Join-Path $automationRoot 'tools\scan_demo_weapon_moments.py') $demoPath --out $planFile --max-probe-ticks $MaxProbeTicks
    if ($LASTEXITCODE -ne 0) { throw 'offline plan failed' }
}
$plan = Get-Content -LiteralPath $planFile -Raw | ConvertFrom-Json
$probeTicks = @($plan.probeTicks | ForEach-Object { [int]$_ } | Sort-Object)
if ($probeTicks.Count -eq 0) { $probeTicks = @(4000, 12000) }

$doTransitions = $ScanTransitions -or ($Thorough -and -not $Fast)

try {
    $mode = if ($Fast) { 'FAST' } else { 'THOROUGH' }
    Write-Host ""
    Write-Host "=== SCAN ($mode) - $MaxProbeTicks ticks max, stop after $StopAfterUniqueWeapons weapons ===" -ForegroundColor Cyan
    Write-Host "    No skin changes. Demo seeks are the slow part." -ForegroundColor DarkGray
    Write-Host ""

    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Get-Process -Name cs2,hlae -EA SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds $(if ($Fast) { 6 } else { 10 }) -OutDir (Join-Path $outAbs 'launch')
    }

    NC @('mvm_debug start') 1.2 'mvm' | Out-Null
    NC @("playdemo `"$demoPlay`"") $(if ($Fast) { 7.0 } else { 10.0 }) 'playdemo' | Out-Null
    Start-Sleep -Seconds $(if ($Fast) { 1 } else { 2 })
    NC @('demo_pause', 'mirv_filmmaker follow stop', 'mirv_filmmaker follow clear', 'mirv_filmmaker cosmetics ticknudge off') 1.0 'setup' | Out-Null

    # Cold-start warm-up: the FIRST demo_gototick after playdemo is unreliable --
    # confirmed live in the verify script, it can land 100+ ticks past the
    # requested target and get stuck there. The SECOND seek in a session lands
    # exactly on target. Absorb the cold-start penalty with one cheap,
    # disposable seek before the real probe-tick loop starts.
    Seek-Tick 500

    $scanTargets = @($plan.scanTargets)
    if ($scanTargets.Count -eq 0) {
        Write-Host "WARN: plan has no scanTargets - offline plan may be stale; re-run without -SkipOfflinePlan" -ForegroundColor Yellow
    }

    $allMoments = [System.Collections.Generic.List[object]]::new()
    $uniqueDefs = @{}
    $lastTick = -1

    foreach ($tick in $probeTicks) {
        if (-not (Test-Netcon $Port)) { throw "CS2 died at tick $tick" }
        if ($uniqueDefs.Count -ge $StopAfterUniqueWeapons) {
            Write-Host "Enough weapon types ($($uniqueDefs.Count)) - stopping early." -ForegroundColor Green
            break
        }

        Seek-Tick $tick
        $lastTick = $tick
        if (-not (Test-Cs2Netcon $Port)) {
            Export-CrashVehReport -Tag "seek_$tick" -OutDir $outAbs -Port $Port | Out-Null
            throw "CS2 died at tick $tick"
        }
        foreach ($m in (Find-GunHoldersAtTick $tick $scanTargets)) {
            $allMoments.Add($m)
            $uniqueDefs[[string]$m.weaponDefIndex] = $true
        }

        if ($doTransitions -and $allMoments.Count -gt 0) {
            $steam = $allMoments[-1].playerSteamId
            $tName = $allMoments[-1].playerName
            $tTeam = $allMoments[-1].team
            $prevDef = [int]$allMoments[-1].weaponDefIndex
            for ($d = $TransitionStep; $d -le $TransitionWindowTicks; $d += $TransitionStep) {
                NC @("mirv_skip tick $TransitionStep") $(if ($Fast) { 1.0 } else { 2.0 }) "skip" | Out-Null
                Start-Sleep -Milliseconds 200
                $diag = Read-Diag
                if ($diag.steam -ne $steam) { continue }
                $def = if ($diag.defIndex) { [int]$diag.defIndex } else { 0 }
                if ((Get-SkipReason $def)) { continue }
                if ($def -ne $prevDef -and $def -gt 0) {
                    # Just switched -- model may not have fully appeared yet; keep the
                    # moment (it's useful for weapon-type coverage / discovery) but mark
                    # settled=false so selection deprioritizes it vs a stable idle read.
                    $allMoments.Add((New-RawMoment $diag ($tick + $d) 'weapon_switch' "switched to def $def" 'unknown' $tName $tTeam $false))
                    $uniqueDefs["$def"] = $true
                    $prevDef = $def
                }
            }
            Seek-Tick $tick
        }
    }

    Write-Host "Raw moments: $($allMoments.Count), unique defs: $($uniqueDefs.Count)" -ForegroundColor Cyan

    $scanRaw = [ordered]@{
        schemaVersion = 1
        scannedAt = (Get-Date -Format o)
        fastMode = [bool]$Fast
        demo = $demoPlay
        demoName = $script:demoName
        map = $plan.map
        probeTicks = $probeTicks
        moments = @($allMoments)
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($scanRawFile, ($scanRaw | ConvertTo-Json -Depth 6), $utf8NoBom)

    & python (Join-Path $automationRoot 'tools\enrich_scan_moments.py') $scanRawFile
    $buildArgs = @($scanRawFile, '--out-dir', $outAbs)
    if ($Thorough) { $buildArgs += @('--max-total', '48') }
    else { $buildArgs += @('--max-total', '24') }
    & python (Join-Path $automationRoot 'tools\build_weapon_moment_run.py') @buildArgs

    $mj = Get-Content (Join-Path $outAbs 'moments.json') -Raw | ConvertFrom-Json
    Write-Host "`nDone: $($mj.selectedMomentCount) moments -> verify step" -ForegroundColor Green
    Write-Host "  $(Join-Path $outAbs 'verification_report.txt')" -ForegroundColor DarkGray
    if ($LeaveCs2Open) { Write-Host "  CS2 left open for verify (-NoLaunch)" -ForegroundColor DarkGray }
    exit 0
}
catch {
    Add-Content -LiteralPath $logFile -Value "ERROR $($_.Exception.Message)"
    Export-CrashVehReport -Tag 'scan_fail' -OutDir $outAbs -Port $Port | Out-Null
    Write-Host "SCAN FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
