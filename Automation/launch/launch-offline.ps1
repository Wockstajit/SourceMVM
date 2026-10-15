[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
try {
    # Keep all game arguments in the supported launcher, which always supplies
    # -insecure and +sv_lan 1. This entry point accepts no custom game arguments.
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot 'launch-cs2-netcon.ps1') `
        -ReadyTimeoutSeconds 120
    if ($LASTEXITCODE -ne 0) { throw 'The offline CS2 launcher failed.' }

    $game = Get-CimInstance Win32_Process -Filter "Name = 'cs2.exe'"
    if (-not $game) { throw 'CS2 exited before launch verification.' }
    foreach ($process in $game) {
        if ($process.CommandLine -notmatch '(?i)(?:^|\s)-insecure(?:\s|$)' -or
            $process.CommandLine -match '(?i)(?:^|\s)-secure(?:\s|$)') {
            Stop-Process -Id $process.ProcessId -Force
            throw 'CS2 was closed because its VAC-disabled launch flag could not be verified.'
        }
    }
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot 'wait-for-cs2-ready.ps1') -TimeoutSeconds 30
    if ($LASTEXITCODE -ne 0) { throw 'CS2 started offline, but the SOURCE:MVM hook did not answer.' }
    Write-Host 'SOURCE:MVM is ready. Verified -insecure (VAC disabled). Use offline demos or local practice only.' -ForegroundColor Green
} catch {
    Write-Error $_
    exit 1
}
