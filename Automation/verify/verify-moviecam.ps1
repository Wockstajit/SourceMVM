#requires -Version 5
<#
============================================================
  Movie-cam / director HUD - verifier
============================================================
  Drives the live build over the CS2 netcon REPL to verify:
    * Free cam moves while the demo is PAUSED, and the demo
      tick does NOT advance (Feature 1).
    * The native Panorama director HUD panel shows/hides and
      reports the right mode/pause state (Feature 2).

  It reads the director HUD's own state JSON (camera pos + demo
  tick) via 'mirv_filmmaker hud_eval', so the only manual part
  is physically moving the camera during a short wait window -
  the assertions (pos changed / tick frozen) are automated.

  PREREQ:
    1. Launch with Automation\verify\test-filmmaker.ps1 (CS2 windowed, -netconport 29010).
    2. Load a demo (Watch -> Downloaded -> Watch) and let it play.
    3. Run:  pwsh Automation\verify\verify-moviecam.ps1
============================================================
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$OutDir,
    [double]$MoveSeconds = 5.0
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$here = $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-moviecam'
} else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-moviecam' -Additional @{ port = $Port } | Out-Null

$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if (-not $cs2) { Write-Host '[FAIL] cs2.exe is not running. Launch with test-filmmaker.ps1 first.' -ForegroundColor Red; exit 1 }

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] No netcon on 127.0.0.1:$Port. Add -netconport $Port to CS2 launch options." -ForegroundColor Red; exit 1 }
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
function Send([string]$cmd, [double]$sec = 1.0) {
    $stream.Write(($enc.GetBytes($cmd + "`n")),0,($cmd.Length+1)); $stream.Flush()
    return Drain $sec
}

# Read the director HUD state JSON (set on #MovieHudRoot by C++) from the HUD JS context.
$readStateJs = "var p=`$('#MovieHudRoot');`$.Msg('[MV] '+(p?p.GetAttributeString('state','none'):'nopanel')+'\n');"
function ReadState() {
    $txt = Send ('mirv_filmmaker hud_eval "' + $readStateJs + '"') 1.4
    $m = [regex]::Match($txt, '\[MV\]\s+(\{.*\})')
    if (-not $m.Success) { return $null }
    try { return ($m.Groups[1].Value | ConvertFrom-Json) } catch { return $null }
}
function ReadVisible() {
    $js = "var p=`$('#MovieHudRoot');`$.Msg('[MVVIS] '+(p&&p.visible?1:0)+'\n');"
    $txt = Send ('mirv_filmmaker hud_eval "' + $js + '"') 1.4
    $m = [regex]::Match($txt, '\[MVVIS\]\s+(\d)')
    if ($m.Success) { return ($m.Groups[1].Value -eq '1') } else { return $null }
}
function Shot([string]$name) {
    $out = Join-Path $OutDir $name
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-main-monitor.ps1') -Out $out 2>$null | Out-Null
    if (Test-Path $out) { Write-Host "      shot -> $out" -ForegroundColor DarkGray }
}
$results = New-Object System.Collections.Generic.List[string]
function Check([string]$label, [bool]$cond) {
    if ($cond) { Write-Host "[ PASS ] $label" -ForegroundColor Green; $results.Add("PASS $label") }
    else { Write-Host "[ FAIL ] $label" -ForegroundColor Red; $results.Add("FAIL $label") }
}

Write-Host '=== Movie-cam verifier ===' -ForegroundColor Cyan
Drain 0.5 | Out-Null

# Setup: show HUD, enter free cam.
Send 'mirv_filmmaker hud on' 0.8 | Out-Null
Send 'mirv_input camera 1' 0.8 | Out-Null
Start-Sleep -Milliseconds 500
$s0 = ReadState
if (-not $s0) {
    Write-Host '[FAIL] Could not read director HUD state. Is a demo loaded and the HUD built? (mirv_filmmaker hud on while in a demo)' -ForegroundColor Red
    $client.Close(); exit 1
}
Check 'Director HUD reports a demo is playing' ([bool]$s0.playing)
Check 'Free cam is enabled' ([bool]$s0.freecam)
Shot '1_freecam_playing.png'

