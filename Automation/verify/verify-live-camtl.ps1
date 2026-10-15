#requires -Version 5
<#
Verifies Camera Timeline Live playback against a running CS2/HLAE netcon session.

This intentionally checks the rendered CS2 window, not just console logs:
  1. Opens the camera editor and enables debug instrumentation.
  2. Scrubs to a start tick in the loaded sidecar camera path.
  3. Captures the actual CS2 window at start / mid / later playback.
  4. Prints console evidence and pixel-difference scores between captures.
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [int]$StartTick = 1945,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'

$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
if (-not (Test-Path $capture)) { throw "Missing capture helper: $capture" }

. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-live-camtl'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-live-camtl' -Additional @{ port = $Port; startTick = $StartTick } | Out-Null

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port)
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII
$log = New-Object System.Text.StringBuilder

function Read-Netcon([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                $txt = $enc.GetString($buf, 0, $n)
                [void]$log.Append($txt)
                Write-Host -NoNewline $txt
            }
        } else {
            Start-Sleep -Milliseconds 40
        }
    }
}

function Send-Cmd([string]$cmd, [double]$readSeconds = 1.0) {
    Write-Host "`n>>> $cmd" -ForegroundColor Cyan
    $bytes = $enc.GetBytes($cmd + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Read-Netcon $readSeconds
}

function Capture-Cached([string]$name) {
    $path = Join-Path $OutDir $name
    powershell -ExecutionPolicy Bypass -File $capture -Out $path | Write-Host
    return $path
}

Add-Type -AssemblyName System.Drawing
function Compare-Images([string]$a, [string]$b) {
    $ia = [System.Drawing.Bitmap]::new($a)
    $ib = [System.Drawing.Bitmap]::new($b)
    try {
        if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) {
            return @{ Mean = 255.0; Changed = 1.0; Samples = 0 }
        }
        $stepX = [Math]::Max(1, [int]($ia.Width / 160))
        $stepY = [Math]::Max(1, [int]($ia.Height / 90))
        $sum = 0.0
        $changed = 0
        $samples = 0
        for ($y = 0; $y -lt $ia.Height; $y += $stepY) {
            for ($x = 0; $x -lt $ia.Width; $x += $stepX) {
                $pa = $ia.GetPixel($x, $y)
                $pb = $ib.GetPixel($x, $y)
                $d = ([Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)) / 3.0
                $sum += $d
                if ($d -gt 8.0) { ++$changed }
                ++$samples
            }
        }
        return @{ Mean = $sum / [Math]::Max(1, $samples); Changed = $changed / [Math]::Max(1, $samples); Samples = $samples }
    } finally {
        $ia.Dispose()
        $ib.Dispose()
    }
}

Read-Netcon 0.5
Send-Cmd 'mirv_filmmaker editor on' 1.0
Send-Cmd 'mirv_filmmaker camtl open' 0.5
Send-Cmd 'mirv_filmmaker camtl debug 1' 0.5
Send-Cmd 'mirv_filmmaker marker timing live' 0.5
Send-Cmd 'mirv_filmmaker marker list' 0.8
Send-Cmd "mirv_filmmaker camtl scrub $StartTick" 2.0
Start-Sleep -Milliseconds 600
$start = Capture-Cached 'live-start.png'

Send-Cmd 'mirv_filmmaker camtl play' 1.0
Start-Sleep -Milliseconds 900
$mid = Capture-Cached 'live-mid.png'
Read-Netcon 0.8
Start-Sleep -Milliseconds 1100
$later = Capture-Cached 'live-later.png'
Read-Netcon 1.0
Send-Cmd 'mirv_filmmaker camtl pause' 0.8
$pause = Capture-Cached 'live-paused.png'
Read-Netcon 0.5
Send-Cmd 'mirv_filmmaker camtl debug 0' 0.3

$d1 = Compare-Images $start $mid
$d2 = Compare-Images $mid $later
$d3 = Compare-Images $later $pause

$logPath = Join-Path $OutDir 'netcon.log'
[System.IO.File]::WriteAllText($logPath, $log.ToString())

Write-Host "`n=== VISUAL DIFF ===" -ForegroundColor Cyan
Write-Host ("start -> mid   mean={0:N2} changed={1:P1}" -f $d1.Mean, $d1.Changed)
Write-Host ("mid   -> later mean={0:N2} changed={1:P1}" -f $d2.Mean, $d2.Changed)
Write-Host ("later -> pause mean={0:N2} changed={1:P1}" -f $d3.Mean, $d3.Changed)
Write-Host "captures: $OutDir"
Write-Host "log     : $logPath"

$text = $log.ToString()
$liveTickPushes = ([regex]::Matches($text, '\[campath\]\[tick\].*pushed=1')).Count
$bridgePushes = ([regex]::Matches($text, '\[campath\]\[bridge\] SetCameraPose queued')).Count
$hasLivePush = ($liveTickPushes -ge 2 -and $bridgePushes -ge 2)
$hasOwner = ([regex]::Matches($text, '\[setupview\]\[owner\].*blockDemoViewOverride=1')).Count -ge 1
$moves = ($d1.Mean -gt 2.0 -and $d2.Mean -gt 2.0)

if ($hasLivePush -and $hasOwner -and $moves) {
    Write-Host "LIVE VIEWPORT VERIFICATION PASSED" -ForegroundColor Green
    exit 0
}

Write-Host "LIVE VIEWPORT VERIFICATION FAILED" -ForegroundColor Red
Write-Host ("hasLivePush={0} liveTickPushes={1} bridgePushes={2} hasOwner={3} moves={4}" -f $hasLivePush, $liveTickPushes, $bridgePushes, $hasOwner, $moves)
exit 1




