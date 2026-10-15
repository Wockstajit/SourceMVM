# Read the same per-process GPU memory counters Windows uses. No input or window activation.
# Missing counters are reported as unavailable, never as zero usage.
param(
    [string]$Process = 'cs2',
    [ValidateRange(1, 30)][int]$Samples = 3,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
$game = Get-Process -Name $Process | Where-Object MainWindowHandle -NE 0 | Select-Object -First 1
if (-not $game) { throw "No running $Process window." }
$gameId = $game.Id
$rows = @(Get-Counter '\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage' -SampleInterval 1 -MaxSamples $Samples | ForEach-Object {
    $all = @($_.CounterSamples | Where-Object { $_.InstanceName -like "pid_${gameId}_*" -and $_.Status -eq 0 })
    $dedicated = @($all | Where-Object Path -Like '*\Dedicated Usage')
    $shared = @($all | Where-Object Path -Like '*\Shared Usage')
    [pscustomobject]@{
        timestamp = $_.Timestamp.ToString('o')
        processId = $gameId
        dedicatedMiB = if ($dedicated.Count) { [math]::Round(($dedicated | Measure-Object CookedValue -Sum).Sum / 1MB, 1) } else { $null }
        sharedMiB = if ($shared.Count) { [math]::Round(($shared | Measure-Object CookedValue -Sum).Sum / 1MB, 1) } else { $null }
        workingSetMiB = [math]::Round((Get-Process -Id $gameId).WorkingSet64 / 1MB, 1)
    }
})
$json = ConvertTo-Json -InputObject $rows -Depth 3
if ($Out) { [IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out), $json, [Text.UTF8Encoding]::new($false)) }
$json
