param([string]$ManifestPath = (Join-Path $PSScriptRoot 'packages.json'), [string]$DestinationRoot = '', [switch]$Dlss)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $DestinationRoot) { $DestinationRoot = $repo }
$DestinationRoot = [IO.Path]::GetFullPath($DestinationRoot)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
function Get-ReleaseHeaders {
    $token = $env:GH_TOKEN
    if (-not $token) { $token = $env:GITHUB_TOKEN }
    if (-not $token) {
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Install Git for Windows and sign in to the repository, or supply GH_TOKEN for private package downloads.' }
        $oldPrompt=$env:GIT_TERMINAL_PROMPT; $oldInteractive=$env:GCM_INTERACTIVE
        try {
            $env:GIT_TERMINAL_PROMPT='0'; $env:GCM_INTERACTIVE='Never'
            $credential = "protocol=https`nhost=github.com`npath=Wockstajit/Source-MVM.git`n`n" | git -C $repo credential fill 2>$null
            foreach ($line in $credential) { if ($line.StartsWith('password=')) { $token=$line.Substring(9) } }
        } finally { $env:GIT_TERMINAL_PROMPT=$oldPrompt; $env:GCM_INTERACTIVE=$oldInteractive }
    }
    if (-not $token) { throw 'Sign in to GitHub with the same account used to clone Source-MVM, then launch again. Private release downloads need repository access.' }
    return @{ Authorization=('Bearer '+$token); 'User-Agent'='SourceMVM-setup'; Accept='application/vnd.github+json' }
}

$names = if ($Dlss) { @('runtime','dlss') } else { @('runtime') }
foreach ($name in $names) {
    $package = $manifest.packages.$name
    if (-not $package -or @($package.files).Count -eq 0 -or $package.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw "Invalid package manifest: $name" }
    $destination = Join-Path $DestinationRoot $package.destination
    $rootPrefix = $DestinationRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not ([IO.Path]::GetFullPath($destination).StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase))) { throw 'Package destination escapes workspace.' }
    $complete = $true
    foreach ($file in $package.files) {
        $target = Join-Path $destination $file.path
        if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.sha256) { $complete = $false; break }
    }
    if ($complete) { continue }
    if (Get-Process cs2 -ErrorAction SilentlyContinue) { throw 'Close CS2 before installing runtime packages.' }
    $cache = Join-Path $DestinationRoot 'build/package-cache'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $archive = Join-Path $cache ($package.sha256 + '.zip')
    if (-not (Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $package.sha256) {
        Write-Host "Downloading SOURCE:MVM $name package ($($manifest.version))..."
        $partial = $archive + '.partial'
        if ($package.url -match '^https://') {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            if ($package.url -match '^https://github.com/Wockstajit/Source-MVM/releases/download/([^/]+)/([^/]+)$') {
                $tag=$matches[1]; $assetName=$matches[2]
                $headers=Get-ReleaseHeaders
                $release=Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/Wockstajit/Source-MVM/releases/tags/$tag"
                $asset=$release.assets | Where-Object {$_.name -eq $assetName} | Select-Object -First 1
                if (-not $asset) {throw "Release $tag is missing $assetName. The package publisher must finish uploading it."}
                $headers.Accept='application/octet-stream'
                Invoke-WebRequest -UseBasicParsing -Headers $headers -Uri $asset.url -OutFile $partial
            } else {
                Invoke-WebRequest -UseBasicParsing -Uri $package.url -OutFile $partial
            }
        } elseif (Test-Path -LiteralPath $package.url) {
            Copy-Item -LiteralPath $package.url -Destination $partial -Force
        } else { throw "Package unavailable: $($package.url)" }
        if ((Get-FileHash -LiteralPath $partial).Hash -ne $package.sha256) { throw "Checksum mismatch for $name. No files were installed." }
        Move-Item -LiteralPath $partial -Destination $archive -Force
    }
    # Inspect every archive path before extracting any entry.
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $prefix = [IO.Path]::GetFullPath($destination).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        foreach ($entry in $zip.Entries) {
            $target = [IO.Path]::GetFullPath((Join-Path $destination $entry.FullName))
            if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive entry escapes its installation directory.' }
            # Never write through a junction/symlink into somebody else's installation.
            $parent = $target
            while ($parent -and $parent.Length -ge $DestinationRoot.Length) {
                if ((Test-Path -LiteralPath $parent) -and ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Redirected package path: $parent" }
                $parent = Split-Path $parent -Parent
            }
        }
        foreach ($entry in $zip.Entries) {
            $target = Join-Path $destination $entry.FullName
            if (-not $entry.Name) { New-Item -ItemType Directory -Path $target -Force | Out-Null; continue }
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally { $zip.Dispose() }
    foreach ($file in $package.files) {
        $target = Join-Path $destination $file.path
        if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ne $file.sha256) { throw "Installed package verification failed: $target" }
    }
    Write-Host "Installed $name package in $destination"
}
