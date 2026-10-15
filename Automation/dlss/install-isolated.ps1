# Installs the local DLSS 5 payload into SOURCE:MVM's private CS2 binary tree.
# Steam's normal CS2 directory is deliberately kept free of ReShade proxy DLLs.
[CmdletBinding()]
param(
    [ValidateSet('NVIDIA original', 'RTX 40, 30, 20 compatible')]
    [string]$RuntimeVariant = 'NVIDIA original',
    [string]$Cs2Dir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$payload = Join-Path $repo 'dlss 5'
$privateRoot = Join-Path $repo 'build\dlss5-cs2'
$privateBin = Join-Path $privateRoot 'game\bin\win64'
$marker = Join-Path $privateRoot 'source-mvm-isolation.json'
$manifestPath = Join-Path $privateRoot 'dlss5-files.json'
$versionsPath = Join-Path $PSScriptRoot 'payload-versions.json'

function Write-Info([string]$Message) { Write-Host "[DLSS 5] $Message" }

function Test-SameFile([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or
        -not (Test-Path -LiteralPath $Destination -PathType Leaf)) { return $false }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash -eq
        (Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash
}

function Copy-IfDifferent([string]$Source, [string]$Destination, [string]$DisplayName) {
    if (Test-SameFile $Source $Destination) {
        Write-Info "$DisplayName is already current; skipped."
        return $false
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    Write-Info "Installed $DisplayName."
    return $true
}

function Test-IsReShadeProxy([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $version = (Get-Item -LiteralPath $Path).VersionInfo
    return $version.ProductName -match '(?i)ReShade' -or
        $version.FileDescription -match '(?i)ReShade'
}

if (-not (Test-Path -LiteralPath $versionsPath -PathType Leaf)) {
    throw "Version manifest is missing: $versionsPath"
}
$versions = Get-Content -Raw -LiteralPath $versionsPath | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    Write-Info 'Detecting the Counter-Strike 2 installation through Steam...'
    $Cs2Dir = & (Join-Path $repo 'Automation\lib\Resolve-Cs2Dir.ps1')
}
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Install CS2 through Steam or set SOURCEMVM_CS2_DIR to its install root.'
}
try { $Cs2Dir = (Resolve-Path -LiteralPath $Cs2Dir).Path }
catch { throw "The selected CS2 directory does not exist: $Cs2Dir" }
$sharedBin = Join-Path $Cs2Dir 'game\bin\win64'
if (-not (Test-Path -LiteralPath (Join-Path $sharedBin 'cs2.exe') -PathType Leaf)) {
    throw "Invalid CS2 directory: cs2.exe was not found under $sharedBin"
}
Write-Info "Detected CS2: $Cs2Dir"
Write-Info "Install target: $privateBin"
Write-Info 'Normal Steam launches will continue to use the original CS2 directory.'

if (Get-Process cs2 -ErrorAction SilentlyContinue) {
    throw 'CS2 is running. Close it before installing so DLL and ReShade configuration files are not locked or rewritten.'
}

$streamlineFiles = @(
    'nvngx_dlss.dll', 'nvngx_dlssg.dll',
    'sl.common.dll', 'sl.deepdvc.dll', 'sl.dlss_d.dll', 'sl.dlss_g.dll',
    'sl.dlss_nr.dll', 'sl.dlss.dll', 'sl.interposer.dll', 'sl.nis.dll',
    'sl.pcl.dll', 'sl.reflex.dll',
    'nvngx_dlss.license.txt', 'nis.license.txt', 'reflex.license.txt'
)
$sources = [ordered]@{}
foreach ($name in $streamlineFiles) {
    $sources[$name] = Join-Path $payload "streamline\$name"
}
$sources['nvngx_dlssnr.dll'] = Join-Path $payload "$RuntimeVariant\nvngx_dlssnr.dll"
$sources['renodx-dlss.addon64'] = Join-Path $payload 'addons\renodx-dlss.addon64'

$missing = @($sources.GetEnumerator() | Where-Object {
    -not (Test-Path -LiteralPath $_.Value -PathType Leaf)
} | ForEach-Object { $_.Value })
if ($missing.Count) {
    throw "The local DLSS 5 payload is incomplete. Missing:`n  $($missing -join "`n  ")`nNo files were copied."
}

$expectedRuntimeHash = $versions.runtimes.$RuntimeVariant.sha256
$actualRuntimeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sources['nvngx_dlssnr.dll']).Hash
if ($actualRuntimeHash -ne $expectedRuntimeHash) {
    throw "The $RuntimeVariant compatibility DLL failed its SHA-256 check. No files were copied."
}
$addonHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sources['renodx-dlss.addon64']).Hash
if ($addonHash -ne $versions.addon.sha256) {
    throw 'renodx-dlss.addon64 failed its SHA-256 check. No files were copied.'
}
if ($RuntimeVariant -eq 'NVIDIA original') {
    $signature = Get-AuthenticodeSignature -LiteralPath $sources['nvngx_dlssnr.dll']
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') {
        throw 'The NVIDIA original runtime does not have the expected valid NVIDIA signature. No files were copied.'
    }
}
Write-Info "Selected runtime: $RuntimeVariant"
Write-Info "Validated $($sources.Count) required local payload files. No downloads are used."

