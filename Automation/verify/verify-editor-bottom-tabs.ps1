#requires -Version 5
<#
Live verifier for the Camera Editor bottom-editor tab bar.

Proves, over CS2's -netconport console:
  * opening the editor defaults the bottom to the REGULAR (native CS2) timeline,
  * the bottom tab bar switches Camera / Graph / Regular Timeline (bottomMode in editor state),
  * exiting the editor restores the normal state.
Captures a full-monitor screenshot of each state.

Usage:
  pwsh Automation\verify\verify-editor-bottom-tabs.ps1
  ... -Demo "replays/<name-without-.dem>"   (any demo with gameplay)
  ... -NoLaunch                              (CS2 already running on -netconport)
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string[]]$Demos = @("test all hard", "replays/match730_003825531068514042101_1762908576_389", "physics", "anim"),
    [int]$LoadTimeoutSeconds = 45,
    [switch]$NoLaunch,
    [string]$OutDir
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
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
Set-StrictMode -Off
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = New-AutomationRunFolder -Name 'verify-editor-bottom-tabs' }
else { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-editor-bottom-tabs' -Additional @{ port = $Port; demos = ($Demos -join ';') } | Out-Null
$capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
$logPath = Join-Path $OutDir 'verification.log'
try { Start-Transcript -LiteralPath $logPath -Force | Out-Null; $transcript = $true } catch { $transcript = $false }

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try { $c = $t.BeginConnect('127.0.0.1', $p, $null, $null); $ok = $c.AsyncWaitHandle.WaitOne(500); if ($ok) { $t.EndConnect($c) }; return $ok }
    catch { return $false } finally { $t.Dispose() }
}

