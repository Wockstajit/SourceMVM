[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bundleRoot = [IO.Path]::GetFullPath((Join-Path $repo 'SourceMovieMakerLauncher\bundle'))
$target = [IO.Path]::GetFullPath($Destination)
if (-not $target.StartsWith($bundleRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'FFmpeg must be staged inside the launcher bundle.'
}
$cache = Join-Path $repo 'build\bundle-cache\ffmpeg'
New-Item -ItemType Directory -Path $cache -Force | Out-Null
$archive = Join-Path $cache 'ffmpeg-release-essentials.zip'
$url = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
$checksum = (Invoke-WebRequest -UseBasicParsing -Uri ($url + '.sha256')).Content
$match = [regex]::Match([string]$checksum, '[a-fA-F0-9]{64}')
if (-not $match.Success) { throw 'The upstream FFmpeg checksum is invalid.' }
$expected = $match.Value.ToLowerInvariant()
if (-not (Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'The FFmpeg archive failed its upstream SHA-256 check.'
}
# Archive and extraction are build-only. The installer carries binaries, license and source notice.
$unpacked = Join-Path $cache $expected
if (-not (Test-Path -LiteralPath $unpacked)) { Expand-Archive -LiteralPath $archive -DestinationPath $unpacked }
$root = Get-ChildItem -LiteralPath $unpacked -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\ffmpeg.exe') } | Select-Object -First 1
if (-not $root -or -not (Test-Path -LiteralPath (Join-Path $root.FullName 'LICENSE')) -or
    -not (Test-Path -LiteralPath (Join-Path $root.FullName 'README.txt'))) { throw 'Incomplete FFmpeg archive.' }
New-Item -ItemType Directory -Path (Join-Path $target 'bin') -Force | Out-Null
foreach ($name in 'ffmpeg.exe', 'ffprobe.exe') {
    Copy-Item -LiteralPath (Join-Path $root.FullName ('bin\' + $name)) -Destination (Join-Path $target 'bin') -Force
}
foreach ($name in 'LICENSE', 'README.txt') { Copy-Item -LiteralPath (Join-Path $root.FullName $name) -Destination $target -Force }
Set-Content -LiteralPath (Join-Path $target 'archive.sha256') -Value $expected -Encoding ASCII
$versionOutput = & (Join-Path $target 'bin\ffmpeg.exe') -hide_banner -version
$versionExitCode = $LASTEXITCODE
if ($versionExitCode -ne 0) { throw 'Bundled FFmpeg cannot execute.' }
Write-Host ($versionOutput | Select-Object -First 1)