# ReShade must be a proxy beside the private cs2.exe. Prefer an existing private copy. If an old
# shared install exists, preserve it outside Steam and reuse that exact local DLL. The DLSS payload
# itself always comes exclusively from the local "dlss 5" folder above.
$reshadeSource = ''
$localReShade = Join-Path $payload 'ReShade64.dll'
$privateReShade = Join-Path $privateBin 'd3d11.dll'
$proxyNames = @('d3d11.dll', 'dxgi.dll', 'version.dll', 'dinput8.dll')
$sharedReShadeProxies = @($proxyNames | Where-Object {
    Test-IsReShadeProxy (Join-Path $sharedBin $_)
})
if (Test-IsReShadeProxy $localReShade) { $reshadeSource = $localReShade }
elseif (Test-IsReShadeProxy $privateReShade) { $reshadeSource = $privateReShade }
elseif ($sharedReShadeProxies.Count) { $reshadeSource = Join-Path $sharedBin $sharedReShadeProxies[0] }

if (-not $reshadeSource) {
    throw "ReShade with add-on support was not found. Put its 64-bit DLL at '$localReShade' and run this installer again. Nothing was changed."
}

$alreadyInstalled = (Test-Path -LiteralPath $marker -PathType Leaf) -and
    (Test-Path -LiteralPath $manifestPath -PathType Leaf) -and
    (Test-IsReShadeProxy $privateReShade) -and
    $sharedReShadeProxies.Count -eq 0
if ($alreadyInstalled) {
    foreach ($entry in $sources.GetEnumerator()) {
        if (-not (Test-SameFile $entry.Value (Join-Path $privateBin $entry.Key))) {
            $alreadyInstalled = $false
            break
        }
    }
}
if ($alreadyInstalled) {
    Write-Info "DLSS 5 ($RuntimeVariant) is already installed and current. Nothing was copied."
    Write-Info 'ReShade is isolated from the normal Steam CS2 directory.'
    return
}

# Remove only positively identified ReShade proxy DLLs from Steam. They are moved, never deleted,
# so normal Steam launches cannot load ReShade and the previous files remain recoverable.
$backup = $null
if ($sharedReShadeProxies.Count) {
    $backup = Join-Path $repo ('build\dlss5-steam-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($name in $sharedReShadeProxies) {
        $source = Join-Path $sharedBin $name
        $destination = Join-Path $backup $name
        Copy-Item -LiteralPath $source -Destination $destination -Force
        if ($source -eq $reshadeSource) { $reshadeSource = $destination }
        Remove-Item -LiteralPath $source -Force
        Write-Info "Moved Steam ReShade proxy $name to recoverable backup: $destination"
    }
}

