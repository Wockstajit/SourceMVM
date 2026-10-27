#requires -Version 5
<#
Automated live verifier for Follow / Lock-On Camera.

Prerequisites:
  1. Build and launch the staging release with -netconport 29010.
  2. Load and play or pause any demo with at least one player entity.
  3. Run: pwsh Automation\verify\verify-followcam.ps1
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-followcam'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-followcam' -Additional @{ port = $Port } | Out-Null
$logPath = Join-Path $OutDir 'verification.log'
$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath $logPath -Force | Out-Null
    $transcriptStarted = $true
} catch {
    Write-Warning "Could not start transcript: $($_.Exception.Message)"
}

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] No CS2 netcon on 127.0.0.1:$Port." -ForegroundColor Red; exit 1 }
$client.NoDelay = $true
$stream = $client.GetStream()
$encoding = [System.Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $builder = New-Object System.Text.StringBuilder
    $buffer = New-Object byte[] 16384
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) { [void]$builder.Append($encoding.GetString($buffer, 0, $read)) }
        } else { Start-Sleep -Milliseconds 30 }
    }
    $builder.ToString()
}

function Send([string]$command, [double]$seconds = 0.7) {
    Drain 0.08 | Out-Null
    $bytes = $encoding.GetBytes($command + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Drain $seconds
}

function Read-EncodedState([string]$command, [string]$marker) {
    $text = Send $command 1.3
    $escapedMarker = [regex]::Escape($marker)
    $matches = [regex]::Matches($text, "$escapedMarker\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)")
    if ($matches.Count -eq 0) { return $null }
    $parts = @{}
    $expected = 0
    $complete = New-Object System.Collections.Generic.List[string]
    foreach ($match in $matches) {
        $index = [int]$match.Groups[1].Value
        $total = [int]$match.Groups[2].Value
        if ($index -eq 1) {
            $parts = @{}
            $expected = $total
        }
        $parts[$index] = $match.Groups[3].Value
        if ($expected -gt 0 -and $index -eq $expected -and $parts.Count -eq $expected) {
            $complete.Add(((1..$expected | ForEach-Object { $parts[$_] }) -join ''))
        }
    }
    if ($complete.Count -eq 0) { return $null }
    $encoded = $complete[$complete.Count - 1]
    try {
        $json = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded))
        return ($json | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Read-State { Read-EncodedState 'mirv_filmmaker follow state64' '[followcam][state64]' }
function Read-EditorState { Read-EncodedState 'mirv_filmmaker editor state64' '[cameraeditor][state64]' }
$results = New-Object System.Collections.Generic.List[string]
function Check([string]$name, [bool]$condition) {
    if ($condition) {
        Write-Host "[ PASS ] $name" -ForegroundColor Green
        $results.Add("PASS $name")
    } else {
        Write-Host "[ FAIL ] $name" -ForegroundColor Red
        $results.Add("FAIL $name")
    }
}

Write-Host '=== Follow / Lock-On Camera verifier ===' -ForegroundColor Cyan
Drain 0.3 | Out-Null
Send 'demo_pause' 0.5 | Out-Null
Send 'mirv_filmmaker editor on' | Out-Null
$editorReady = $null
for ($attempt = 0; $attempt -lt 8; ++$attempt) {
    $editorReady = Read-EditorState
    if ($editorReady -and $editorReady.enabled -and $editorReady.graphExp) { break }
    Start-Sleep -Milliseconds 250
}
Send 'mirv_filmmaker follow stop' | Out-Null
Send 'mirv_filmmaker follow debug 1' | Out-Null
Send 'mirv_filmmaker follow place' | Out-Null
Send 'mirv_filmmaker follow place' | Out-Null
Send 'mirv_filmmaker follow type player' | Out-Null
$selectOutput = Send 'mirv_filmmaker follow nearest' 1.0
$placed = Read-State

Check 'Camera pose was placed' ($placed -and $placed.hasCamera)
Check 'Follow Camera is explicitly single-camera state' ($placed -and $placed.singleCamera)
Check 'Nearest player was selected' ($placed -and $placed.targetIndex -ge 0)
Check 'Selected player provider is valid' ($placed -and $placed.targetValid)

Send 'mirv_filmmaker camtl cursor off' | Out-Null
$gameMouse = Read-EditorState
Check 'G/game-mouse mode releases UI cursor' ($gameMouse -and -not $gameMouse.cursor)
Check 'G/game-mouse mode releases graph camera drive' ($gameMouse -and -not $gameMouse.graphDrive)
Send 'mirv_filmmaker camtl cursor on' | Out-Null
$uiMouse = Read-EditorState
Check 'UI mouse mode restores cursor' ($uiMouse -and $uiMouse.cursor)
# Handing the cursor back never re-arms the graph's camera drive (CameraEditor_SetCursorMode).
Check 'UI mouse mode leaves graph drive off' ($uiMouse -and -not $uiMouse.graphDrive)

Send 'mirv_filmmaker follow reposition' | Out-Null
$reposition = Read-State
$repositionEditor = Read-EditorState
Check 'Follow reposition enters placement mode' ($reposition -and $reposition.repositioning)
Check 'Follow reposition automatically returns mouse to freecam' ($repositionEditor -and -not $repositionEditor.cursor -and -not $repositionEditor.graphDrive)
Send 'mirv_filmmaker follow repositioncancel' | Out-Null
$afterCancel = Read-State
$afterCancelEditor = Read-EditorState
Check 'Reposition cancel exits placement mode' ($afterCancel -and -not $afterCancel.repositioning)
Check 'Reposition cancel restores normal editor mouse' ($afterCancelEditor -and $afterCancelEditor.cursor -and -not $afterCancelEditor.graphDrive)

$previewOutput = Send 'mirv_filmmaker follow preview' 0.8
$telemetry = Drain 1.5
$running = Read-State
Check 'Preview entered active state' ($running -and $running.enabled)
Check 'Per-frame camera telemetry was emitted' (($previewOutput + $telemetry) -match '\[followcam\]\[tick\]')

Send 'mirv_filmmaker follow stop' | Out-Null
$stopped = Read-State
Check 'Stop released follow-camera ownership' ($stopped -and -not $stopped.enabled)

$capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
if (Test-Path $capture) {
    $pathShot = Join-Path $OutDir 'path-camera-ui.png'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out $pathShot | Out-Null
    Send "mirv_filmmaker editor eval `"`$.CamEditor && `$.CamEditor.setInspectorMode('follow');`"" | Out-Null
    Send 'mirv_filmmaker follow type attachment' | Out-Null
    Send 'mirv_filmmaker follow nearest' | Out-Null
    Start-Sleep -Milliseconds 500
    Send "mirv_filmmaker editor eval `"`$.CamEditor && `$.CamEditor.setAttachmentMenuOpen(true);`"" | Out-Null
    Start-Sleep -Milliseconds 300
    $followShot = Join-Path $OutDir 'follow-camera-ui.png'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out $followShot | Out-Null
    Check 'Path Camera full-monitor screenshot was captured' (Test-Path $pathShot)
    Check 'Follow Camera full-monitor screenshot was captured' (Test-Path $followShot)
}

$client.Close()
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
$failures = @($results | Where-Object { $_ -like 'FAIL*' }).Count
if ($failures -eq 0) {
    Write-Host "`nALL FOLLOW-CAMERA CHECKS PASSED" -ForegroundColor Green
    Write-Host "Artifacts: $OutDir"
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    exit 0
}
Write-Host "`n$failures CHECK(S) FAILED" -ForegroundColor Red
Write-Host "Artifacts: $OutDir"
if ($transcriptStarted) { Stop-Transcript | Out-Null }
exit 1




