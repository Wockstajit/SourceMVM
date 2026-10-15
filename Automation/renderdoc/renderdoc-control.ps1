param(
    [ValidateSet('status', 'capture', 'path')]
    [string]$Action = 'status',
    [string]$Value = '',
    [int]$ProcessId = 0,
    [int]$TimeoutMilliseconds = 5000
)

$ErrorActionPreference = 'Stop'
$candidates = @(Get-Process cs2 -ErrorAction Stop | Where-Object { $ProcessId -eq 0 -or $_.Id -eq $ProcessId })
if ($candidates.Count -ne 1) { throw 'Specify -ProcessId for exactly one running CS2 process.' }
$cs2 = $candidates[0]
$pipeName = "SourceMvmRenderDocBridge_$($cs2.Id)"
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', $pipeName,
    [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::None)
try {
    $pipe.Connect($TimeoutMilliseconds)
    $writer = [System.IO.StreamWriter]::new($pipe, [System.Text.UTF8Encoding]::new($false), 4096, $true)
    $writer.AutoFlush = $true
    $reader = [System.IO.StreamReader]::new($pipe, [System.Text.Encoding]::UTF8, $false, 4096, $true)
    $command = if ([string]::IsNullOrWhiteSpace($Value)) { $Action } else { "$Action $Value" }
    $writer.WriteLine($command)
    $pending = $reader.ReadLineAsync()
    if (-not $pending.Wait($TimeoutMilliseconds)) { throw 'RenderDoc bridge response timed out.' }
    $reply = $pending.Result
    if (-not ($reply | ConvertFrom-Json).ok) { throw "RenderDoc bridge rejected command: $reply" }
    $reply
} finally {
    $pipe.Dispose()
}
