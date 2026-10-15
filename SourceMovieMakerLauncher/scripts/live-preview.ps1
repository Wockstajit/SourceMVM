param(
    [int]$Port = 5173,
    # Keep the saved profile instead of starting clean. Use when you are debugging something that
    # depends on state you built up by hand.
    [switch]$KeepProfile
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vite = $null

# FIRST-RUN BY DEFAULT.
#
# Live preview exists to look at the UI, and the parts most worth looking at are the ones that
# only appear once: the FFmpeg prompt, an empty settings file, the first-launch checks. Against
# the real profile you see each of those exactly once and never again, which is precisely
# backwards for a preview tool. So every run gets a throwaway userData directory and therefore
# behaves as if the launcher had never been started on this machine.
#
# Only the launcher's own saved state is discarded. The CS2 installation and the runtime
# payload live elsewhere and are untouched.
$profileDir = $null
if (-not $KeepProfile) {
    $profileDir = Join-Path ([System.IO.Path]::GetTempPath()) "smvm-live-$([System.Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $profileDir -Force | Out-Null
    Write-Host "First-run mode: throwaway profile at $profileDir (use -KeepProfile to opt out)."
}

function Test-TcpPort([int]$PortNumber) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect('127.0.0.1', $PortNumber)
        return $true
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

try {
    Write-Host "Starting Vite live renderer on http://127.0.0.1:$Port ..."
    $vite = Start-Process `
        -FilePath 'npm.cmd' `
        -ArgumentList @('run', 'dev', '--', '--host', '127.0.0.1', '--port', "$Port", '--strictPort') `
        -WorkingDirectory $root `
        -PassThru `
        -WindowStyle Hidden

    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        if (Test-TcpPort $Port) { break }
        if ($vite.HasExited) { throw "Vite exited before opening port $Port." }
        Start-Sleep -Milliseconds 250
    }
    if (-not (Test-TcpPort $Port)) {
        throw "Timed out waiting for Vite on port $Port."
    }

    $env:SMVM_RENDERER_URL = "http://127.0.0.1:$Port"
    Write-Host 'Starting Electron against the live renderer. Save a source file to trigger HMR.'
    $electronArgs = @('.')
    if ($profileDir) { $electronArgs += "--user-data-dir=$profileDir" }
    $electron = Join-Path $root 'node_modules\electron\dist\electron.exe'
    if (Test-Path -LiteralPath $electron) {
        $electronProcess = Start-Process `
            -FilePath $electron `
            -ArgumentList $electronArgs `
            -WorkingDirectory $root `
            -PassThru `
            -Wait
        $electronExitCode = $electronProcess.ExitCode
    } else {
        & npm.cmd exec -- electron @electronArgs
        $electronExitCode = $LASTEXITCODE
    }
    if ($electronExitCode -ne 0) {
        throw "Electron exited with code $electronExitCode."
    }
} finally {
    Remove-Item Env:SMVM_RENDERER_URL -ErrorAction SilentlyContinue
    if ($vite -and -not $vite.HasExited) {
        Stop-Process -Id $vite.Id -Force -ErrorAction SilentlyContinue
    }
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.CommandLine -and
            $_.CommandLine -match 'vite.*127\.0\.0\.1.*--port'
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    if ($profileDir -and (Test-Path -LiteralPath $profileDir)) {
        Remove-Item -LiteralPath $profileDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
