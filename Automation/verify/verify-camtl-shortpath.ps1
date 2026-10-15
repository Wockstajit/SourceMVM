#requires -Version 5
param(
    [int]$Port = 29010,
    [int]$StartTick = 11148,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot

$capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-camtl-shortpath'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-camtl-shortpath' -Additional @{ port = $Port; startTick = $StartTick } | Out-Null

$client = [System.Net.Sockets.TcpClient]::new()
$client.Connect('127.0.0.1', $Port)
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [Text.Encoding]::ASCII
$log = [Text.StringBuilder]::new()

function Read-Net([double]$seconds) {
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
            Start-Sleep -Milliseconds 30
        }
    }
}

function Send-Cmd([string]$cmd, [double]$read = 0.5) {
    Write-Host "`n>>> $cmd" -ForegroundColor Cyan
    $bytes = $enc.GetBytes($cmd + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Read-Net $read
}

function Capture-Cached([string]$name) {
    $path = Join-Path $OutDir $name
    powershell.exe -ExecutionPolicy Bypass -File $capture -Out $path | Write-Host
    return $path
}

Add-Type -AssemblyName System.Drawing
function Compare-Images([string]$a, [string]$b) {
    $ia = [Drawing.Bitmap]::new($a)
    $ib = [Drawing.Bitmap]::new($b)
    try {
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
        return @{ Mean = $sum / [Math]::Max(1, $samples); Changed = $changed / [Math]::Max(1, $samples) }
    } finally {
        $ia.Dispose()
        $ib.Dispose()
    }
}

Read-Net 0.4
Send-Cmd 'mirv_filmmaker editor on' 1.5
Send-Cmd 'mirv_filmmaker camtl open' 0.5
Send-Cmd 'mirv_filmmaker camtl debug 1' 0.4
Send-Cmd 'mirv_filmmaker marker timing live' 0.3
Send-Cmd 'mirv_filmmaker marker list' 0.5
Send-Cmd "mirv_filmmaker camtl scrub $StartTick" 2.5
Start-Sleep -Milliseconds 200
$start = Capture-Cached 'start.png'

Send-Cmd 'mirv_filmmaker camtl play' 0.25
Start-Sleep -Milliseconds 120
$p1 = Capture-Cached 'play-120ms.png'
Read-Net 0.1
Start-Sleep -Milliseconds 160
$p2 = Capture-Cached 'play-280ms.png'
Read-Net 0.1
Start-Sleep -Milliseconds 220
$p3 = Capture-Cached 'play-500ms.png'
Read-Net 0.1
Start-Sleep -Milliseconds 900
Read-Net 1.0
$end = Capture-Cached 'end.png'

Send-Cmd 'mirv_filmmaker camtl play' 0.8
Start-Sleep -Milliseconds 500
Read-Net 1.0
Send-Cmd 'mirv_filmmaker camtl pause' 0.4
Send-Cmd 'mirv_filmmaker camtl debug 0' 0.2
$client.Close()

$text = $log.ToString()
$logPath = Join-Path $OutDir 'netcon.log'
[IO.File]::WriteAllText($logPath, $text)

$d01 = Compare-Images $start $p1
$d12 = Compare-Images $p1 $p2
$d23 = Compare-Images $p2 $p3
$d3e = Compare-Images $p3 $end
$segMatches = [regex]::Matches($text, '\[campath\]\[tick\].*seg=(\d)/5') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$wrapped = $text.Contains('wrapped to first keyframe tick')
$owners = ([regex]::Matches($text, '\[setupview\]\[owner\].*blockDemoViewOverride=1')).Count

Write-Host "`n=== SHORT PATH VERIFY ===" -ForegroundColor Cyan
Write-Host ("visual start->120ms mean={0:N2} changed={1:P1}" -f $d01.Mean, $d01.Changed)
Write-Host ("visual 120->280ms   mean={0:N2} changed={1:P1}" -f $d12.Mean, $d12.Changed)
Write-Host ("visual 280->500ms   mean={0:N2} changed={1:P1}" -f $d23.Mean, $d23.Changed)
Write-Host ("visual 500ms->end   mean={0:N2} changed={1:P1}" -f $d3e.Mean, $d3e.Changed)
Write-Host ("segments seen: {0}" -f (($segMatches -join ', ')))
Write-Host ("wrap message seen: {0}" -f $wrapped)
Write-Host ("owner frames: {0}" -f $owners)
Write-Host "captures: $OutDir"
Write-Host "log     : $logPath"

if ($segMatches.Count -ge 5 -and $wrapped -and $owners -gt 0 -and $d01.Mean -gt 2.0 -and $d12.Mean -gt 2.0) {
    exit 0
}
exit 1




