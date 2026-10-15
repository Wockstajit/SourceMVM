# Blocks until CS2 is launched, the AfxHookSource2 hook is loaded, and the netcon console is
# actually answering commands -- then exits 0. Exits 1 on timeout.
#
# WHY THIS EXISTS (agent workflow): after a rebuild the agent asks the user to relaunch CS2 and
# then has no signal for "the game is up, get back to work". This script IS that signal. The
# agent runs it as a BACKGROUND task (Bash/PowerShell run_in_background=true); when the script
# exits, the harness notifies the agent automatically. The agent must never sit in a foreground
# sleep loop waiting for a launch again.
#
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\wait-for-cs2-ready.ps1
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\wait-for-cs2-ready.ps1 -AfterClose
#
# Readiness definition (strict by default):
#   1. a cs2.exe process exists,
#   2. TCP 127.0.0.1:<Port> accepts a connection (-netconport is up),
#   3. `mirv_filmmaker health` returns BYTES over that connection (hook DLL loaded and the
#      advancedfx command actually executed -- an open port alone only proves the engine started).
# Pass -PortOnly to stop at step 2 (e.g. a vanilla launch without the hook).
#
# -AfterClose: if CS2 is already up when the script starts (stale instance about to be replaced),
# first wait for it to go away, THEN wait for the fresh launch. Use this when starting the waiter
# BEFORE the user has closed the old game.
[CmdletBinding()]
param(
    [int]$Port = 29010,
    # Long default on purpose: the user may take minutes to hit the button. This is a
    # notification channel, not a health check.
    [int]$TimeoutSeconds = 1800,
    [int]$PollSeconds = 2,
    [switch]$PortOnly,
    [switch]$AfterClose
)
$ErrorActionPreference = 'Stop'

function Test-NetconPortOpen {
    try {
        $probe = [System.Net.Sockets.TcpClient]::new()
        $connect = $probe.BeginConnect('127.0.0.1', $Port, $null, $null)
        $open = $connect.AsyncWaitHandle.WaitOne(500)
        if ($open) { $probe.EndConnect($connect) }
        $probe.Dispose()
        return $open
    } catch { return $false }
}

# Connects, sends `mirv_filmmaker health`, and returns $true only if the console replied with
# bytes. One short-lived connection per attempt -- a persistent attached netcon reader lags a
# live map, so never hold the socket open here.
function Test-HookAnswering {
    $client = $null
    try {
        $client = [System.Net.Sockets.TcpClient]::new()
        $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        if (-not $connect.AsyncWaitHandle.WaitOne(500)) { return $false }
        $client.EndConnect($connect)
        $client.NoDelay = $true
        $stream = $client.GetStream()
        Start-Sleep -Milliseconds 200
        $bytes = [System.Text.Encoding]::ASCII.GetBytes("mirv_filmmaker health`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $buffer = New-Object byte[] 8192
        $reply = ''
        $stop = (Get-Date).AddSeconds(2)
        while ((Get-Date) -lt $stop) {
            if ($stream.DataAvailable) {
                $read = $stream.Read($buffer, 0, $buffer.Length)
                if ($read -eq 0) { break }
                $reply += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
                if ($reply -match '(?m)^mirv_filmmaker health:' -and $reply -match 'render pump') { return $true }
            }
            Start-Sleep -Milliseconds 100
        }
        return $false
    } catch {
        return $false
    } finally {
        if ($client) { $client.Dispose() }
    }
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$lastStatus = Get-Date

if ($AfterClose) {
    $stale = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue).Count -gt 0 -or (Test-NetconPortOpen)
    if ($stale) {
        Write-Host "CS2 (or netcon $Port) is still up from a previous run; waiting for it to close first..."
        while ((Get-Date) -lt $deadline) {
            $procUp = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue).Count -gt 0
            if (-not $procUp -and -not (Test-NetconPortOpen)) { break }
            Start-Sleep -Seconds $PollSeconds
        }
        if ((Get-Date) -ge $deadline) {
            Write-Host "TIMEOUT: previous CS2 instance never closed within $TimeoutSeconds seconds."
            exit 1
        }
        Write-Host "Previous instance is gone. Now waiting for the fresh launch..."
    }
}

Write-Host "Waiting for CS2 readiness on netcon port $Port (timeout ${TimeoutSeconds}s, mode: $(if ($PortOnly) { 'port only' } else { 'hook health reply' }))..."
while ((Get-Date) -lt $deadline) {
    $cs2Up = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue).Count -gt 0
    if ($cs2Up -and (Test-NetconPortOpen)) {
        if ($PortOnly -or (Test-HookAnswering)) {
            $stamp = (Get-Date).ToString('HH:mm:ss')
            Write-Host "CS2 READY at $stamp -- process up, netcon $Port answering$(if (-not $PortOnly) { ', mirv_filmmaker health replied' })."
            exit 0
        }
    }
    if (((Get-Date) - $lastStatus).TotalSeconds -ge 30) {
        $lastStatus = Get-Date
        Write-Host ("still waiting... cs2 process: {0}, netcon port open: {1}" -f $cs2Up, (Test-NetconPortOpen))
    }
    Start-Sleep -Seconds $PollSeconds
}
Write-Host "TIMEOUT: CS2 never became ready within $TimeoutSeconds seconds."
exit 1
