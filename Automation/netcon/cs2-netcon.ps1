# Sends console commands to a running CS2 over its -netconport TCP console and
# prints the streamed console output. Pure backend - no window focus needed.
#
#   pwsh Automation\netcon\cs2-netcon.ps1 -Port 29010 -Commands "mirv_filmmaker ui_status"
#   powershell.exe -File Automation\netcon\cs2-netcon.ps1 -CommandBatch "demo_pause;;mirv_filmmaker health"
param(
    [int]$Port = 29010,
    # powershell.exe -File cannot reliably bind comma-separated values to an array parameter.
    # CommandBatch is the cross-shell/agent-safe multi-command form.
    [string]$CommandBatch,
    [string[]]$Commands = @('mirv_filmmaker ui_status'),
    [double]$ReadSeconds = 1.5,   # how long to read output after each command
    [string]$LogPath,
    [int]$ConnectTimeoutMs = 3000
)
$ErrorActionPreference = 'Stop'

if ($PSBoundParameters.ContainsKey('CommandBatch')) {
    $Commands = @($CommandBatch -split ';;' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($Commands.Count -eq 0) {
        throw 'cs2-netcon: -CommandBatch did not contain any commands.'
    }
}

# A fresh TcpClient is opened per invocation (this script is called once per
# command batch). The synchronous Connect() overload has no timeout and can
# hang indefinitely under connection churn instead of failing -- use
# BeginConnect/WaitOne so a stuck connect throws a clear, catchable error
# instead of silently wedging the whole calling pipeline with zero output.
$client = New-Object System.Net.Sockets.TcpClient
$connectResult = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
if (-not $connectResult.AsyncWaitHandle.WaitOne($ConnectTimeoutMs)) {
    $client.Close()
    throw "cs2-netcon: connect to 127.0.0.1:$Port timed out after ${ConnectTimeoutMs}ms"
}
$client.EndConnect($connectResult)
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII
$sb = New-Object System.Text.StringBuilder

function Read-Available([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                $text = $enc.GetString($buf, 0, $n)
                [void]$sb.Append($text)
                Write-Host -NoNewline $text
            }
        } else {
            Start-Sleep -Milliseconds 50
        }
    }
}

# Drain any banner/backlog first.
Read-Available 0.6
foreach ($cmd in $Commands) {
    Write-Host "`n>>> $cmd" -ForegroundColor Cyan
    $bytes = $enc.GetBytes($cmd + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Read-Available $ReadSeconds
}
$client.Close()

if ($LogPath) { [System.IO.File]::WriteAllText($LogPath, $sb.ToString()) }
