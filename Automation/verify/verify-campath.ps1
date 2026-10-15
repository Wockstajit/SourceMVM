#requires -Version 5
<#
============================================================
  Camera-path / dolly marker system - automated verifier
============================================================
  Drives the live camera-marker system over the CS2 netcon REPL
  (mirv_filmmaker marker ...) and asserts the redesigned playback
  + speed model WITHOUT pixel-clicking. It reads the deterministic
  console output the C++ now emits:

    [campath] PLAY: <n> markers, speedMode=<m>, interp=<i>,
              timing=<t>, duration=<secs>s.
    [campath] PLAY refused: need >=2 markers (have <n>).
    [campath] <n> marker(s), selected #<i>, speed=..., ...

  The `duration=<secs>s` figure is the synthetic-axis length the
  speed mode produced, so we can verify each mode's rule by reading
  it back and STOPPING immediately (no need to watch the fly-through).

  What it checks:
    * Gating: 0/1 markers refuse to play; 2 markers play (min = 2).
    * Delete marker #1: the path still plays from the survivors.
    * Manual:   duration tracks the demo-tick spacing (no multiplier).
    * Constant: one global speed; constspeed 0.2 ~= 5x the 1.0 duration.
    * Per-Seg:  one segment's multiplier changes total duration; and
                Manual duration is UNAFFECTED by constspeed/speedmul.
    * Path mode (Linear/Bezier) is independent of speed (duration
      unchanged by interp; Bezier<4 markers logs a Linear fallback).

  PREREQ:
    1. Rebuild (build.bat) and relaunch with netcon:
         powershell -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-netcon.ps1 -Port 29010
    2. Load a demo and let it PLAY (Watch -> Downloaded -> a demo, or
       `playdemo <name>` in the console). A demo must be playing.
    3. Run:  pwsh Automation\verify\verify-campath.ps1
============================================================
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    # Demo ticks to drop markers at. Spread wide so the spectated player
    # actually moves between them (Constant mode needs non-zero chords).
    [int[]]$Ticks = @(2000, 6000, 10000, 14000),
    [double]$ReadSeconds = 1.1,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-campath'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-campath' -Additional @{ port = $Port; ticks = $Ticks } | Out-Null
$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath (Join-Path $OutDir 'verification.log') -Force | Out-Null
    $transcriptStarted = $true
} catch {
    Write-Warning "Could not start transcript: $($_.Exception.Message)"
}

# --- netcon plumbing (mirror of cs2-netcon.ps1, but returns the text) --------
$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if (-not $cs2) { Write-Host '[FAIL] cs2.exe is not running. Launch with launch-cs2-netcon.ps1 first.' -ForegroundColor Red; exit 1 }

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] No netcon on 127.0.0.1:$Port. Relaunch with -netconport $Port (launch-cs2-netcon.ps1)." -ForegroundColor Red; exit 1 }
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) { $n = $stream.Read($buf,0,$buf.Length); if ($n -gt 0) { [void]$sb.Append($enc.GetString($buf,0,$n)) } }
        else { Start-Sleep -Milliseconds 40 }
    }
    return $sb.ToString()
}
function Send([string]$cmd, [double]$sec = $ReadSeconds) {
    $stream.Write(($enc.GetBytes($cmd + "`n")),0,($cmd.Length+1)); $stream.Flush()
    return Drain $sec
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$label, [bool]$cond, [string]$detail = '') {
    if ($cond) { Write-Host "[ PASS ] $label" -ForegroundColor Green; $results.Add("PASS $label") }
    else { Write-Host "[ FAIL ] $label  $detail" -ForegroundColor Red; $results.Add("FAIL $label") }
}

# --- domain helpers ----------------------------------------------------------
# Place a marker at each requested demo tick, then set deterministic positions.
# The live spectator camera can be static at seek time, which makes Constant mode's
# chord-length duration collapse to ~0 and turns this into a demo-dependent test.
function PlaceAtTicks([int[]]$ticks) {
    Send 'mirv_input camera 0' 0.5 | Out-Null
    $i = 0
    foreach ($t in $ticks) {
        Send "demo_gototick $t" 0.9 | Out-Null  # let the seek settle before reading the tick
        Send 'mirv_filmmaker marker place' 0.6 | Out-Null
        Send "mirv_filmmaker camtl setval $i 0 $($i * 300)" 0.25 | Out-Null
        Send "mirv_filmmaker camtl setval $i 1 $($i * 80)" 0.25 | Out-Null
        Send "mirv_filmmaker camtl setval $i 2 128" 0.25 | Out-Null
        $i++
    }
}
function MarkerCount() {
    $txt = Send 'mirv_filmmaker marker list' 0.8
    $m = [regex]::Match($txt, '\[campath\]\s+(\d+)\s+marker')
    if ($m.Success) { return [int]$m.Groups[1].Value } else { return -1 }
}
function ClearAll() { Send 'mirv_filmmaker marker deleteall confirm' 0.6 | Out-Null }

