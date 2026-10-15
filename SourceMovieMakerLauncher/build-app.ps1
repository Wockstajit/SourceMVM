param(
    [switch]$Package,
    [switch]$Dir,
    # One self-contained installer exe: hook runtime, FX pack and DLSS 5 (ReShade) inside it.
    [switch]$Complete
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Stop-LauncherProcesses {
    Get-Process -Name SourceMovieMaker -ErrorAction SilentlyContinue | Stop-Process -Force
    # Electron helper processes that keep app.asar locked after the UI exits.
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $_.CommandLine -and
            $_.CommandLine -match 'SourceMovieMakerLauncher\\release'
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    Start-Sleep -Milliseconds 400
}

function Remove-StrayReleaseFolders {
    Get-ChildItem -LiteralPath $PSScriptRoot -Directory -Force |
        Where-Object { $_.Name -eq 'packaged' -or $_.Name -like 'release-*' } |
        ForEach-Object {
            try {
                Remove-Item -LiteralPath $_.FullName -Recurse -Force -Confirm:$false -ErrorAction Stop
                Write-Host "Removed stray $($_.Name)"
            } catch {
                Write-Warning "Could not remove stray $($_.Name): $($_.Exception.Message)"
            }
        }
}

if (-not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
    throw 'Node.js/npm was not found. Install the current Node.js LTS release first.'
}

if (Test-Path 'package-lock.json') {
    & npm.cmd ci
} else {
    & npm.cmd install
}

if ($LASTEXITCODE -ne 0) {
    throw "npm dependency installation failed with exit code $LASTEXITCODE."
}

& npm.cmd run build
if ($LASTEXITCODE -ne 0) {
    throw "Launcher app build failed with exit code $LASTEXITCODE."
}

if ($Complete) {
    # Stages bundle\ from the built hook, the compiled FX pack and the "dlss 5" folder.
    & npm.cmd run build:bundle
    if ($LASTEXITCODE -ne 0) { throw "Staging the bundle failed with exit code $LASTEXITCODE." }
}

if ($Package -or $Dir -or $Complete) {
    Stop-LauncherProcesses
    Remove-StrayReleaseFolders

    $releaseDir = Join-Path $PSScriptRoot 'release'
    if (Test-Path -LiteralPath $releaseDir) {
        try {
            Remove-Item -LiteralPath $releaseDir -Recurse -Force -Confirm:$false -ErrorAction Stop
        } catch {
            throw @"
Could not clear '$releaseDir'.
Close SourceMovieMaker.exe (and any Explorer windows inside that folder), then retry.
$($_.Exception.Message)
"@
        }
    }

    if ($Complete) {
        & npm.cmd exec -- electron-builder --win --config electron-builder.bundled.cjs --config.directories.output=release --publish never
    } elseif ($Dir) {
        & npm.cmd exec -- electron-builder --win --dir --config.directories.output=release --publish never
    } else {
        & npm.cmd exec -- electron-builder --win --config.directories.output=release --publish never
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Launcher packaging failed with exit code $LASTEXITCODE."
    }

    Write-Host "Packaged into $releaseDir"
    Write-Host 'Run: release\win-unpacked\SourceMovieMaker.exe'
}

Write-Host 'Launcher app build complete. No CS2/DLL target was built.'
