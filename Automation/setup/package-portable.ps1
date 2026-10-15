param(
    [string]$Version = 'portable-20260905',
    [string]$OutputDirectory = '',
    [ValidateSet('NVIDIA original', 'RTX 40, 30, 20 compatible')]
    [string]$DlssRuntimeVariant = 'NVIDIA original'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo ('build/packages/' + $Version) }
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stage = Join-Path $OutputDirectory ('stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$runtime = Join-Path $stage 'runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
foreach ($directory in @('x64','resources','osu','easter-eggs')) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo "build/source2-release/bin/$directory"))) { continue }
    Copy-Item -LiteralPath (Join-Path $repo "build/source2-release/bin/$directory") -Destination $runtime -Recurse
}
Copy-Item -LiteralPath (Join-Path $repo 'SourceMovieMakerLauncher/host/build/SourceMovieMakerHost.exe') -Destination $runtime
# Keep symbols and build products out of the distribution.
Get-ChildItem -LiteralPath $runtime -Recurse -File | Where-Object { $_.Extension -in '.pdb','.lib','.exp','.i64','.id0','.id1','.id2','.nam','.til' } | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
$dlss = Join-Path $stage 'dlss'
New-Item -ItemType Directory -Path $dlss -Force | Out-Null
foreach ($item in @('reshade/ReShade64.dll')) {
    $target = Join-Path $dlss $item
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "build/dlss5-downloads/$item") -Destination $target
}
$runtimeSource = Join-Path $repo "dlss 5/$DlssRuntimeVariant/nvngx_dlssnr.dll"
$addonSource = Join-Path $repo 'dlss 5/addons/renodx-dlss.addon64'
if (-not (Test-Path -LiteralPath $runtimeSource) -or -not (Test-Path -LiteralPath $addonSource)) {
    throw 'The selected local DLSS runtime or RenoDX add-on is missing from dlss 5 storage.'
}
New-Item -ItemType Directory -Path (Join-Path $dlss 'runtime') -Force | Out-Null
Copy-Item -LiteralPath $runtimeSource -Destination (Join-Path $dlss 'runtime/nvngx_dlssnr.dll')
Copy-Item -LiteralPath $addonSource -Destination (Join-Path $dlss 'renodx-dlss.addon64')
$versions = Get-Content -LiteralPath (Join-Path $repo 'Automation/dlss/payload-versions.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $runtimeSource).Hash -ne $versions.runtimes.$DlssRuntimeVariant.sha256) {
    throw 'Selected DLSS runtime does not match payload-versions.json.'
}
$packages = [ordered]@{}
foreach ($name in @('runtime','dlss')) {
    $source = Join-Path $stage $name
    $archive = Join-Path $OutputDirectory "source-mvm-$name-$Version.zip"
    if (Test-Path -LiteralPath $archive) { throw "Use a new package version; archive already exists: $archive" }
    [IO.Compression.ZipFile]::CreateFromDirectory($source,$archive,[IO.Compression.CompressionLevel]::Optimal,$false)
    $files = @(Get-ChildItem -LiteralPath $source -Recurse -File | ForEach-Object {
        @{path=$_.FullName.Substring($source.Length+1); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
    })
    $packages[$name] = [ordered]@{
        url="https://github.com/Wockstajit/SourceMVM/releases/download/$Version/$([IO.Path]::GetFileName($archive))"
        sha256=(Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
        destination=$(if($name -eq 'runtime') {'build/portable-runtime/bin'} else {'build/dlss5-downloads'})
        files=$files
    }
}
[ordered]@{version=$Version; sourceCommit=(& git -C $repo rev-parse HEAD); packages=$packages} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'packages.json') -Encoding UTF8
Write-Host "Packages and manifest ready: $OutputDirectory"