# Set a speed mode, play, read the reported synthetic duration, then STOP.
# Returns the duration in seconds (or -1 if the path refused / wasn't reported).
function PlayDuration([string]$speedMode) {
    Send "mirv_filmmaker marker speedmode $speedMode" 0.5 | Out-Null
    $txt = Send 'mirv_filmmaker marker play' 1.2
    Send 'mirv_filmmaker marker previewstop' 0.4 | Out-Null
    $m = [regex]::Match($txt, 'PLAY:\s+\d+\s+markers.*?duration=([\d.]+)s')
    if ($m.Success) { return [double]$m.Groups[1].Value } else { return -1.0 }
}
function PlayRaw() { $t = Send 'mirv_filmmaker marker play' 1.2; Send 'mirv_filmmaker marker previewstop' 0.4 | Out-Null; return $t }

Write-Host '=== Camera-path verifier ===' -ForegroundColor Cyan
Drain 0.6 | Out-Null

# Confirm a demo is actually playing (markers/ticks are meaningless otherwise).
$status = Send 'demo_info' 0.6
$playing = (Send 'mirv_filmmaker marker list' 0.6) -match '\[campath\]'
if (-not $playing) {
    Write-Host '[FAIL] No campath response. Is a demo loaded and playing? (Watch a demo, then re-run.)' -ForegroundColor Red
    $client.Close(); exit 1
}

# Keep playback from running the demo underneath us; duration is timing-independent.
Send 'mirv_filmmaker marker timing freeze' 0.4 | Out-Null
Send 'mirv_filmmaker marker interp linear' 0.4 | Out-Null

# --- 1) Minimum-marker gating -----------------------------------------------
Write-Host "`n--- Minimum-marker rule (>=2) ---" -ForegroundColor Cyan
ClearAll
Check '0 markers cleared' ((MarkerCount) -eq 0)
$out0 = PlayRaw
Check '0 markers refuses to play' ($out0 -match 'refused: need >=2')

PlaceAtTicks @($Ticks[0])
Check '1 marker placed' ((MarkerCount) -eq 1)
$out1 = PlayRaw
Check '1 marker refuses to play (need >=2)' ($out1 -match 'refused: need >=2')

PlaceAtTicks @($Ticks[1])
Check '2 markers placed' ((MarkerCount) -eq 2)
$out2 = PlayRaw
Check '2 markers PLAY (minimum satisfied)' ($out2 -match 'PLAY:\s+2 markers')

# --- 2) Delete marker #1, path still plays ----------------------------------
Write-Host "`n--- Delete marker #1 then replay ---" -ForegroundColor Cyan
ClearAll
PlaceAtTicks $Ticks                      # 4 markers
$before = MarkerCount
Send 'mirv_filmmaker marker delete 0' 0.6 | Out-Null   # remove the ORIGINAL first marker
$after = MarkerCount
Check 'Marker #1 deleted, list renumbered' ($after -eq ($before - 1) -and $after -ge 2)
$outD = PlayRaw
Check 'Path still plays after deleting marker #1' ($outD -match 'PLAY:\s+\d+ markers' -and $outD -notmatch 'refused')

# --- 3) Speed-mode behaviour (read back duration) ---------------------------
Write-Host "`n--- Speed model (Manual / Constant / Per-Segment) ---" -ForegroundColor Cyan
ClearAll
PlaceAtTicks $Ticks                      # fresh 4-marker path

$dManual = PlayDuration 'manual'
Write-Host ("      Manual duration            = {0:N2}s" -f $dManual)
Check 'Manual reports a positive duration' ($dManual -gt 0)

