<#
.SYNOPSIS
  Blocks until a hooked Deadlock answers, for a session this script did not start.

.DESCRIPTION
  The readiness probe half of launch-deadlock.ps1, split out so it can be pointed at a Deadlock
  that was launched some other way -- by hand, from Steam, or by a launcher UI.

  Run this as a BACKGROUND task. The completion notification IS the "game is up" signal; there is
  no reason to idle-wait on it in the foreground, and doing so blocks a turn for nothing.

  Readiness means the hook ANSWERED. An open port only proves the listener thread started, which
  can be true while every version-coupled address failed to resolve.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File Automation\launch\wait-for-deadlock-ready.ps1
#>
[CmdletBinding()]
param(
    [int]$Port = 29020,
    [int]$TimeoutSeconds = 120,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$sawProcess = $false

while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Name deadlock -ErrorAction SilentlyContinue)) {
        Start-Sleep -Milliseconds 500
        continue
    }
    if (-not $sawProcess) {
        $sawProcess = $true
        if (-not $Quiet) { Write-Host "deadlock.exe is running; waiting for the hook..." }
    }

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        if ($async.AsyncWaitHandle.WaitOne(500)) {
            $client.EndConnect($async)
            $stream = $client.GetStream()
            $writer = New-Object System.IO.StreamWriter($stream)
            $writer.AutoFlush = $true
            Start-Sleep -Milliseconds 400
            $writer.Write("mirv_deadlock health`n")
            Start-Sleep -Milliseconds 900
            $buffer = New-Object byte[] 65536
            $text = ''
            while ($stream.DataAvailable) {
                $read = $stream.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) { break }
                $text += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            }
            $writer.Dispose()
            $stream.Dispose()
            if ($text -match 'AfxHookDeadlock health') {
                if (-not $Quiet) { Write-Host $text.TrimEnd() }
                Write-Host "[OK] the hook is answering on port $Port." -ForegroundColor Green
                exit 0
            }
        }
    } catch { } finally { $client.Close() }

    Start-Sleep -Milliseconds 500
}

if (-not $sawProcess) {
    Write-Host "[FAIL] deadlock.exe never appeared within $TimeoutSeconds s." -ForegroundColor Red
} else {
    Write-Host "[FAIL] Deadlock is running but the hook did not answer on port $Port." -ForegroundColor Red
    Write-Host "       Was it launched with -insecure? The DLL refuses to load without it." -ForegroundColor Red
}
exit 1
