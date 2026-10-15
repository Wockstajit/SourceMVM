<#
.SYNOPSIS
  Sends console commands to a hooked Deadlock and prints what comes back.

.DESCRIPTION
  The Deadlock counterpart of Automation/netcon/cs2-netcon.ps1, and the same protocol: a plain TCP
  socket to 127.0.0.1, ASCII commands terminated by newline, ASCII output streamed back, no
  handshake.

  The difference is who is listening. CS2's engine serves this itself via -netconport. Deadlock's
  engine2.dll carries the same netconsole server code, but whether the client build exposes it
  through that flag is not something a static scan can answer -- so AfxHookDeadlock hosts the
  channel in-process instead, on port 29020 by default. Either way this script is the client and
  cannot tell the difference; `mirv_deadlock netcon` reports which one is serving.

  Port 29020 rather than CS2's 29010 so a CS2 session and a Deadlock session can be driven side by
  side without either script reaching the wrong game.

.PARAMETER CommandBatch
  Commands separated by ';;'. Use this form from `powershell.exe -File`: a comma-separated array
  does not bind through that host, which is a mistake worth making only once.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\netcon\deadlock-netcon.ps1 `
      -CommandBatch "demo_pause;;mirv_deadlock health"
#>
[CmdletBinding()]
param(
    [string[]]$Commands = @(),
    [string]$CommandBatch = '',
    [int]$Port = 29020,
    # 3 seconds, not CS2's 1.5. A command sent here is queued and executed by the MAIN-THREAD
    # PUMP, so the reply cannot arrive until the game reaches its next frame -- and during a load
    # or a seek that is well over a second. A read window shorter than the round trip reads as
    # "the hook ignored me".
    [double]$ReadSeconds = 3.0,
    [int]$ConnectTimeoutMs = 3000,
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
    # BeginConnect + WaitOne rather than a blocking Connect: a closed port otherwise stalls for the
    # OS connect timeout, which turns a fast "the hook is not up" into a twenty-second hang.
    $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne($ConnectTimeoutMs)) {
        throw "no netcon on 127.0.0.1:$Port"
    }
    $client.EndConnect($async)
} catch {
    Write-Host "[FAIL] No netcon on 127.0.0.1:$Port." -ForegroundColor Red
    Write-Host "       Launch with Automation\launch\deadlock.bat, or run 'mirv_deadlock netcon start' in game." -ForegroundColor Red
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
    # Drain the connect banner so it does not get mixed into the first command's reply.
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
