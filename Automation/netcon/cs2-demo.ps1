# Drive a running CS2 over netcon with VERIFIED, BOUNDED steps. Every step reads the game back
# and either prints what it confirmed or fails loudly -- no open-ended "waiting for health" loops.
#
#   cs2-demo.ps1 -Demo "replays\repaired\match.dem"          load a demo (retries playdemo, waits for the map)
#   cs2-demo.ps1 -Tick 112033                                 pause, seek there, confirm it holds
#   cs2-demo.ps1 -Tick 111700 -PlayTo 112033                  seek, then PLAY up to 112033 and pause. Use this
#                                                             for smokes/effects: CS2 does not rebuild them on a seek
#   cs2-demo.ps1 -Position -732.1,-540.9,-200.6 -Angles -0.1,-74.7,0
#                                                             free cam at that pose; angles are pitch,yaw,roll
#                                                             exactly as the Movie Director panel shows them
#   cs2-demo.ps1 -Status                                      one line: demo, map, tick, paused, camera
#
# Steps combine and run in this order: Demo, Tick, PlayTo, Position/Angles, Status. Exit code 0 only when
# every requested step was confirmed by reading it back from the game.
[CmdletBinding()]
param(
    [string]$Demo,
    [int]$Tick = -1,
    [int]$PlayTo = -1,
    [double[]]$Position,
    [double[]]$Angles,
    [switch]$Status,
    [int]$Port = 29010,
    [int]$DemoTimeoutSeconds = 150,
    [int]$SeekTimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$netcon = Join-Path $PSScriptRoot 'cs2-netcon.ps1'
if (-not $Demo -and $Tick -lt 0 -and $PlayTo -lt 0 -and -not $Position -and -not $Angles -and -not $Status) {
    Get-Content $PSCommandPath | Select-Object -Skip 1 -First 12 | ForEach-Object { $_ -replace '^# ?', '' }
    exit 2
}
function Send([string]$batch, [double]$read = 0.6) {
    # -Width: Out-String otherwise wraps at the console width and splits long paths mid-string.
    return ((& $netcon -Port $Port -CommandBatch $batch -ReadSeconds $read 6>&1) | Out-String -Width 8192)
}
function Leaf([string]$path) {
    $clean = ($path -replace '[
]', '').Trim()
    if (-not $clean) { return '' }
    try { return [IO.Path]::GetFileNameWithoutExtension($clean) } catch { return '' }
}
function Probe([double]$read = 0.8) {
    $r = Send 'mirv_filmmaker demoprobe' $read
    [pscustomobject]@{
        playing  = $r -match 'IsPlayingDemo=1'
        resolved = [regex]::Match($r, "(?m)^\[demoprobe\] RESOLVED = '([^']*)'").Groups[1].Value
        level    = [regex]::Match($r, "(?m)^\[demoprobe\] level = '([^']*)'").Groups[1].Value
        tick     = $(if ($r -match '(?m)^\[demoprobe\] tick=(-?\d+)') { [int]$Matches[1] } else { -1 })
        paused   = $r -match '(?m)^\[demoprobe\] tick=-?\d+ paused=1'
        raw      = $r
    }
}
function Fail([string]$m) { Write-Host "FAIL: $m" -ForegroundColor Red; exit 1 }
function Ok([string]$m)   { Write-Host "OK:   $m" -ForegroundColor Green }

if (-not (Get-Process cs2 -ErrorAction SilentlyContinue)) { Fail 'cs2.exe is not running' }
try { $null = Send 'echo cs2-demo' 0.3 } catch { Fail "netcon $Port not answering: $($_.Exception.Message)" }

if ($Demo) {
    $leaf = [IO.Path]::GetFileNameWithoutExtension($Demo)
    $arg  = ($Demo -replace '\\', '/') -replace '\.dem$', ''
    $cmd  = 'playdemo "' + $arg + '"'
    $deadline = (Get-Date).AddSeconds($DemoTimeoutSeconds)
    $sent = @()
    $p = Probe
    if ($p.playing -and ((Leaf $p.resolved) -ieq $leaf) -and $p.level) {
        Ok "demo already playing: $leaf on $($p.level)"
    } else {
        # playdemo is dropped if the menu is not ready yet, so it is re-sent while nothing has
        # started loading. Once IsPlayingDemo flips the map is loading: stop sending, just wait.
        $i = 0
        while ((Get-Date) -lt $deadline) {
            $p = Probe
            $ours = (Leaf $p.resolved) -ieq $leaf
            if ($p.playing -and $ours -and $p.level) { break }
            if (-not ($p.playing -and $ours) -and ($i % 8 -eq 0)) {
                Write-Host "  > $cmd"; $null = Send $cmd 0.4; $sent += (Get-Date).ToString('HH:mm:ss')
            } elseif ($i % 4 -eq 0) {
                Write-Host ("  loading... playing={0} level='{1}' ({2}s left)" -f [int]$p.playing, $p.level, [int]($deadline - (Get-Date)).TotalSeconds)
            }
            $i++; Start-Sleep 2
        }
        if (-not ($p.playing -and $p.level)) { Fail "demo did not load in ${DemoTimeoutSeconds}s (playdemo sent at $($sent -join ', ')). Last probe:`n$($p.raw.Trim())" }
        # The class tables settle a moment after the level name appears; seeking earlier can crash.
        Start-Sleep 3
        Ok "demo loaded: $leaf on $($p.level)"
    }
}

function EnsurePaused {
    # demo_pause is a TOGGLE, so it is only sent after reading the state, and the result is read back.
    for ($k = 0; $k -lt 3; $k++) {
        $p = Probe
        if ($p.paused) { return $p }
        $null = Send 'demo_pause' 0.3
        Start-Sleep -Milliseconds 800
    }
    $p = Probe
    if (-not $p.paused) { Fail "could not pause the demo (tick=$($p.tick))" }
    return $p
}

if ($Tick -ge 0) {
    # Pause FIRST, then seek. CS2 ignores demo_gototick's CS:GO-era "pause" argument, and a LONG
    # forward seek (demo skipping, e.g. right after a load) drops the pause and resumes playback.
    # So: seek, re-pause wherever it landed, and seek again while paused -- a short seek from a
    # paused demo holds. Up to three rounds; every round is read back.
    $held = $null
    for ($round = 1; $round -le 3 -and -not $held; $round++) {
        $null = EnsurePaused
        $null = Send "demo_gototick $Tick" 0.3
        $deadline = (Get-Date).AddSeconds($SeekTimeoutSeconds)
        $p = $null; $last = -99
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 700
            $p = Probe 0.4
            if ([math]::Abs($p.tick - $Tick) -le 2 -and $p.paused) { break }
            if (-not $p.paused -and $p.tick -ne $last -and $last -ge 0 -and [math]::Abs($last - $Tick) -le 400) { break }  # landed, but playing on
            $last = $p.tick
            Write-Host ("  seeking... tick={0} paused={1} (round {2})" -f $p.tick, [int]$p.paused, $round)
        }
        if ($p.tick -lt 0) { Fail 'the hook does not report a tick (stale DLL?) -- cannot verify the seek' }
        $p = EnsurePaused
        Start-Sleep 1
        $again = Probe
        if ([math]::Abs($again.tick - $Tick) -le 2 -and $again.paused -and $again.tick -eq $p.tick) { $held = $again }
        else { Write-Host ("  round {0}: landed tick={1} paused={2}, seeking again while paused" -f $round, $again.tick, [int]$again.paused) }
    }
    if (-not $held) { Fail "seek to $Tick not confirmed after 3 rounds: tick=$($again.tick) paused=$([int]$again.paused)" }
    Ok "paused on tick $($held.tick) (held)"
}