try {
    Write-Info 'Preparing the private SOURCE:MVM CS2 binary tree...'
    & (Join-Path $PSScriptRoot 'prepare-isolated.ps1') -Cs2Dir $Cs2Dir
    if (-not (Test-Path -LiteralPath (Join-Path $privateBin 'cs2.exe') -PathType Leaf)) {
        throw 'Private CS2 preparation did not produce cs2.exe.'
    }

    foreach ($directory in @($privateRoot, (Join-Path $privateRoot 'game'), (Join-Path $privateRoot 'game\bin'), $privateBin)) {
        if ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing redirected private binary directory: $directory"
        }
    }

    Copy-IfDifferent $reshadeSource $privateReShade 'private ReShade proxy (d3d11.dll)' | Out-Null

    $stateAddon = Join-Path $repo 'ReShadeStateAddon\mvm_reshade_state.addon64'
    if (Test-Path -LiteralPath $stateAddon -PathType Leaf) {
        Copy-IfDifferent $stateAddon (Join-Path $privateBin 'mvm_reshade_state.addon64') 'SOURCE:MVM ReShade state add-on' | Out-Null
    } else {
        Write-Warning 'The optional SOURCE:MVM ReShade state add-on is missing; DLSS installation will continue.'
    }

    foreach ($entry in $sources.GetEnumerator()) {
        Copy-IfDifferent $entry.Value (Join-Path $privateBin $entry.Key) $entry.Key | Out-Null
    }

    $conflictingAddon = Join-Path $privateBin 'renodx-dlss5.addon64'
    if (Test-Path -LiteralPath $conflictingAddon) {
        if (-not $backup) {
            $backup = Join-Path $repo ('build\dlss5-steam-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
        }
        Move-Item -LiteralPath $conflictingAddon -Destination (Join-Path $backup 'renodx-dlss5.addon64') -Force
        Write-Info 'Moved the conflicting generic RenoDX add-on out of the private load directory.'
    }

    $templatePath = Join-Path $PSScriptRoot 'ReShade.isolated.ini'
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
        throw "ReShade configuration template is missing: $templatePath"
    }
    $config = (Get-Content -Raw -LiteralPath $templatePath).Replace('{{BINDIR}}', $privateBin)
    foreach ($name in @('ReShade.ini', 'ReShade2.ini')) {
        $destination = Join-Path $privateBin $name
        if (-not (Test-Path -LiteralPath $destination)) {
            [IO.File]::WriteAllText($destination, $config, [Text.UTF8Encoding]::new($false))
            Write-Info "Created $name."
        } else {
            Write-Info "$name already exists; preserved user settings."
        }
    }

    $manifest = [ordered]@{
        installedAt = (Get-Date).ToString('o')
        sourceCs2 = $Cs2Dir
        runtimeVariant = $RuntimeVariant
        steamReShadeIsolated = $true
        files = @($sources.Keys + @('d3d11.dll') | ForEach-Object {
            $file = Get-Item -LiteralPath (Join-Path $privateBin $_)
            [ordered]@{ name = $_; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash; size = $file.Length; version = $file.VersionInfo.FileVersion }
        })
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}
catch {
    throw "DLSS 5 installation did not complete: $($_.Exception.Message)"
}

Write-Host ''
Write-Info 'Installation completed successfully.'
Write-Info "DLSS/ReShade location: $privateBin"
Write-Info 'Normal Steam CS2 will not load ReShade because no ReShade proxy DLL remains beside its cs2.exe.'
Write-Info 'Launch DLSS only through Automation\dlss\launch-isolated.ps1 -EnableDlss.'
if ($backup) { Write-Info "Previous shared files are recoverable from: $backup" }
