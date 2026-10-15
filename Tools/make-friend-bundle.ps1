<#
.SYNOPSIS
    Assembles a self-contained, shareable SOURCE:MVM zip for another person's PC.

.DESCRIPTION
    Produces ONE zip a friend can extract and run with zero downloads and zero tooling:

        build\friend-bundle\SourceMVM-<version>.zip
          SourceMVM\
            START SOURCEMVM.bat                     plain cmd starter (no PowerShell needed)
            README.txt                              setup notes for the recipient
            SourceMovieMaker-Portable-<version>.exe the portable launcher
            SourceMovieMaker-data\payload\runtime\  hook + injector + resources, pre-installed
            SourceMovieMaker-data\payload\fx\       particle FX pack, pre-installed
            SourceMovieMaker-data\payload\installed.json

    The portable launcher keeps its data beside the exe (SourceMovieMaker-data\), so unpacking
    the payload there is exactly what the launcher's own runtime installer would have produced.
    installed.json is written from payload-manifest.json so the in-app update check compares
    real hashes instead of treating the pre-seed as "nothing installed".

    This script only READS already-built artifacts. Build them first:

        SourceMovieMakerLauncher\build-app.bat -Package        (the portable exe)
        SourceMovieMakerLauncher\scripts\build-release-payload.ps1   (the payload zips)

    Runs on stock Windows PowerShell 5.1; no pwsh 7, no Get-FileHash, no external tools.

.PARAMETER Version
    Bundle version. Defaults to the launcher's package.json version.

.PARAMETER OutputDir
    Where the zip lands. Defaults to <repo>\build\friend-bundle.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repoRoot = Split-Path -Parent $PSScriptRoot
$launcherRoot = Join-Path $repoRoot 'SourceMovieMakerLauncher'

if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $launcherRoot 'package.json') -Raw |
        ConvertFrom-Json).version
}
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'build\friend-bundle' }

# ---- locate inputs ---------------------------------------------------------------------------

$portableExe = Join-Path $launcherRoot "release\SourceMovieMaker-Portable-$Version.exe"
$payloadDist = Join-Path $launcherRoot 'payload-dist'
$runtimeZip = Join-Path $payloadDist "source-mvm-runtime-$Version.zip"
$fxZip = Join-Path $payloadDist "source-mvm-fx-$Version.zip"
$manifestPath = Join-Path $payloadDist 'payload-manifest.json'

$missing = @($portableExe, $runtimeZip, $fxZip, $manifestPath) |
    Where-Object { -not (Test-Path -LiteralPath $_) }
if ($missing) {
    throw @"
Missing prebuilt inputs:
$(($missing | ForEach-Object { "  $_" }) -join "`n")

Build them first:
  SourceMovieMakerLauncher\build-app.bat -Package
  SourceMovieMakerLauncher\scripts\build-release-payload.ps1
This script does not build anything.
"@
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.version -ne $Version) {
    throw "payload-manifest.json is version $($manifest.version) but the bundle is $Version. Rebuild the payload."
}

# ---- stage -----------------------------------------------------------------------------------

$stageRoot = Join-Path $OutputDir '.staging'
$bundleDir = Join-Path $stageRoot 'SourceMVM'
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Copy-Item -LiteralPath $portableExe -Destination $bundleDir

$payloadDir = Join-Path $bundleDir 'SourceMovieMaker-data\payload'
$runtimeDir = Join-Path $payloadDir 'runtime'
$fxDir = Join-Path $payloadDir 'fx'
New-Item -ItemType Directory -Path $runtimeDir -Force | Out-Null
New-Item -ItemType Directory -Path $fxDir -Force | Out-Null

Write-Host "Unpacking runtime payload into the bundle..."
[System.IO.Compression.ZipFile]::ExtractToDirectory($runtimeZip, $runtimeDir)
Write-Host "Unpacking FX payload into the bundle (~230 MB, takes a moment)..."
[System.IO.Compression.ZipFile]::ExtractToDirectory($fxZip, $fxDir)

# The same probes the launcher's own installer gates on. A bundle that fails these would unpack
# cleanly on the friend's machine and then fail in-game, which is the worst way to find out.
$probes = @(
    @{ Path = Join-Path $runtimeDir 'x64\AfxHookSource2.dll'; Needed = 'the hook itself' }
    @{ Path = Join-Path $runtimeDir 'x64\injector.exe';       Needed = 'injecting the hook' }
    @{ Path = Join-Path $fxDir 'source_mvm_fx\particles';     Needed = 'the particle FX pack' }
)
foreach ($probe in $probes) {
    if (-not (Test-Path -LiteralPath $probe.Path)) {
        throw "Bundle probe failed: $($probe.Path) is missing (needed for $($probe.Needed))."
    }
}

