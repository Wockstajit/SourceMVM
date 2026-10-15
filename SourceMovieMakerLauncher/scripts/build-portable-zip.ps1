<#
.SYNOPSIS
    Zips the unpacked complete app into SourceMovieMaker-Portable-<version>.zip.

.DESCRIPTION
    Run after `npm run package:bundled`. Zips release\win-unpacked and adds an empty portable.flag
    beside the exe, which makes the launcher keep its settings and runtime in a
    SourceMovieMaker-data folder next to the exe (electron/main.cjs portableFolder). Nothing is
    installed and nothing unpacks to %TEMP%.

    The zip is a plain extra release asset. It is not listed in latest.yml, so the updater never
    offers it, and the website only links the Complete-Setup exe. A portable launcher cannot
    replace itself: it tells the user to download the new zip or the setup exe.
#>
param([string]$ReleaseDirectory = '')
$ErrorActionPreference = 'Stop'
$launcher = Split-Path $PSScriptRoot -Parent
if (-not $ReleaseDirectory) { $ReleaseDirectory = Join-Path $launcher 'release' }
$version = (Get-Content -LiteralPath (Join-Path $launcher 'package.json') -Raw | ConvertFrom-Json).version
$unpacked = Join-Path $ReleaseDirectory 'win-unpacked'
if (-not (Test-Path -LiteralPath (Join-Path $unpacked 'SourceMovieMaker.exe'))) {
    throw "Run npm run package:bundled first: $unpacked has no SourceMovieMaker.exe."
}
if (-not (Test-Path -LiteralPath (Join-Path $unpacked 'resources/bundle'))) {
    throw 'win-unpacked is not the complete build (resources/bundle is missing). Use npm run package:bundled.'
}
$archive = Join-Path $ReleaseDirectory "SourceMovieMaker-Portable-$version.zip"
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Entries are added one by one so every name uses forward slashes. ZipFile.CreateFromDirectory on
# Windows PowerShell 5.1 writes backslashes, which the ZIP spec forbids and other tools mishandle.
$root = (Resolve-Path -LiteralPath $unpacked).Path.TrimEnd('\')
$stream = [IO.File]::Open($archive, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
$zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
try {
    Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        $name = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
    }
    [void]$zip.CreateEntry('portable.flag')
} finally { $zip.Dispose(); $stream.Dispose() }
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Portable zip ready: $archive"
Write-Host "SHA-256: $hash"
Write-Host 'Add it to SHA256SUMS.txt and upload it as a normal release asset. Do not add it to latest.yml.'
