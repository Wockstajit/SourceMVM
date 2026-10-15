#requires -Version 5
# Captures screenshots during sustained rifle fire while barrel-smoke / spray wrappers swap.
param(
    [int]$Port = 29010,
    [string]$Profile = 'classic',
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $OutDir) {
    $OutDir = Join-Path $repo "Automation\runs\fx-weapons-go\$(Get-Date -Format 'yyyyMMdd-HHmmss')-smoke-burst"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$client = [System.Net.Sockets.TcpClient]::new()
$client.Connect('127.0.0.1', $Port)
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [Text.Encoding]::ASCII

function Send([string[]]$cmds, [double]$wait = 1.2) {
    foreach ($c in $cmds) {
        $bytes = $enc.GetBytes("$c`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds ([int]($wait * 1000))
    }
}

Send @(
    'mirv_filmmaker fx on',
    'mirv_filmmaker fx debughud on',
    "mirv_filmmaker fx profile $Profile",
    'demo_gototick 12000',
    'demo_timescale 1',
    'demo_resume'
) 0.8

Start-Sleep -Seconds 3
$capture = Join-Path $repo 'Automation\capture\capture-game-window.ps1'
for ($i = 1; $i -le 12; $i++) {
    & $capture -Process cs2 -Mode client -Out (Join-Path $OutDir "$Profile-burst-$i.png") | Out-Null
    Start-Sleep -Milliseconds 350
}

Send @('demo_pause', 'mirv_filmmaker fx recent') 1.5
$buf = New-Object byte[] 65536
$read = 0
$deadline = (Get-Date).AddSeconds(2)
while ((Get-Date) -lt $deadline -and $stream.DataAvailable) {
    $read += $stream.Read($buf, 0, $buf.Length)
}
$recent = $enc.GetString($buf, 0, $read)
$recent | Out-File (Join-Path $OutDir 'fx-recent-tail.txt') -Encoding utf8
$client.Close()
Write-Host "OUTDIR=$OutDir"
Write-Host "CAPTURES=12"
