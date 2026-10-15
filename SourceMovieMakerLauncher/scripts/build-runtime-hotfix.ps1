<#
.SYNOPSIS
    Packages a runtime HOTFIX: the changed hook files only, for the current app release.

.DESCRIPTION
    A hook-only fix does not need a new installer. This writes two assets to add to the EXISTING
    release of the launcher version users already have (tag v<package.json version>):

        payload-dist/hotfix/source-mvm-runtime-<version>-hotfix<serial>.zip   the changed files
        payload-dist/hotfix/runtime-hotfix.json                               serial, url, sha256

    On its next start a bundled launcher of that version downloads the zip, verifies it, and lays
    it over its installed runtime (electron/runtime-payload.cjs applyRuntimeHotfix). Increase
    -Serial for every hotfix of the same version. When the launcher itself changes, publish a new
    version with the complete installer instead (Docs/setup/publishing-releases.md).

    Reads the staged build only; it does not compile anything. Build and install the hook first.

.PARAMETER Serial
    Hotfix number for this app version: 1 for the first, then 2, 3, ...

.PARAMETER Files
    Paths under the staged bin\ folder to ship. Default: the hook DLL alone. Add a resources
    path when shaders or snippets changed with it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$Serial,
    [string[]]$Files = @('x64\AfxHookSource2.dll'),
    [string]$Version,
    [string]$StageRoot
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$launcherRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $launcherRoot
if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $launcherRoot 'package.json') -Raw | ConvertFrom-Json).version
}
if (-not $StageRoot) { $StageRoot = Join-Path $repoRoot 'build\source2-release' }
if ($Serial -lt 1) { throw '-Serial starts at 1.' }
$tag = "v$Version"
$binRoot = Join-Path $StageRoot 'bin'

$outputDir = Join-Path $launcherRoot 'payload-dist\hotfix'
$stage = Join-Path $outputDir '.staging'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($relative in $Files) {
    $source = Join-Path $binRoot $relative
    if (-not (Test-Path -LiteralPath $source)) { throw "Not in the staged build: $source" }
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}
# Keep the symbols of what ships, so a crash report from this hotfix resolves to function names
# (Automation/tools/archive-symbols.ps1). Refuses to continue on a PDB from a different link.
$shippedDlls = @($Files | Where-Object { $_ -like 'x64\*.dll' } | ForEach-Object { Split-Path -Leaf $_ })
if ($shippedDlls.Count) {
    & (Join-Path $repoRoot 'Automation\tools\archive-symbols.ps1') -BinDir (Join-Path $binRoot 'x64') `
        -PdbDir (Join-Path $StageRoot 'pdb\x64') -Names $shippedDlls
}
# Symbols and link leftovers never ship.
Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in '.pdb', '.lib', '.exp' } | Remove-Item -Force

$assetName = "source-mvm-runtime-$Version-hotfix$Serial.zip"
$archive = Join-Path $outputDir $assetName
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -LiteralPath $stage -Recurse -Force

$item = Get-Item -LiteralPath $archive
$manifest = [ordered]@{
    schema     = 1
    appVersion = $Version
    serial     = $Serial
    runtime    = [ordered]@{
        asset  = $assetName
        bytes  = $item.Length
        sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        url    = "https://github.com/Wockstajit/SourceMVM/releases/download/$tag/$assetName"
    }
}
# UTF-8 without a byte order mark (the launcher parses it as JSON).
[IO.File]::WriteAllText((Join-Path $outputDir 'runtime-hotfix.json'), ($manifest | ConvertTo-Json -Depth 4),
    (New-Object Text.UTF8Encoding($false)))

Write-Host ''
Write-Host ("Hotfix {0} for {1}: {2} ({3:N1} MB)" -f $Serial, $tag, $assetName, ($item.Length / 1MB))
Write-Host 'Add both to the existing release (replacing an older runtime-hotfix.json):'
Write-Host "  gh release upload $tag --repo Wockstajit/SourceMVM --clobber `"$archive`" `"$(Join-Path $outputDir 'runtime-hotfix.json')`""