if ($PlayTo -ge 0) {
    # Smoke volumes, fires and decals that started before a seek target are not rebuilt by the seek,
    # so reaching a frame WITH them means playing into it. Full speed until close, then crawl, so the
    # pause lands within a few ticks of the target despite the netcon round trip.
    $p = EnsurePaused
    if ($p.tick -ge $PlayTo) { Fail "already at tick $($p.tick), past -PlayTo $PlayTo; seek earlier with -Tick" }
    # Three speeds: full until 160 ticks out, quarter until 48 out, then a crawl. Each netcon probe
    # takes ~0.3 s, so the overshoot at each switch stays inside the next stage's window.
    $deadline = (Get-Date).AddSeconds(($PlayTo - $p.tick) / 64 + 60)
    $left = $PlayTo - $p.tick
    $stage = if ($left -le 48) { 2 } elseif ($left -le 160) { 1 } else { 0 }
    $initialSpeed = if ($stage -eq 2) { 0.05 } elseif ($stage -eq 1) { 0.25 } else { 1 }
    $null = Send ('demo_timescale ' + $initialSpeed + ';;demo_resume') 0.2
    while ((Get-Date) -lt $deadline) {
        $p = Probe 0.25
        $left = $PlayTo - $p.tick
        if ($left -le 1) { break }
        if ($stage -lt 1 -and $left -le 160) { $null = Send 'demo_timescale 0.25' 0.1; $stage = 1 }
        if ($stage -lt 2 -and $left -le 48)  { $null = Send 'demo_timescale 0.05' 0.1; $stage = 2 }
        if ($p.paused) { $null = Send 'demo_resume' 0.1 }
    }
    if ((Get-Date) -ge $deadline) { $null = Send 'demo_pause;;demo_timescale 1' 0.2; Fail "play-to $PlayTo timed out at tick $($p.tick)" }
    $null = Send 'demo_pause' 0.2
    Start-Sleep -Milliseconds 500
    $p = EnsurePaused
    $null = Send 'demo_timescale 1' 0.2
    if ([math]::Abs($p.tick - $PlayTo) -gt 8) { Fail "play-to $PlayTo landed on tick $($p.tick)" }
    Ok "played to tick $($p.tick) and paused (target $PlayTo)"
}