# installed.json mirrors what runtime-payload.cjs writes after a real install, so the in-app
# CHECK FOR UPDATES compares hashes instead of seeing an unversioned mystery payload.
$installed = [ordered]@{}
foreach ($name in @('runtime', 'fx')) {
    $entry = $manifest.components.$name
    if (-not $entry) { throw "payload-manifest.json has no '$name' component." }
    $installed[$name] = [ordered]@{
        version = $entry.version
        sha256  = $entry.sha256
        bytes   = $entry.bytes
    }
}
[IO.File]::WriteAllText(
    (Join-Path $payloadDir 'installed.json'),
    (($installed | ConvertTo-Json -Depth 4) + "`n"),
    (New-Object Text.UTF8Encoding($false)))

# ---- friend-facing starter + README ----------------------------------------------------------

# Plain cmd on purpose: it must run on any Windows with no PowerShell of any version involved.
$startBat = @"
@echo off
cd /d "%~dp0"
if not exist "SourceMovieMaker-Portable-$Version.exe" (
    echo.
    echo Could not find SourceMovieMaker-Portable-$Version.exe next to this file.
    echo Make sure you extracted the WHOLE zip first ^(right-click the zip, Extract All^),
    echo then run this .bat from the extracted SourceMVM folder.
    echo.
    pause
    exit /b 1
)
start "" "SourceMovieMaker-Portable-$Version.exe"
"@
[IO.File]::WriteAllText((Join-Path $bundleDir 'START SOURCEMVM.bat'), $startBat,
    (New-Object Text.UTF8Encoding($false)))

$readme = @"
SOURCE:MVM $Version  --  portable bundle
========================================

A CS2 demo / movie-making toolkit. Everything is already included in this
folder: the launcher, the hook runtime, and the particle FX pack. Nothing
else needs to be downloaded or installed to launch.

HOW TO START
------------
1. Extract the whole zip somewhere permanent (NOT inside the zip window),
   for example C:\SourceMVM. It keeps its settings in this folder.
2. Double-click "START SOURCEMVM.bat" (or the SourceMovieMaker-Portable exe).
   Windows SmartScreen may warn because the exe is not code-signed by a
   known publisher: click "More info" then "Run anyway".
3. If the launcher says CS2 NOT FOUND, click LOCATE GAME and pick your
   cs2.exe (usually C:\Program Files (x86)\Steam\steamapps\common\
   Counter-Strike Global Offensive\game\bin\win64\cs2.exe).
4. Press GO. CS2 starts with SOURCE:MVM loaded.

RECOMMENDED: WORKSHOP TOOLS MODE
--------------------------------
In the launcher, pick the DEV / Workshop Tools launch mode. It is the
recommended way to run: streams work better and it has more options than
regular CS2. It needs the free "Counter-Strike 2 Workshop Tools" DLC --
if it is missing, the launcher shows an INSTALL button that opens it in
Steam (free, installs into your existing CS2).

RENDERING (FFMPEG)
------------------
Rendering videos needs FFmpeg. On first start the launcher checks for it
and offers a one-click "INSTALL IT FOR ME" (with a progress bar) that
downloads the official build -- that is the only download this bundle
ever needs, and only for rendering. Already have FFmpeg somewhere? Use
"I HAVE IT -- PICK ffmpeg.exe" in that prompt, or ADVANCED -> Locations
-> FFmpeg -> SELECT, to point the launcher at your copy instead.

REQUIREMENTS
------------
- Windows 10/11, 64-bit (everything else needed is preinstalled on them).
- Counter-Strike 2 installed through Steam.
- This is for OFFLINE demo and movie work only. Never use it on
  VAC-protected or online servers.

UPDATES
-------
The launcher's CHECK FOR UPDATES button fetches newer official releases.
This bundle may be newer than the last published release; if the updater
offers a version older than $Version, skip it.
"@
[IO.File]::WriteAllText((Join-Path $bundleDir 'README.txt'), ($readme -replace "`n", "`r`n"),
    (New-Object Text.UTF8Encoding($false)))

# ---- zip -------------------------------------------------------------------------------------

$archive = Join-Path $OutputDir "SourceMVM-$Version.zip"
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }

Write-Host "Zipping the bundle (the FX pack makes this the slow part)..."
# Entry-by-entry rather than CreateFromDirectory: under Windows PowerShell 5.1 the .NET
# Framework writes ENTRY NAMES WITH BACKSLASHES, which violates the zip spec and breaks
# non-Windows-Explorer unzippers. Building the names ourselves pins them to forward slashes
# on every PowerShell. Paths are made relative to the staging ROOT (which holds only
# SourceMVM\), so the zip extracts as a single SourceMVM\ folder.
$zip = [System.IO.Compression.ZipFile]::Open($archive, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $prefixLength = (Get-Item -LiteralPath $stageRoot).FullName.Length + 1
    foreach ($file in Get-ChildItem -LiteralPath $stageRoot -Recurse -File) {
        $entryName = $file.FullName.Substring($prefixLength).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $file.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}

Remove-Item -LiteralPath $stageRoot -Recurse -Force

$size = [math]::Round((Get-Item -LiteralPath $archive).Length / 1MB, 1)
Write-Host ''
Write-Host "Friend bundle ready: $archive ($size MB)"
Write-Host 'Send that one zip. The recipient extracts it and runs "START SOURCEMVM.bat".'