# Pause and capture the frozen baseline.
Write-Host "`n--- Pause + free-cam-while-paused proof ---" -ForegroundColor Cyan
Send 'demo_pause' 0.8 | Out-Null
Start-Sleep -Milliseconds 600
$p0 = ReadState
Check 'Demo is paused' ([bool]$p0.paused)
Shot '2_paused_before_move.png'

Write-Host "`n>>> MOVE THE CAMERA NOW with WASD + mouse for ~$MoveSeconds seconds (demo is paused) <<<" -ForegroundColor Yellow
Start-Sleep -Seconds $MoveSeconds
$p1 = ReadState
Shot '3_paused_after_move.png'

$moved = $false
if ($p0 -and $p1) {
    $d = [math]::Abs($p1.ox-$p0.ox) + [math]::Abs($p1.oy-$p0.oy) + [math]::Abs($p1.oz-$p0.oz) `
       + [math]::Abs($p1.pitch-$p0.pitch) + [math]::Abs($p1.yaw-$p0.yaw)
    $moved = $d -gt 0.5
    Write-Host ("      pos/ang delta = {0:N2}; tick {1} -> {2}" -f $d, $p0.tick, $p1.tick)
}
Check 'Free cam MOVED while paused (pos/angles changed)' $moved
Check 'Demo tick did NOT advance while paused' ($p0.tick -eq $p1.tick)
Check 'Still paused after moving (not unpaused)' ([bool]$p1.paused)
if (-not $moved) { Write-Host '      (If this failed: did you actually move WASD/mouse during the wait?)' -ForegroundColor DarkYellow }

# Speed control while paused (manual shift+scroll), then re-read.
Write-Host "`n>>> Now SHIFT+SCROLL to change cam speed (still paused), ~3s <<<" -ForegroundColor Yellow
Start-Sleep -Seconds 3
$p2 = ReadState
Check 'Free-cam speed control worked while paused' ($p2 -and ($p2.speed -ne $p0.speed))
Write-Host ("      speed {0} -> {1}" -f $p0.speed, $p2.speed)

# HUD show/hide.
Write-Host "`n--- HUD show/hide ---" -ForegroundColor Cyan
Send 'mirv_filmmaker hud off' 0.6 | Out-Null; Start-Sleep -Milliseconds 400
$visOff = ReadVisible
Send 'mirv_filmmaker hud on' 0.6 | Out-Null; Start-Sleep -Milliseconds 400
$visOn = ReadVisible
Check 'HUD hides on "hud off"' ($visOff -eq $false)
Check 'HUD shows on "hud on"' ($visOn -eq $true)

# Mode cycle (manual scroll), verify mode string changes.
Write-Host "`n>>> Scroll DOWN to leave free cam (Free -> Third -> Default), ~3s <<<" -ForegroundColor Yellow
Start-Sleep -Seconds 3
$m1 = ReadState
Check 'Camera mode changed via scroll' ($m1 -and ($m1.mode -ne 'Free cam'))
Write-Host ("      mode now: {0}; freecam={1}" -f $m1.mode, $m1.freecam)

$client.Close()

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
Write-Host "Screenshots in: $OutDir"
Write-Host "`nManual checks to eyeball: LMB/RMB next/prev player, X = X-ray (free cam OFF)," -ForegroundColor DarkGray
Write-Host "G = cursor/UI, panel placement (middle-right), and repeat at a larger resolution." -ForegroundColor DarkGray
$fail = ($results | Where-Object { $_ -like 'FAIL*' }).Count
if ($fail -eq 0) { Write-Host "`nALL AUTOMATED CHECKS PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`n$fail CHECK(S) FAILED" -ForegroundColor Red; exit 1 }




