$ErrorActionPreference='Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$testRoot = Join-Path $repo ('build/portable-test-' + [guid]::NewGuid().ToString('N') + '/checkout with spaces')
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$manifest = Get-Content (Join-Path $repo 'Automation/setup/packages.json') -Raw | ConvertFrom-Json
foreach ($name in @('runtime','dlss')) {
    $manifest.packages.$name.url = Join-Path $repo "build/packages/$($manifest.version)/source-mvm-$name-$($manifest.version).zip"
}
$localManifest=Join-Path $testRoot 'local-packages.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content $localManifest
& (Join-Path $repo 'Automation/setup/ensure-packages.ps1') -ManifestPath $localManifest -DestinationRoot $testRoot -Dlss
$hostPath = Join-Path $testRoot 'build/portable-runtime/bin/SourceMovieMakerHost.exe'
if (-not (Test-Path $hostPath)) {throw 'No native host in fresh installation'}
# Re-running must verify without accessing package sources.
foreach ($name in @('runtime','dlss')) { $manifest.packages.$name.url='https://invalid.invalid/not-used.zip' }
$manifest | ConvertTo-Json -Depth 8 | Set-Content $localManifest
& (Join-Path $repo 'Automation/setup/ensure-packages.ps1') -ManifestPath $localManifest -DestinationRoot $testRoot -Dlss
# Corrupt a package in a fresh root; no runtime may be written.
$badRoot=Join-Path $testRoot 'bad-checksum'
$manifest.packages.runtime.sha256='0'*64
$manifest.packages.runtime.url=Join-Path $repo "build/packages/$($manifest.version)/source-mvm-runtime-$($manifest.version).zip"
$manifest | ConvertTo-Json -Depth 8 | Set-Content $localManifest
$failed=$false
try { & (Join-Path $repo 'Automation/setup/ensure-packages.ps1') -ManifestPath $localManifest -DestinationRoot $badRoot } catch { $failed=$_.Exception.Message -match 'Checksum mismatch' }
if (-not $failed -or (Test-Path (Join-Path $badRoot 'build/portable-runtime/bin/SourceMovieMakerHost.exe'))) {throw 'Checksum failure did not block installation'}
# A checksum-valid archive must still be confined to its installation root.
$evilArchive=Join-Path $testRoot 'traversal.zip'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::Open($evilArchive,[IO.Compression.ZipArchiveMode]::Create)
try { $entry=$archive.CreateEntry('../escape.txt'); $writer=New-Object IO.StreamWriter($entry.Open()); $writer.Write('must not escape'); $writer.Dispose() } finally {$archive.Dispose()}
$manifest.packages.runtime.url=$evilArchive
$manifest.packages.runtime.sha256=(Get-FileHash $evilArchive).Hash
$manifest | ConvertTo-Json -Depth 8 | Set-Content $localManifest
$failed=$false
try { & (Join-Path $repo 'Automation/setup/ensure-packages.ps1') -ManifestPath $localManifest -DestinationRoot (Join-Path $testRoot 'bad-path') } catch { $failed=$_.Exception.Message -match 'Archive entry escapes' }
if (-not $failed) {throw 'Archive traversal was not rejected'}
Write-Host "PASS fresh install, idempotence, checksum and traversal rejection: $testRoot"
