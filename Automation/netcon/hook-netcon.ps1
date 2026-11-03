<#
.SYNOPSIS
  Sends console commands to a hook-hosted netcon and prints what comes back.

.DESCRIPTION
  The client side of Shared/NetconServer: a plain TCP socket to 127.0.0.1, ASCII commands
  terminated by newline, ASCII output streamed back, no handshake. Game wrappers such as
  css-netcon.ps1 pick the port and the hint shown when nothing is listening.

.PARAMETER CommandBatch
  Commands separated by ';;'. Use this form from `powershell.exe -File`: a comma-separated array
  does not bind through that host.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int]$Port,
    [string[]]$Commands = @(),
    [string]$CommandBatch = '',
    # A command is queued and run by the game's main thread, so the reply comes a frame later at
    # best, and well over a second later during a map load. A shorter read window reads as "ignored".
    [double]$ReadSeconds = 3.0,
    [int]$ConnectTimeoutMs = 3000,
    [string]$NotListeningHint = '',
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($CommandBatch) {
    $Commands = @($CommandBatch -split ';;' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
if (-not $Commands -or $Commands.Count -eq 0) {
    Write-Host "Nothing to send. Pass -CommandBatch 'a;;b' or -Commands a,b." -ForegroundColor Yellow
    exit 2
}

$client = New-Object System.Net.Sockets.TcpClient
try {
    # BeginConnect + WaitOne: a closed port otherwise stalls for the OS connect timeout.
    $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne($ConnectTimeoutMs)) { throw "no netcon on 127.0.0.1:$Port" }
    $client.EndConnect($async)
} catch {
    Write-Host "[FAIL] No netcon on 127.0.0.1:$Port." -ForegroundColor Red
    if ($NotListeningHint) { Write-Host "       $NotListeningHint" -ForegroundColor Red }
    exit 1
}

$client.NoDelay = $true
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$buffer = New-Object byte[] 65536

function Read-Available([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $text = ''
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) {
                $text += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
                continue
            }
        }
        Start-Sleep -Milliseconds 20
    }
    return $text
}

try {
    # Drain the connect banner so it does not mix into the first reply.
    Read-Available 0.6 | Out-Null
    foreach ($command in $Commands) {
        if (-not $Quiet) { Write-Host "> $command" -ForegroundColor Cyan }
        $writer.Write($command + "`n")
        $reply = Read-Available $ReadSeconds
        if ($reply) { Write-Host $reply.TrimEnd() }
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
    $client.Close()
}
exit 0
