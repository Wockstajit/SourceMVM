<#
.SYNOPSIS
    Packages the game-side runtime payload as release assets for the public release repo.

.DESCRIPTION
    Produces the two zips the launcher downloads at runtime, plus the manifest that describes
    them:

        payload-dist/source-mvm-runtime-<version>.zip   hook DLL + injector + resources
        payload-dist/source-mvm-fx-<version>.zip        particle FX pack
        payload-dist/payload-manifest.json              versions, sizes, sha256, URLs

    This script only READS an already-staged CS2 build. It never invokes CMake, never compiles
    AfxHookSource2, never converts FX, and never launches CS2 -- run build-source2.bat yourself
    first if the staged output is stale.

    The manifest's download URLs are written for a GitHub release tag, because the launcher
    resolves assets by browser_download_url. -Tag defaults to v<version>.

.PARAMETER Version
    Payload version. Defaults to the launcher's package.json version.

.PARAMETER Tag
    Release tag the assets will be uploaded under. Defaults to "v<Version>".

.PARAMETER StageRoot
    Staged CS2 build to package. Defaults to <repo>/build/source2-release.

.PARAMETER FxRoot
    FX pack to package. Defaults to the staged pack, falling back to the converter output.

.PARAMETER SkipFx
    Package only the hook runtime. The FX pack is ~230 MB and rarely changes, so this makes a
    hook-only beta bump take seconds instead of minutes. The manifest still advertises the FX
    pack, carried over from -FxFromTag, so a fresh install is never left without it.

.PARAMETER FxFromTag
    Release tag to carry the existing FX component over from when -SkipFx is used. Defaults to
    the newest published release that has a manifest.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Tag,
    [string]$StageRoot,
    [string]$FxRoot,
    [switch]$SkipFx,
    [string]$FxFromTag
)

$ErrorActionPreference = 'Stop'
# Present by default on PowerShell 7, but this script is invoked through powershell.exe (5.1)
# from npm run build:payload, where the assembly has to be loaded explicitly.
Add-Type -AssemblyName System.IO.Compression.FileSystem

$launcherRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $launcherRoot

if (-not $Version) {
    $Version = (Get-Content -LiteralPath (Join-Path $launcherRoot 'package.json') -Raw |
        ConvertFrom-Json).version
}
if (-not $Tag) { $Tag = "v$Version" }
if (-not $StageRoot) { $StageRoot = Join-Path $repoRoot 'build\source2-release' }

$owner = 'Wockstajit'
$repo = 'SourceMVM'

# Deliberately NOT under release\: build-app.ps1 clears that folder before packaging and deletes
# anything matching release-*, so payload assets staged there would vanish on the next app build.
$outputDir = Join-Path $launcherRoot 'payload-dist'
$stagingDir = Join-Path $outputDir '.staging'

# ---- locate inputs ---------------------------------------------------------------------------

$binRoot = Join-Path $StageRoot 'bin'

# Everything the hook resolves at runtime, and where it must land relative to the runtime root.
# The launcher unpacks this zip so AfxHookSource2.dll sits at <runtime>\x64\, and the hook finds
# its helpers relative to its own module directory -- so these paths are a contract, not a
# preference. A payload missing any of them installs cleanly and then fails in-game, which is the
# worst way to find out.
$requiredRuntimeFiles = @(
    @{ Path = 'x64\AfxHookSource2.dll';                                Needed = 'the hook itself' }
    @{ Path = 'x64\injector.exe';                                      Needed = 'injecting the hook' }
    @{ Path = 'x64\AfxHook.dat';                                       Needed = 'injection' }
    @{ Path = 'x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe';           Needed = 'demo names, sides, scoreboards and MVPs' }
    @{ Path = 'x64\FilmmakerDemoRepair\FilmmakerDemoRepair.exe';       Needed = 'repairing legacy demos' }
    @{ Path = 'x64\FilmmakerDemoKeyframes\FilmmakerDemoKeyframes.exe'; Needed = 'one-frame backward seeks and scrubbing' }
)

$missing = $requiredRuntimeFiles |
    Where-Object { -not (Test-Path -LiteralPath (Join-Path $binRoot $_.Path)) }

if ($missing) {
    $detail = ($missing | ForEach-Object { "  $($_.Path)`n      needed for: $($_.Needed)" }) -join "`n"
    throw @"
The staged build is missing runtime dependencies:

$detail

Staged build: $StageRoot

Run build-source2.bat first. Note that its demo-helper step is SKIPPED silently when 'go' is not
on PATH, which is how a payload ends up without them -- check for the warning in its output.
This script does not build anything.
"@
}

if (-not $FxRoot) {
    $fxCandidates = @(
        (Join-Path $StageRoot 'fx\source_mvm_fx'),
        (Join-Path $repoRoot 'FX\compiled\source_mvm_fx')
    )
    # A pack without particles/ is the stub layout (cfg + ui_override only), not a usable pack.
    $FxRoot = $fxCandidates |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'particles') } |
        Select-Object -First 1
}

if (-not $SkipFx -and -not $FxRoot) {
    throw 'No FX pack with a particles/ directory was found. Pass -FxRoot, or -SkipFx to omit it.'
}