if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    Write-Host "Launching CS2 (hook + netcon $Port)..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') -Port $Port -Cs2Dir $Cs2Dir
}

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] No CS2 netcon on 127.0.0.1:$Port." -ForegroundColor Red; exit 1 }
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $b = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 16384
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) { $n = $stream.Read($buf, 0, $buf.Length); if ($n -gt 0) { [void]$b.Append($enc.GetString($buf, 0, $n)) } }
        else { Start-Sleep -Milliseconds 30 }
    }
    $b.ToString()
}
function Send([string]$cmd, [double]$seconds = 0.7) {
    Drain 0.08 | Out-Null
    $by = $enc.GetBytes($cmd + "`n")
    $stream.Write($by, 0, $by.Length); $stream.Flush()
    Drain $seconds
}
function Read-EditorState {
    $text = Send 'mirv_filmmaker editor state64' 1.3
    $m = [regex]::Matches($text, "\[cameraeditor\]\[state64\]\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)")
    if ($m.Count -eq 0) { return $null }
    $parts = @{}; $expected = 0; $complete = New-Object System.Collections.Generic.List[string]
    foreach ($x in $m) {
        $i = [int]$x.Groups[1].Value; $tot = [int]$x.Groups[2].Value
        if ($i -eq 1) { $parts = @{}; $expected = $tot }
        $parts[$i] = $x.Groups[3].Value
        if ($expected -gt 0 -and $i -eq $expected -and $parts.Count -eq $expected) { $complete.Add(((1..$expected | ForEach-Object { $parts[$_] }) -join '')) }
    }
    if ($complete.Count -eq 0) { return $null }
    try { return ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($complete[$complete.Count - 1])) | ConvertFrom-Json) } catch { return $null }
}

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class FgWin {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
'@
function Focus-Cs2 {
    $p = Get-Process -Name cs2 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { [FgWin]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; [FgWin]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 500 }
}
function Shot([string]$name) {
    Focus-Cs2
    $out = Join-Path $OutDir $name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out $out | Out-Null
    return $out
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$n, [bool]$c) {
    if ($c) { Write-Host "[ PASS ] $n" -ForegroundColor Green; $results.Add("PASS $n") }
    else { Write-Host "[ FAIL ] $n" -ForegroundColor Red; $results.Add("FAIL $n") }
}

Write-Host '=== Camera Editor bottom-tab verifier ===' -ForegroundColor Cyan
Drain 0.4 | Out-Null

# --- load a demo + wait until it is actually playing (demo time advances). Try each candidate
#     until one is compatible with the current CS2 build (older demos disconnect to the menu). ---
$loaded = $false; $usedDemo = $null
foreach ($demo in $Demos) {
    Write-Host "Loading demo: $demo" -ForegroundColor Cyan
    $out = Send "playdemo `"$demo`"" 2.5
    if ($out -match 'incompatible with this game version') {
        Write-Host "  -> rejected ($demo): incompatible with this CS2 build." -ForegroundColor Yellow
        continue
    }
    # Demo is "playing" only when the DEMO TICK actually advances (engine/menu time can tick
    # without a demo, so checking demo tick is the reliable signal).
    $lastTick = -1; $deadline = (Get-Date).AddSeconds($LoadTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Send 'demo_resume' 0.2 | Out-Null
        $s = Read-EditorState
        if ($s -and ($s.tick -gt 0)) {
            if ($lastTick -ge 0 -and ($s.tick - $lastTick) -gt 4) { $loaded = $true; break }
            $lastTick = $s.tick
        }
        Start-Sleep -Milliseconds 700
    }
    if ($loaded) { $usedDemo = $demo; break }
    Write-Host "  -> $demo never advanced demo time; trying next." -ForegroundColor Yellow
}
Check "A compatible demo loaded and is playing ($usedDemo)" $loaded
Send 'demo_pause' 0.6 | Out-Null

# --- open editor: default bottom MUST be Regular Timeline (native) ---
Send 'mirv_filmmaker editor on' 0.9 | Out-Null
Start-Sleep -Milliseconds 700
$st = Read-EditorState
Check "Editor enabled" ($st -and $st.enabled)
Check "Default bottom mode = Regular Timeline (native)" ($st -and $st.bottomMode -eq 'native')
Shot '01-regular-timeline.png' | Out-Null

# --- TIMELINE tab (KEYFRAMES view) ---
Send 'mirv_filmmaker settings ui editor.timeline.view keys' 0.3 | Out-Null
Send 'mirv_filmmaker dock timeline' 0.6 | Out-Null
Start-Sleep -Milliseconds 800
$st = Read-EditorState
Check "TIMELINE tab -> bottomMode = timeline" ($st -and $st.bottomMode -eq 'timeline')
Check "Timeline dock active on the TIMELINE tab" ($st -and $st.graphExp)
Shot '02-timeline-keyframes.png' | Out-Null

# --- TIMELINE tab, GRAPH view ---
Send 'mirv_filmmaker settings ui editor.timeline.view graph' 0.6 | Out-Null
Start-Sleep -Milliseconds 600
$st = Read-EditorState
Check "GRAPH view keeps bottomMode = timeline" ($st -and $st.bottomMode -eq 'timeline')
Shot '03-timeline-graph.png' | Out-Null
Send 'mirv_filmmaker settings ui editor.timeline.view keys' 0.3 | Out-Null

# --- back to Regular Timeline ---
Send 'mirv_filmmaker dock native' 0.6 | Out-Null
Start-Sleep -Milliseconds 600
$st = Read-EditorState
Check "Regular Timeline tab -> bottomMode = native" ($st -and $st.bottomMode -eq 'native')
Check "Timeline dock off again" ($st -and -not $st.graphExp)
Shot '04-regular-again.png' | Out-Null

# --- exit editor: normal state restored ---
Send 'mirv_filmmaker editor off' 0.6 | Out-Null
Start-Sleep -Milliseconds 700
$st = Read-EditorState
Check "Editor disabled after exit" ($st -and -not $st.enabled)
Shot '05-after-exit.png' | Out-Null

$client.Close()
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
$fail = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "Artifacts: $OutDir"
if ($transcript) { try { Stop-Transcript | Out-Null } catch {} }
if ($fail -eq 0) { Write-Host "ALL CHECKS PASSED" -ForegroundColor Green; exit 0 }
Write-Host "$fail CHECK(S) FAILED" -ForegroundColor Red
exit 1