Send 'mirv_filmmaker marker constspeed 1.0' 0.4 | Out-Null
$dConst1 = PlayDuration 'constant'
Send 'mirv_filmmaker marker constspeed 0.2' 0.4 | Out-Null
$dConst2 = PlayDuration 'constant'
$ratio = if ($dConst1 -gt 0) { $dConst2 / $dConst1 } else { 0 }
Write-Host ("      Constant 1.0 = {0:N2}s  ->  0.2 = {1:N2}s   (ratio {2:N2}, expect ~5)" -f $dConst1, $dConst2, $ratio)
Check 'Constant: 0.2x is ~5x slower than 1.0x (one global speed)' ($ratio -gt 4.0 -and $ratio -lt 6.0)
Send 'mirv_filmmaker marker constspeed 1.0' 0.4 | Out-Null

# Manual must IGNORE constspeed/speedmul (timing comes only from tick spacing).
$dManual2 = PlayDuration 'manual'
Check 'Manual duration unaffected by constspeed (independent of multiplier)' ([math]::Abs($dManual2 - $dManual) -lt 0.05) ("$dManual vs $dManual2")

# Per-Segment: slow ONLY the first segment (marker #0's outgoing) and confirm the
# total grows, while the rest stay put.
Send 'mirv_filmmaker marker speedmode persegment' 0.4 | Out-Null
Send 'mirv_filmmaker marker select 0' 0.4 | Out-Null
Send 'mirv_filmmaker marker speedmul 1.0' 0.4 | Out-Null
$dSegFast = PlayDuration 'persegment'
Send 'mirv_filmmaker marker select 0' 0.4 | Out-Null
Send 'mirv_filmmaker marker speedmul 0.2' 0.4 | Out-Null
$dSegSlow = PlayDuration 'persegment'
Write-Host ("      Per-Seg all-1.0 = {0:N2}s  ->  seg0 @0.2 = {1:N2}s" -f $dSegFast, $dSegSlow)
Check 'Per-Segment: slowing one segment lengthens the path' ($dSegSlow -gt ($dSegFast + 1.0))

# Last marker has no outgoing segment -> speedmul there is refused.
$lastIdx = (MarkerCount) - 1
Send "mirv_filmmaker marker select $lastIdx" 0.4 | Out-Null
$outLast = Send 'mirv_filmmaker marker speedmul 0.2' 0.5
Check 'Last marker has no outgoing segment (speedmul refused)' ($outLast -match 'no outgoing segment')

# --- 4) Path mode is independent of speed -----------------------------------
Write-Host "`n--- Path mode (Linear/Bezier) independent of speed ---" -ForegroundColor Cyan
Send 'mirv_filmmaker marker speedmode constant' 0.4 | Out-Null
Send 'mirv_filmmaker marker constspeed 1.0' 0.4 | Out-Null
Send 'mirv_filmmaker marker interp linear' 0.4 | Out-Null
$dLin = PlayDuration 'constant'
Send 'mirv_filmmaker marker interp bezier' 0.4 | Out-Null
$dBez = PlayDuration 'constant'
Write-Host ("      Linear = {0:N2}s   Bezier = {1:N2}s   (should match: same speed)" -f $dLin, $dBez)
Check 'Interp does not change duration (path shape != speed)' ([math]::Abs($dLin - $dBez) -lt 0.05)
Send 'mirv_filmmaker marker interp linear' 0.4 | Out-Null

# Bezier with <4 markers logs a Linear fallback (cubic needs >=4 keyframes).
ClearAll
PlaceAtTicks @($Ticks[0], $Ticks[1], $Ticks[2])   # 3 markers
$outBz = Send 'mirv_filmmaker marker interp bezier' 0.6
Check 'Bezier<4 markers falls back to Linear (logged)' ($outBz -match 'Bezier(/Smooth)? needs >=4|Smooth needs >=4')
Send 'mirv_filmmaker marker interp linear' 0.4 | Out-Null

$client.Close()

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { if ($_ -like 'PASS*') { Write-Host "  $_" -ForegroundColor Green } else { Write-Host "  $_" -ForegroundColor Red } }
Write-Host "`nManual eyeball pass (in-game, free cam): K places, J flies through ALL markers as" -ForegroundColor DarkGray
Write-Host "one smooth path (no snapping/restart), X stops, Tab toggles HUD, F opens the menu," -ForegroundColor DarkGray
Write-Host "L deletes the aimed marker. With <2 markers, J shows the on-screen 'need 2' banner." -ForegroundColor DarkGray
$fail = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "Artifacts: $OutDir"
if ($transcriptStarted) { Stop-Transcript | Out-Null }
if ($fail -eq 0) { Write-Host "`nALL AUTOMATED CHECKS PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`n$fail CHECK(S) FAILED" -ForegroundColor Red; exit 1 }