# ---- stage ----------------------------------------------------------------------------------

if (Test-Path -LiteralPath $stagingDir) { Remove-Item -LiteralPath $stagingDir -Recurse -Force }
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

function New-PayloadZip {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$SourceDir
    )

    $archive = Join-Path $outputDir "$Name-$Version.zip"
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }

    # An empty stage would otherwise ship as a valid-but-useless ~500 byte zip that only fails
    # later, on a user's machine, as a checksum-clean archive missing the hook.
    $stagedFiles = @(Get-ChildItem -LiteralPath $SourceDir -Recurse -File)
    if ($stagedFiles.Count -eq 0) {
        throw "Nothing was staged for $Name at $SourceDir."
    }

    Write-Host "Packing $Name from $SourceDir ($($stagedFiles.Count) files)"
    # ZipFile rather than Compress-Archive: the FX pack is ~230 MB across thousands of files, and
    # Compress-Archive takes minutes on that. The trailing $false is includeBaseDirectory, which
    # keeps the archive free of a redundant top-level folder -- the launcher unpacks straight
    # into its component directory and probes fixed relative paths.
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDir,
        $archive,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

    $item = Get-Item -LiteralPath $archive
    [pscustomobject]@{
        Asset  = $item.Name
        Bytes  = $item.Length
        Sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

# Keep the hook's symbols before they are stripped, so crash reports from this release resolve to
# function names (Automation/tools/archive-symbols.ps1). Refuses a PDB from a different link.
& (Join-Path $repoRoot 'Automation\tools\archive-symbols.ps1') -BinDir (Join-Path $binRoot 'x64') `
    -PdbDir (Join-Path $StageRoot 'pdb\x64')

# Runtime: only what actually goes into CS2. No .pdb (27 MB of symbols nobody downloading a
# release build needs), no Source 1 / GoldSrc binaries.
$runtimeStage = Join-Path $stagingDir 'runtime'
New-Item -ItemType Directory -Path (Join-Path $runtimeStage 'x64') -Force | Out-Null
Copy-Item -Path (Join-Path $binRoot 'x64\*') -Destination (Join-Path $runtimeStage 'x64') -Recurse -Force
# osu\ (bundled songs + skin) and easter-eggs\ (UwU sounds) are read from beside the hook.
foreach ($extra in @('resources', 'osu', 'easter-eggs')) {
    $source = Join-Path $binRoot $extra
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $runtimeStage -Recurse -Force
    }
}

# ffmpeg\ is deliberately NOT copied from the staged build.
#
# The hook resolves ffmpeg as: <runtime>\ffmpeg\ffmpeg.ini [Ffmpeg]Path if that key is set, and
# only otherwise falls back to <runtime>\ffmpeg\bin\ffmpeg.exe. A developer's ffmpeg.ini points
# at their own machine (a winget package path, say). Shipping it would set Path= to a directory
# that exists on nobody else's disk AND suppress the bundled fallback -- strictly worse than
# shipping no ini at all. The launcher writes a correct one at install time; see
# electron/ffmpeg-tool.cjs.
$stagedFfmpegIni = Join-Path $binRoot 'ffmpeg\ffmpeg.ini'
if (Test-Path -LiteralPath $stagedFfmpegIni) {
    Write-Host 'Skipping the staged ffmpeg.ini (it carries a developer-local path).'
}
# Filter with Where-Object, NOT -Include. `Get-ChildItem -LiteralPath <dir> -Recurse -Include`
# matches the pattern against the path's own leaf, not the children, so a literal directory path
# makes -Include silently match nothing and return EVERY file -- which piped into Remove-Item
# deletes the entire staged runtime and ships a checksum-clean, empty payload.
Get-ChildItem -LiteralPath $runtimeStage -Recurse -File |
    Where-Object { $_.Extension -in '.pdb', '.lib', '.exp', '.i64', '.id0', '.id1', '.id2', '.nam', '.til' } |
    Remove-Item -Force

$components = [ordered]@{}
$components['runtime'] = New-PayloadZip -Name 'source-mvm-runtime' -SourceDir $runtimeStage

$carriedFx = $null

if (-not $SkipFx) {
    # Nested under source_mvm_fx/ because that is the directory USRLOCALCSGO must point at.
    $fxStage = Join-Path $stagingDir 'fx'
    New-Item -ItemType Directory -Path (Join-Path $fxStage 'source_mvm_fx') -Force | Out-Null
    Copy-Item -Path (Join-Path $FxRoot '*') -Destination (Join-Path $fxStage 'source_mvm_fx') -Recurse -Force
    # cfg\ and ui_override\ServerConfig.vdf are what CS2 writes into a mounted pack: this machine's
    # video, sound, bind and bot settings. Never ship them. The launcher keeps each player's own
    # copies when it installs a new pack (electron/payload-swap.cjs).
    foreach ($cs2Settings in @('cfg', 'ui_override\ServerConfig.vdf')) {
        $stagedSettings = Join-Path (Join-Path $fxStage 'source_mvm_fx') $cs2Settings
        if (Test-Path -LiteralPath $stagedSettings) {
            Write-Host "Skipping the staged FX pack's $cs2Settings (CS2 settings from this machine)."
            Remove-Item -LiteralPath $stagedSettings -Recurse -Force
        }
    }
    $components['fx'] = New-PayloadZip -Name 'source-mvm-fx' -SourceDir $fxStage
} else {
    # Carry the FX component over from an already-published release.
    #
    # Without this, -SkipFx emits a manifest whose only component is the runtime -- and since the
    # launcher installs exactly what the manifest lists, a FRESH install would silently end up
    # with no FX pack at all. Skipping the 230 MB re-upload must not mean dropping the entry.
    $sourceTag = $FxFromTag
    try {
        if (-not $sourceTag) {
            $published = Invoke-RestMethod -Uri "https://api.github.com/repos/$owner/$repo/releases?per_page=20" `
                -Headers @{ 'User-Agent' = 'smvm-payload-build'; Accept = 'application/vnd.github+json' }
            $sourceTag = ($published |
                Where-Object { -not $_.draft -and ($_.assets.name -contains 'payload-manifest.json') } |
                Select-Object -First 1).tag_name
        }
        if (-not $sourceTag) { throw 'no published release carries a payload manifest' }

        # Parse explicitly. GitHub serves release assets as application/octet-stream, so
        # Invoke-RestMethod hands back a raw string rather than an object -- and every property
        # lookup on it then silently yields $null.
        $previousText = (Invoke-WebRequest -UseBasicParsing -Headers @{ 'User-Agent' = 'smvm-payload-build' } `
            -Uri "https://github.com/$owner/$repo/releases/download/$sourceTag/payload-manifest.json").Content
        if ($previousText -is [byte[]]) { $previousText = [Text.Encoding]::UTF8.GetString($previousText) }
        $previous = ($previousText -replace '^\xEF\xBB\xBF', '').TrimStart([char]0xFEFF) | ConvertFrom-Json
        if (-not $previous.components.fx) { throw "release $sourceTag has no fx component" }
        $carriedFx = $previous.components.fx
        Write-Host "Carrying the FX component over from $sourceTag ($($carriedFx.asset))."
    } catch {
        throw @"
-SkipFx needs an existing FX component to carry over, and none could be read:
  $($_.Exception.Message)

Either pass -FxFromTag <tag> pointing at a release whose payload-manifest.json has an fx entry,
or drop -SkipFx and repackage the FX pack. Publishing a manifest without one would leave every
fresh install with no particle FX.
"@
    }
}

Remove-Item -LiteralPath $stagingDir -Recurse -Force

# ---- manifest --------------------------------------------------------------------------------

$manifestComponents = [ordered]@{}
foreach ($name in $components.Keys) {
    $entry = $components[$name]
    $manifestComponents[$name] = [ordered]@{
        version = $Version
        asset   = $entry.Asset
        bytes   = $entry.Bytes
        sha256  = $entry.Sha256
        url     = "https://github.com/$owner/$repo/releases/download/$Tag/$($entry.Asset)"
    }
}

# The carried-over FX entry keeps its ORIGINAL absolute url, so it still points at the release
# that actually holds the asset rather than the tag being cut now.
if ($carriedFx) {
    $manifestComponents['fx'] = [ordered]@{
        version = $carriedFx.version
        asset   = $carriedFx.asset
        bytes   = $carriedFx.bytes
        sha256  = $carriedFx.sha256
        url     = $carriedFx.url
    }
}

# schema must stay in step with MANIFEST_SCHEMA in electron/runtime-payload.cjs. Bumping it
# makes older launchers refuse the payload and tell the user to update the app first.
$manifest = [ordered]@{
    schema     = 1
    version    = $Version
    tag        = $Tag
    components = $manifestComponents
}

$manifestPath = Join-Path $outputDir 'payload-manifest.json'
# UTF-8 with NO byte order mark. Windows PowerShell's -Encoding utf8 emits a BOM, which every
# JSON parser downstream then has to tolerate -- and PowerShell's own ConvertFrom-Json does not.
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6),
    (New-Object Text.UTF8Encoding($false)))

Write-Host ''
Write-Host "Payload $Version staged for tag $Tag in:"
Write-Host "  $outputDir"
foreach ($name in $components.Keys) {
    $entry = $components[$name]
    $megabytes = [math]::Round($entry.Bytes / 1MB, 1)
    Write-Host ("  {0,-8} {1,-38} {2,7} MB  {3}" -f $name, $entry.Asset, $megabytes, $entry.Sha256.Substring(0, 12))
}
if ($carriedFx) {
    $fxMegabytes = [math]::Round($carriedFx.bytes / 1MB, 1)
    Write-Host ("  {0,-8} {1,-38} {2,7} MB  carried over, not re-uploaded" -f 'fx', $carriedFx.asset, $fxMegabytes)
}
Write-Host ''
Write-Host 'Upload these to the release with:'
Write-Host "  gh release create $Tag --repo $owner/$repo --prerelease --title `"SOURCE:MVM $Version`" payload-dist\*"
