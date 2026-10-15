# Muzzle-flash camera-alignment capture: for a given demo tick, capture the spectated
# player's muzzle flash in BOTH first person and third person, for one or more EFFECTS
# weaponfx packs (Povarehok "on" vs "modern"). Purpose: prove whether a pack's flash
# renders on the WORLD weapon when the camera detaches (third person / free cam), the way
# Povarehok's world-space flash does -- vs a viewmodel-only flash that only shows in first
# person and floats when the view leaves the eye.
#
# WHY the two camera modes use different seek techniques:
#   First person (in-eye): the engine spawns only the _fps VIEWMODEL muzzle system for the
#     spectated player. A forward micro-seek (preload tick -> target tick) replays the
#     weapon_fire packet and lands EXACTLY on the target tick with the flash alive. Cold and
#     cheap.
#   Third person: the world (non-_fps) muzzle system only spawns while the spec_mode-3 chase
#     is active, and spec_mode does NOT apply while the demo is paused (Filmmaker.cpp). So TP
#     must PLAY through the fire tick in chase, then pause at/just past the target tick.
#
# Lands first person exactly on -Tick; lands third person on the first tick >= -Tick (a
# muzzle flash particle lingers a few ticks at 0.1x, so a 1-2 tick overshoot still shows it).
#
# Output: Automation/runs/muzzleflash-cam/<timestamp>/<pack>-<fp|tp>-t<tick>.png + a
# capture-log.txt. Screenshots are the deliverable -- eyeball fp-vs-tp per pack.
param(
    [int]$Port = 29010,
    [int]$Tick = 287,
    [int]$Preroll = 17,          # ticks before -Tick to seek to, then PLAY into the tick.
                                 # A long play-in (default 270 -> 287) lets the player model,
                                 # weapon and particle assets fully stream in and lets the
                                 # spec_mode-3 chase activate, so the world muzzle flash is
                                 # reliably present -- a short micro-seek does not.
    [double]$SlowScale = 0.1,    # timescale used to play into the tick
    [string[]]$Packs = @('modern'),  # the pack under test; add 'on' for a Povarehok reference pass
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$automationRoot = Join-Path $repo 'Automation'
$netconScript = Join-Path $automationRoot 'netcon\cs2-netcon.ps1'
$captureScript = Join-Path $automationRoot 'capture\capture-game-window.ps1'
$scratchLog = Join-Path $env:TEMP "muzzleflash-cam-$PID.log"

if (-not $OutDir) {
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path $automationRoot "runs\muzzleflash-cam\$ts"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$logLines = New-Object System.Collections.Generic.List[string]
function Log([string]$m) { $logLines.Add($m); Write-Host $m }

# Send commands, return the streamed console text (cs2-netcon writes it via Write-Host, so
# capture it through -LogPath and read the file back -- see verify-modern-muzzle-alignment).
function Send([string[]]$cmds, [double]$read = 0.5) {
    & $netconScript -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $scratchLog | Out-Null
    Get-Content -Raw $scratchLog -ErrorAction SilentlyContinue
}

# demo tick <-> game tick offset: `demo_gototick N` prints "(game tick G)". offset = G - N.
function Get-GameTickOffset([int]$demoTick) {
    $o = Send @("demo_gototick $demoTick") 0.7
    if ($o -match 'demo tick \d+ \(game tick (\d+)\)') { return [int]$Matches[1] - $demoTick }
    if ($o -match 'paused on tick (\d+)') { return [int]$Matches[1] - $demoTick }
    throw "Could not parse game-tick offset from demo_gototick output."
}

# Seek to (Tick - Preroll), PLAY CONTINUOUSLY at $SlowScale into the tick (loads models/
# assets and activates the chase cam), then ONE pause timed to land on $Tick. No stepping
# loop -- repeated pause/resume visibly glitches the demo. The demo is 128-tick, so at
# $SlowScale the play-in duration is Preroll / (128 * $SlowScale) seconds; if the single
# pause lands a hair short of the fire tick, one forward micro-seek snaps to it (cheap,
# assets are already streamed in by the play-in).
function PlayStopAtTick([int]$targetGame) {
    Send @("demo_gototick $($Tick - $Preroll)", "demo_timescale $SlowScale") 0.6 | Out-Null
    Start-Sleep -Milliseconds 500
    $playMs = [int](1000.0 * $Preroll / (128.0 * $SlowScale))
    Send @('demo_resume') 0.05 | Out-Null
    Start-Sleep -Milliseconds $playMs
    $o = Send @('demo_pause') 0.2
    $landed = -1
    if ($o -match 'paused on tick (\d+)') { $landed = [int]$Matches[1] }
    if ($landed -ge 0 -and $landed -lt $targetGame) {
        Send @("demo_gototick $Tick") 0.5 | Out-Null
    }
    Start-Sleep -Milliseconds 400
}

Log "=== Muzzle-flash camera capture ==="
Log "tick=$Tick preroll=$Preroll packs=$($Packs -join ',') out=$OutDir"

# Baseline: fx on, normal speed, first person, and learn the tick offset.
Send @('mirv_filmmaker fx on quiet', 'mirv_filmmaker thirdperson off', 'demo_timescale 1') 0.4 | Out-Null
$offset = Get-GameTickOffset ($Tick - $Preroll)
$targetGameTick = $offset + $Tick
Log "game-tick offset=$offset -> target game tick=$targetGameTick"

foreach ($pack in $Packs) {
    Send @("mirv_filmmaker fx set weaponfx $pack quiet") 0.4 | Out-Null

    # ---- First person: seek to 270, play into 287, stop, screenshot ----
    Send @('mirv_filmmaker thirdperson off') 0.4 | Out-Null
    PlayStopAtTick $targetGameTick
    $fpOut = Join-Path $OutDir "$pack-fp-t$Tick.png"
    & $captureScript -Out $fpOut -Process cs2 -Mode client | Out-Null
    Log "[$pack] first person  -> $fpOut"

    # ---- Third person: go back to 270, play into 287 (chase activates while playing), stop ----
    Send @('mirv_filmmaker thirdperson on') 0.4 | Out-Null
    PlayStopAtTick $targetGameTick
    $tpOut = Join-Path $OutDir "$pack-tp-t$Tick.png"
    & $captureScript -Out $tpOut -Process cs2 -Mode client | Out-Null
    Log "[$pack] third person  -> $tpOut"
}

# Teardown: restore normal speed and first person.
Send @('demo_timescale 1', 'mirv_filmmaker thirdperson off') 0.4 | Out-Null
$logLines | Set-Content (Join-Path $OutDir 'capture-log.txt')
Log "`nDone. Screenshots in: $OutDir"