if ($Position -or $Angles) {
    if ($Position -and $Position.Count -ne 3) { Fail '-Position needs x,y,z' }
    if ($Angles -and $Angles.Count -ne 3)     { Fail '-Angles needs pitch,yaw,roll' }
    $batch = 'mirv_input camera'
    if ($Position) { $batch += ';;mirv_input position ' + ($Position -join ' ') }
    if ($Angles)   { $batch += ';;mirv_input angles '   + ($Angles -join ' ') }
    $null = Send $batch 0.3
    Start-Sleep -Milliseconds 400
    $r = Send 'mirv_input position;;mirv_input angles' 0.6
    $vals = [regex]::Matches($r, 'Current value: (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')
    if ($vals.Count -lt 2) { Fail "could not read the camera back:`n$r" }
    $gotP = 1..3 | ForEach-Object { [double]$vals[0].Groups[$_].Value }
    $gotA = 1..3 | ForEach-Object { [double]$vals[1].Groups[$_].Value }
    $bad = @()
    if ($Position) { for ($k = 0; $k -lt 3; $k++) { if ([math]::Abs($gotP[$k] - $Position[$k]) -gt 0.05) { $bad += "position[$k] wanted $($Position[$k]) got $($gotP[$k])" } } }
    if ($Angles)   { for ($k = 0; $k -lt 3; $k++) { if ([math]::Abs($gotA[$k] - $Angles[$k])   -gt 0.05) { $bad += "angles[$k] wanted $($Angles[$k]) got $($gotA[$k])" } } }
    if ($bad) { Fail ($bad -join '; ') }
    Ok ("camera pos=({0}) ang=({1})  [pitch,yaw,roll, as the Movie Director shows]" -f ($gotP -join ', '), ($gotA -join ', '))
}

if ($Status) {
    $p = Probe
    $r = Send 'mirv_input position;;mirv_input angles' 0.6
    $vals = [regex]::Matches($r, 'Current value: (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')
    $cam = if ($vals.Count -ge 2) { "pos=($($vals[0].Groups[1].Value), $($vals[0].Groups[2].Value), $($vals[0].Groups[3].Value)) ang=($($vals[1].Groups[1].Value), $($vals[1].Groups[2].Value), $($vals[1].Groups[3].Value))" } else { 'camera unreadable' }
    Write-Host ("STATUS: demo={0} map='{1}' tick={2} paused={3} {4}" -f ((Leaf $p.resolved)), $p.level, $p.tick, [int]$p.paused, $cam)
}
exit 0
