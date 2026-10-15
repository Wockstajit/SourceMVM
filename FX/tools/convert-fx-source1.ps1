#requires -Version 5
<#
Converts the committed Source 1 FX pack trees for the CS2 runtime effect system.

Both packs' Source 1 inputs live in-repo under FX\sources\, so a normal build needs no game
install beyond CS2 itself:

  On / Less -> FX\sources\insurgency2\          -> particles/filmmaker/insurgency/<pcf>/...
  Modern    -> FX\sources\modern-warfare-gmod\  -> particles/filmmaker/modern/<pcf>/...

Those are the namespaces AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp swaps to. On and
Less are two selections WITHIN the single Insurgency tree (Less picks Insurgency's own _cheap
systems where it authored one), not two separately converted trees -- see the mode notes in
ParticleFxRules.cpp.

Requires a checkout of https://github.com/long0900/source1import with its Python
dependencies installed, plus `pip install srctools vtf2img` for lossless VTF export.

Usage:
  powershell -ExecutionPolicy Bypass -File FX/tools/convert-fx-source1.ps1 `
    -Source1ImportDir C:\path\to\source1import -Compile
#>
[CmdletBinding()]
param(
    # Tool inputs default to repo-local copies so a fresh checkout converts without any
    # machine-specific setup: the converters are cloned under Misc\ and both packs' Source 1
    # asset trees are committed under FX\sources\.
    [string]$Source1ImportDir = '',
    [string]$Source2ConverterDir = '',
    [string]$Python = 'python',
    [string]$OutputRoot,
    # Both empty by default and filled in below the param block: they used to carry the author's
    # own Steam library path, which no other machine has. See the resolution just after param().
    [string]$Cs2Dir = '',
    [string]$ResourceCompiler = '',
    # Source2Viewer-CLI (ValveResourceFormat). Not a build dependency -- only the asset
    # conversion/research tools below need it. Resolved as a checkout sitting NEXT TO this
    # repository, which is the documented layout; set SOURCEMVM_VRF_CLI to point elsewhere.
    [string]$VrfCli = '',
    # MW2019 "modern" pack sources. These are now COMMITTED to the repo under
    # FX\sources\modern-warfare-gmod\ (the ~44 MB extracted Source 1 tree), so a normal
    # build never touches a GMod install. The GMod paths below are only used when the
    # committed tree is absent, or when -RefreshModernFromGmod is passed to re-extract it
    # from an updated GMod (writes straight back into the committed folder).
    [string]$ModernSourceDir = '',
    # Only read when the committed tree above is missing or -RefreshModernFromGmod is passed, and
    # both are Test-Path guarded before use, so an unset default just means "no GMod here".
    # Point GMOD_DIR at a Garry's Mod install to re-extract; the workshop root is derived from it.
    [string]$GmodWorkshopRoot = $(if ($env:GMOD_WORKSHOP_DIR) { $env:GMOD_WORKSHOP_DIR } elseif ($env:GMOD_DIR) { Join-Path (Split-Path -Parent (Split-Path -Parent $env:GMOD_DIR)) 'workshop\content\4000' } else { '' }),
    [string]$GmodRoot = $(if ($env:GMOD_DIR) { $env:GMOD_DIR } else { '' }),
    [switch]$RefreshModernFromGmod,
    [switch]$SkipModern,
    # Insurgency 2 "insurgency" pack sources (the On/Less asset source since 2026-09-01).
    # Committed under FX\sources\insurgency2\ (the ~54 MB extracted Source 1 tree) exactly
    # like the Modern tree above, so a normal build never touches an Insurgency install.
    # -RefreshInsurgencyFromGame re-extracts it from a local install into the committed
    # folder; $InsurgencyRoot is only read then (the extractor auto-detects Steam otherwise).
    [string]$InsurgencySourceDir = '',
    [string]$InsurgencyRoot = $(if ($env:SOURCEMVM_INSURGENCY2_DIR) { $env:SOURCEMVM_INSURGENCY2_DIR } else { '' }),
    [switch]$RefreshInsurgencyFromGame,
    # money-on-headshot sources (its own committed tree; see the staging block below).
    [string]$MoneySourceDir = '',
    [switch]$Compile,
    # Publish even when the build has fewer particle systems than the published pack.
    [switch]$AllowSmallerPack
)

$ErrorActionPreference = 'Stop'

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This used to
# default to the author's own Steam library, a path no other machine has. Override with -Cs2Dir
# or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\..\Automation\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass -Cs2Dir <install root> or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($ResourceCompiler)) { $ResourceCompiler = Join-Path $Cs2Dir 'game\bin\win64\resourcecompiler.exe' }

function Resolve-ExistingPath([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label is required."
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Ensure-Directory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-Under([string]$Path, [string]$Root, [string]$Label) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (-not ($fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "$Label must stay under $fullRoot, got $fullPath"
    }
}

function Copy-TreeContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { return }
    Ensure-Directory $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}




function Remove-GeneratedJunction([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        throw "Refusing to remove non-junction generated dependency path: $Path"
    }
    # Delete ONLY the reparse point. Remove-Item (without -Recurse) sees the junction's target
    # children and blocks on an interactive "has children ... Are you sure?" confirmation --
    # which hangs/kills the build.bat pipeline run; -Recurse is NOT a safe fix (on some
    # PowerShell versions it follows the junction and deletes the REAL CS2 game files it points
    # at). Directory.Delete(non-recursive) is the junction-safe, prompt-free way.
    [System.IO.Directory]::Delete($item.FullName, $false)
}

function Ensure-GeneratedJunction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) {
        Remove-GeneratedJunction $Path
    }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path

# Resolved here (not as param() defaults) because $PSScriptRoot reads empty during
# param-block default evaluation under `powershell -File` in this environment (repro'd
# with [CmdletBinding()] + a $PSScriptRoot-based default; harmless without CmdletBinding,
# but keeping CmdletBinding and resolving defaults in the body is the robust fix).
if ([string]::IsNullOrWhiteSpace($VrfCli)) {
    $VrfCli = if ($env:SOURCEMVM_VRF_CLI) { $env:SOURCEMVM_VRF_CLI } else { Join-Path (Split-Path -Parent $repoRoot) 'ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe' }
}
if ([string]::IsNullOrWhiteSpace($Source1ImportDir)) {
    $Source1ImportDir = if ($env:SOURCE1IMPORT_DIR) { $env:SOURCE1IMPORT_DIR } else { Join-Path $repoRoot 'Misc\source1import' }
}
if ([string]::IsNullOrWhiteSpace($Source2ConverterDir)) {
    $Source2ConverterDir = if ($env:SOURCE2CONVERTER_DIR) { $env:SOURCE2CONVERTER_DIR } else { Join-Path $repoRoot 'Misc\Source2Converter' }
}
# The MW2019 "modern" pack Source 1 tree lives in-repo so builds don't need a GMod install.
if ([string]::IsNullOrWhiteSpace($ModernSourceDir)) {
    $ModernSourceDir = if ($env:MODERN_SOURCE_DIR) { $env:MODERN_SOURCE_DIR } else { Join-Path $repoRoot 'FX\sources\modern-warfare-gmod' }
}
# Likewise the Insurgency 2 tree: committed, so builds don't need an Insurgency install.
if ([string]::IsNullOrWhiteSpace($InsurgencySourceDir)) {
    $InsurgencySourceDir = if ($env:INSURGENCY_SOURCE_DIR) { $env:INSURGENCY_SOURCE_DIR } else { Join-Path $repoRoot 'FX\sources\insurgency2' }
}
if ([string]::IsNullOrWhiteSpace($MoneySourceDir)) {
    $MoneySourceDir = if ($env:MONEY_SOURCE_DIR) { $env:MONEY_SOURCE_DIR } else { Join-Path $repoRoot 'FX\sources\povarehok-money' }
}

# Conversion and compilation work under build\fx (git-ignored). A validated -Compile run then
# publishes the compiled game dir to FX\compiled\source_mvm_fx (committed through git LFS), which
# build-source2.bat stages into the build. Retail CS2 no longer ships resourcecompiler.exe, so the
# committed pack is what lets a clone launch with custom particles.
$safeOutputRoot = Ensure-Directory (Join-Path $repoRoot 'build\fx')
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $safeOutputRoot 'fx-source1import'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
Assert-Under $OutputRoot $safeOutputRoot 'OutputRoot'

$source1ImportRoot = Resolve-ExistingPath $Source1ImportDir 'Source1ImportDir'
$source1ImportUtils = Resolve-ExistingPath (Join-Path $source1ImportRoot 'utils') 'source1import utils folder'
$materialsImport = Resolve-ExistingPath (Join-Path $source1ImportUtils 'materials_import.py') 'materials_import.py'
$particlesImport = Resolve-ExistingPath (Join-Path $source1ImportUtils 'particles_import.py') 'particles_import.py'
$textureExporter = Resolve-ExistingPath (Join-Path $PSScriptRoot 'export-source1-vtf.py') 'VTF texture exporter'
$particlePostprocess = Resolve-ExistingPath (Join-Path $PSScriptRoot 'postprocess_insurgency.py') 'Insurgency post-process tool'
$lessVariantBuilder = Resolve-ExistingPath (Join-Path $PSScriptRoot 'build-less-variants.py') 'Less variant builder'
$impactSmokeVariantBuilder = Resolve-ExistingPath (Join-Path $PSScriptRoot 'build-impact-smoke-variants.py') 'impact smoke variant builder'
$impactSurfaceLayerBuilder = Resolve-ExistingPath (Join-Path $PSScriptRoot 'layer-vanilla-impacts.py') 'surface impact layer builder'
$legacyImpactMerger = Resolve-ExistingPath (Join-Path $PSScriptRoot 'merge-legacy-povarehok-impacts.py') 'legacy Povarhok impact merger'
$legacyWeaponSmokeLayer = Resolve-ExistingPath (Join-Path $PSScriptRoot 'layer-legacy-weapon-smoke.py') 'legacy weapon-smoke layer'
$legacy3dDebrisStripper = Resolve-ExistingPath (Join-Path $PSScriptRoot 'strip-povarehok-3d-debris.py') 'Povarhok 3D debris stripper'
$povarehokSparkScaler = Resolve-ExistingPath (Join-Path $PSScriptRoot 'scale-povarehok-metal-sparks.py') 'Povarhok metal-spark scaler'
$nadeImpactFeather = Resolve-ExistingPath (Join-Path $PSScriptRoot 'feather-nade-impact-smoke.py') 'nade/impact smoke feather'
$modernHeTrailAnim = Resolve-ExistingPath (Join-Path $PSScriptRoot 'animate-modern-he-trails.py') 'Modern HE smoke-trail flow'
$additiveAlphaRepair = Resolve-ExistingPath (Join-Path $PSScriptRoot 'repair-additive-alpha.py') 'additive alpha repair'
$source2ConverterRoot = Resolve-ExistingPath $Source2ConverterDir 'Source2ConverterDir'
$modelConverter = Resolve-ExistingPath (Join-Path $source2ConverterRoot 'convert_model.py') 'Source2Converter convert_model.py'

if (Test-Path -LiteralPath $OutputRoot) {
    Assert-Under (Resolve-Path -LiteralPath $OutputRoot).Path $safeOutputRoot 'OutputRoot'
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo_imported')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\csgo_core')
    Remove-GeneratedJunction (Join-Path $OutputRoot 'source2\game\core')
    Remove-Item -LiteralPath $OutputRoot -Recurse -Force
}

$src1Root = Ensure-Directory (Join-Path $OutputRoot 'source1_game')
$source2Root = Ensure-Directory (Join-Path $OutputRoot 'source2')
$contentDir = Ensure-Directory (Join-Path $source2Root 'content\source_mvm_fx')
$gameDir = Ensure-Directory (Join-Path $source2Root 'game\source_mvm_fx')
$csgoGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo') 'CS2 game\csgo folder'
$csgoImportedGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo_imported') 'CS2 game\csgo_imported folder'
$csgoCoreGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\csgo_core') 'CS2 game\csgo_core folder'
$coreGameDir = Resolve-ExistingPath (Join-Path $Cs2Dir 'game\core') 'CS2 game\core folder'
$csgoJunction = Join-Path $source2Root 'game\csgo'
$csgoImportedJunction = Join-Path $source2Root 'game\csgo_imported'
$csgoCoreJunction = Join-Path $source2Root 'game\csgo_core'
$coreJunction = Join-Path $source2Root 'game\core'
Ensure-GeneratedJunction $csgoJunction $csgoGameDir
Ensure-GeneratedJunction $csgoImportedJunction $csgoImportedGameDir
Ensure-GeneratedJunction $csgoCoreJunction $csgoCoreGameDir
Ensure-GeneratedJunction $coreJunction $coreGameDir

# ---- Insurgency 2 pack staging (particles/filmmaker/insurgency/...) ----
# The Insurgency pack's Source 1 tree (11 Insurgency 2 PCFs + their material closure) is
# COMMITTED in-repo under FX\sources\insurgency2\, exactly like the Modern tree below, so a
# normal build reads it directly with no Insurgency install required.
#
# This REPLACED the old CS:GO "Povarehok" mod as the On/Less asset source (2026-09-01). That
# mod's 214-material closure was assembled from ~12 donor games with mutually inconsistent
# VMT conventions, which is the root cause behind essentially every texture-failure pattern
# in Docs/porting/povarehok-csgo-mod-reference.md §10 -- and it lived in a git-ignored ~779 MB
# folder no other machine had. Insurgency 2 ships a complete, self-consistent FX set that
# inherited Source's impact/tracer naming, so it maps onto CS2's system names almost 1:1.
#
# -RefreshInsurgencyFromGame re-extracts the committed tree from a local Insurgency 2 install
# first (for pulling in an updated game); otherwise the install is never touched.
if ($RefreshInsurgencyFromGame) {
    $insurgencyExtractor = Resolve-ExistingPath (Join-Path $PSScriptRoot 'extract-insurgency-fx.py') 'Insurgency FX extractor'
    Write-Host "Refreshing committed Insurgency source tree: $InsurgencySourceDir" -ForegroundColor Cyan
    $extractArgs = @($insurgencyExtractor, '--output', $InsurgencySourceDir)
    if ($InsurgencyRoot) { $extractArgs += @('--insurgency-root', $InsurgencyRoot) }
    & $Python @extractArgs
    if ($LASTEXITCODE -ne 0) { throw "Insurgency pack re-extraction failed with exit code $LASTEXITCODE" }
}

$insurgencyPcfSrc = Resolve-ExistingPath (Join-Path $InsurgencySourceDir 'particles\filmmaker\insurgency') 'committed Insurgency PCF tree'
Write-Host "Staging committed Insurgency particle sources from: $InsurgencySourceDir" -ForegroundColor Cyan
Copy-TreeContents (Join-Path $InsurgencySourceDir 'materials') (Join-Path $src1Root 'materials')
$insurgencyPcfDst = Ensure-Directory (Join-Path $src1Root 'particles\filmmaker\insurgency')
$stagedPcfs = 0
Get-ChildItem -LiteralPath $insurgencyPcfSrc -Filter '*.pcf' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $insurgencyPcfDst -Force
    $stagedPcfs++
}
if ($stagedPcfs -eq 0) { throw "No .pcf files found under $insurgencyPcfSrc" }

# ---- money-on-headshot staging (particles/filmmaker/money/...) ----
# Its OWN committed tree (FX\sources\povarehok-money\, ~6 MB) because it is its own feature
# with its own runtime toggle -- `mirv_filmmaker fx moneyshot`, ParticleFxMoney.cpp -- and it
# is the one Povarehok effect that survived the Insurgency rebake (user request 2026-09-01):
# Insurgency ships no money burst, so there was nothing to re-source it from. Extracted once by
# FX\tools\extract-povarehok-money.py; the mod folder is never read at build time.
$moneyPcfSrc = Resolve-ExistingPath (Join-Path $MoneySourceDir 'particles\filmmaker\money') 'committed money PCF tree'
Write-Host "Staging committed money-on-headshot sources from: $MoneySourceDir" -ForegroundColor Cyan
foreach ($sub in @('materials', 'models')) {
    Copy-TreeContents (Join-Path $MoneySourceDir $sub) (Join-Path $src1Root $sub)
}
$moneyPcfDst = Ensure-Directory (Join-Path $src1Root 'particles\filmmaker\money')
Get-ChildItem -LiteralPath $moneyPcfSrc -Filter '*.pcf' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $moneyPcfDst -Force
}

# ---- MW2019 "modern" pack staging (particles/filmmaker/modern/...) ----
# The Modern pack's Source 1 tree (3 ARC9/MW2019 PCFs + their material closure + the four
# mw2019 brass shell models its casing systems render) is COMMITTED in-repo under
# FX\sources\modern-warfare-gmod\, so a normal build reads it directly with no GMod install
# required. It gets merged into the same source1 conversion root so one materials/particles
# import pass covers both packs.
#
# MODERN WINS same-path collisions with the Insurgency tree staged above (17 of them,
# measured 2026-09-01 -- all stock Source sprites both donors happen to ship: particle_flares,
# vistasmokev1, antlion_goop3, blood_mist, splashwake, spark). Modern's converted look is
# already validated in-game and Insurgency is the new, untested input, so the overwrite keeps
# a working pack bit-identical rather than risking a silent regression in it.
#
# -RefreshModernFromGmod re-extracts that committed tree from a local GMod install first
# (for pulling in an updated addon); otherwise the GMod paths are never touched.
$modernAvailable = $false
if (-not $SkipModern) {
    if ($RefreshModernFromGmod) {
        $modernExtractor = Join-Path $PSScriptRoot 'extract-modern-particles-gmod.py'
        if ((Test-Path -LiteralPath $GmodWorkshopRoot) -and (Test-Path -LiteralPath $GmodRoot)) {
            Write-Host "Refreshing committed Modern source tree from GMod: $ModernSourceDir" -ForegroundColor Cyan
            & $Python $modernExtractor --workshop-root $GmodWorkshopRoot --gmod-root $GmodRoot --output $ModernSourceDir
            if ($LASTEXITCODE -ne 0) {
                throw "Modern pack re-extraction from GMod failed with exit code $LASTEXITCODE"
            }
        } else {
            throw "-RefreshModernFromGmod requires a GMod install; not found at $GmodWorkshopRoot / $GmodRoot"
        }
    }

    $modernPcfSrc = Join-Path $ModernSourceDir 'particles\filmmaker\modern'
    if (Test-Path -LiteralPath $modernPcfSrc) {
        Write-Host "Staging committed MW2019 modern particle sources from: $ModernSourceDir" -ForegroundColor Cyan
        # 'templates' carries Modern's committed casing VPCF templates; postprocess_modern
        # reads them from the staged source1 tree (see CASING_TEMPLATE_SUBDIR).
        foreach ($sub in @('materials', 'models', 'templates')) {
            $modernSub = Join-Path $ModernSourceDir $sub
            if (-not (Test-Path -LiteralPath $modernSub)) { continue }
            Get-ChildItem -LiteralPath $modernSub -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($ModernSourceDir.Length + 1)
                $dst = Join-Path $src1Root $rel
                Ensure-Directory (Split-Path -Parent $dst) | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
            }
        }
        $modernPcfDst = Ensure-Directory (Join-Path $src1Root 'particles\filmmaker\modern')
        Get-ChildItem -LiteralPath $modernPcfSrc -Filter '*.pcf' | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $modernPcfDst -Force
        }
        $modernAvailable = $true
    } else {
        Write-Warning "Committed Modern source tree not found at $ModernSourceDir; skipping the MW2019 modern pack. (Run with -RefreshModernFromGmod, or restore FX\sources\modern-warfare-gmod\.)"
    }
}

# NOTE: the two stock CS:GO gib models (concrete_chunk07a / wood_gib01b) that used to be
# staged here are gone with the Povarehok mod (2026-09-01). They existed only for that mod's
# RenderModels impact debris; Insurgency 2's impact systems are entirely sprite-based (its 11
# PCFs contain exactly one .mdl reference, an unused HL2 placeholder in ins_burning_fx), so
# the pack now has no Source 1 model dependency of its own. The only models still converted
# are the Modern pack's four mw2019 brass casings, staged from its committed tree above.

@'
"GameInfo"
{
    game "SOURCE:MVM FX Import"
    FileSystem
    {
        SearchPaths
        {
            Game "."
        }
    }
}
'@ | Set-Content -LiteralPath (Join-Path $src1Root 'gameinfo.txt') -Encoding ASCII

@"
"GameInfo"
{
    game "SOURCE:MVM FX"
    title "SOURCE:MVM FX"
    FileSystem
    {
        SearchPaths
        {
            Game source_mvm_fx
            Game csgo
            Game csgo_imported
            Game csgo_core
            Game core
            Mod source_mvm_fx
            Mod csgo
            Mod csgo_imported
            Mod csgo_core
        }
    }
}
"@ | Set-Content -LiteralPath (Join-Path $gameDir 'gameinfo.gi') -Encoding ASCII

Write-Host "Staged FX pack sources under: $src1Root" -ForegroundColor Green
Write-Host "Exporting original VTF textures for source1import..." -ForegroundColor Cyan
$textureManifest = Join-Path $OutputRoot 'texture-export.json'
& $Python $textureExporter --input (Join-Path $src1Root 'materials') --output (Join-Path $contentDir 'materials') --manifest $textureManifest
if ($LASTEXITCODE -ne 0) { throw "VTF texture export failed with exit code $LASTEXITCODE" }

# Generated VMATs reference source1import's standard default textures. CS2 ships the
# authoritative copies in content/core; keep them in this isolated content root so
# ResourceCompiler can resolve associated VTEX inputs.
$defaultMaterialSource = Resolve-ExistingPath (Join-Path $Cs2Dir 'content\core\materials\default') 'CS2 default material sources'
$defaultMaterialDestination = Ensure-Directory (Join-Path $contentDir 'materials\default')
foreach ($name in @('default.tga', 'default_trans.tga', 'default_rough_s1import.tga')) {
    $source = Resolve-ExistingPath (Join-Path $defaultMaterialSource $name) "CS2 default texture $name"
    Copy-Item -LiteralPath $source -Destination (Join-Path $defaultMaterialDestination $name) -Force
}

Write-Host "Running source1import material conversion..." -ForegroundColor Cyan
Push-Location $source1ImportUtils
try {
    & $Python $materialsImport -i $src1Root -e $contentDir -b cs2
    if ($LASTEXITCODE -ne 0) { throw "materials_import.py failed with exit code $LASTEXITCODE" }

    Write-Host "Running source1import particle conversion..." -ForegroundColor Cyan
    & $Python $particlesImport -i $src1Root -e $contentDir -b cs2
    if ($LASTEXITCODE -ne 0) { throw "particles_import.py failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "Applying CS2-specific VPCF post-process..." -ForegroundColor Cyan
Write-Host "  (scans ~7k converted .vpcf files; progress lines follow, usually ~30-90s)" -ForegroundColor DarkGray
$env:PYTHONUNBUFFERED = "1"
& $Python $particlePostprocess --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "FX post-process failed with exit code $LASTEXITCODE" }

Write-Host "Generating reduced and probabilistic-smoke impact variants..." -ForegroundColor Cyan
& $Python $lessVariantBuilder --content-root $contentDir --subtree particles/filmmaker/insurgency/impact_fx_ins
if ($LASTEXITCODE -ne 0) { throw "Less variant generation failed with exit code $LASTEXITCODE" }
$legacyFxRoot = Join-Path $safeOutputRoot 'povarehok-source1import\source2'
$legacyContentRoot = Join-Path $legacyFxRoot 'content\source_mvm_fx'
$legacyGameRoot = Join-Path $legacyFxRoot 'game\source_mvm_fx'
if (-not (Test-Path -LiteralPath $legacyContentRoot) -or -not (Test-Path -LiteralPath $legacyGameRoot)) {
    throw "The hybrid Impact mode needs the old GitHub build's reduced Povarhok pack at $legacyFxRoot"
}
& $Python $legacyImpactMerger --legacy-content-root $legacyContentRoot --legacy-game-root $legacyGameRoot `
    --content-root $contentDir --game-root $gameDir
if ($LASTEXITCODE -ne 0) { throw "Legacy Povarhok impact merge failed with exit code $LASTEXITCODE" }
& $Python $legacyWeaponSmokeLayer --legacy-content-root $legacyContentRoot --legacy-game-root $legacyGameRoot `
    --content-root $contentDir --game-root $gameDir
if ($LASTEXITCODE -ne 0) { throw "Legacy weapon-smoke layer failed with exit code $LASTEXITCODE" }
& $Python $legacy3dDebrisStripper --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Povarhok 3D impact-debris stripping failed with exit code $LASTEXITCODE" }
& $Python $povarehokSparkScaler --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Povarhok metal-spark scale failed with exit code $LASTEXITCODE" }
& $Python $impactSmokeVariantBuilder --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Impact smoke variant generation failed with exit code $LASTEXITCODE" }
& $Python $nadeImpactFeather --content-root $contentDir --legacy-content-root $legacyContentRoot
if ($LASTEXITCODE -ne 0) { throw "Nade/impact smoke feather failed with exit code $LASTEXITCODE" }
& $Python $modernHeTrailAnim --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Modern HE smoke-trail flow failed with exit code $LASTEXITCODE" }

# Layering copies old unlit smoke assets, so normalize lighting after all compositions.
& $Python (Join-Path $PSScriptRoot 'light-weapon-smoke.py') --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Weapon-smoke lighting failed with exit code $LASTEXITCODE" }

# LAST material pass before compile: give every additive sprite whose alpha channel is opaque a
# translucency map with a shape in it. Without this an additive glow composites its WHOLE QUAD
# and draws as a hard-edged rectangle of its own colour (the HE grenade's white slab and the
# red-orange shrapnel squares). It ran by hand until now, which meant a rebuild silently undid
# it -- and it also carries the coverage gamma that keeps an additive sprite from being shaped
# twice, i.e. what stops the On-mode muzzle flash rendering ~4x too dim.
& $Python $additiveAlphaRepair --content-root $contentDir
if ($LASTEXITCODE -ne 0) { throw "Additive alpha repair failed with exit code $LASTEXITCODE" }

# Build per-surface vanilla + donor wrappers after donor tuning. Wall closures
# enlarge only local smoke sprites, leaving shared carpet/snow systems unchanged.
& $Python $impactSurfaceLayerBuilder --content-root $contentDir --rules (Join-Path $repoRoot 'AfxHookSource2\Filmmaker\Movie\ParticleFxRules.cpp')
if ($LASTEXITCODE -ne 0) { throw "Surface impact layering failed with exit code $LASTEXITCODE" }

# Current CS2 rejects source1import's legacy m_sMDLFilename wrapper. Convert the
# mod-bundled MDL/VVD/VTX files to ModelDoc 28 with REDxEYE/Source2Converter.
Write-Host "Running Source2Converter model conversion..." -ForegroundColor Cyan
& $Python $modelConverter -g CS2 -a $contentDir -m (Join-Path $src1Root 'models')
if ($LASTEXITCODE -ne 0) { throw "Source2Converter model conversion failed with exit code $LASTEXITCODE" }

# Several bundled files have an internal MDL name that differs from their actual Source 1
# file path. Particle definitions use the file path, so provide exact-path aliases to the
# same converted ModelDoc and DMX data.
$modelAliases = @{
    'models\usMoney\usmoney bills.vmdl' = 'models\usMoney\usmoney_bills.vmdl'
    'models\shells\shell_12gauge.vmdl' = 'models\shells\mw2019\shell_12gauge.vmdl'
    'models\shells\shell_50cal.vmdl' = 'models\shells\mw2019\shell_50cal.vmdl'
    'models\shells\shell_pistol.vmdl' = 'models\shells\mw2019\shell_pistol.vmdl'
    'models\shells\shell_rifle.vmdl' = 'models\shells\mw2019\shell_rifle.vmdl'
}
foreach ($alias in $modelAliases.GetEnumerator()) {
    $source = Resolve-ExistingPath (Join-Path $contentDir $alias.Value) "converted model $($alias.Value)"
    $destination = Join-Path $contentDir $alias.Key
    Ensure-Directory (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

# NOTE: This script is particle FX only; it must never build, compile, graft, or
# stage player ragdoll models.

# Include authored Source 2 particles alongside converted pack inputs before closure pruning.
$nativeContent = Join-Path $repoRoot 'FX\native'
if (Test-Path -LiteralPath $nativeContent) {
    Copy-Item -Path (Join-Path $nativeContent '*') -Destination $contentDir -Recurse -Force
}

if ($Compile) {
    $resourceCompilerPath = Resolve-ExistingPath $ResourceCompiler 'ResourceCompiler'
    Resolve-ExistingPath (Join-Path $gameDir 'gameinfo.gi') 'generated gameinfo.gi' | Out-Null

    # ---- Prune the content tree to the runtime closure BEFORE compiling ----
    # The mod ships far more than the DLL's swap tables ever reference (measured
    # 2026-07-03: 969 MB / 8,743 compiled files, of which only ~143 MB / ~930 files
    # are reachable from the DLL's swap tables -- the rest is bundled third-party material
    # packs and unswapped particle systems). The validator derives the exact keep-set
    # from the ParticleFx sources themselves (we pass ParticleFx.cpp; the validator scans
    # every sibling ParticleFx* file, since the FXRULE tables live in ParticleFxRules.cpp
    # after the 2026-07-04 split -- so a new FXRULE automatically keeps its assets);
    # everything else is deleted from this generated tree so resourcecompiler never
    # sees it. Cuts the shipped pack ~85% and the compile time by minutes.
    $validator = Resolve-ExistingPath (Join-Path $PSScriptRoot 'validate-fx-assets.py') 'FX asset validator'
    $particleFxCpp = Resolve-ExistingPath (Join-Path $repoRoot 'AfxHookSource2\Filmmaker\Movie\ParticleFx.cpp') 'ParticleFx.cpp'
    $validationReport = Join-Path $OutputRoot 'validation.json'
    $runtimeRoots = Join-Path $OutputRoot 'runtime-roots.txt'
    $closureKeepList = Join-Path $OutputRoot 'closure-keep.txt'
    Write-Host "Computing runtime resource closure for pruning..." -ForegroundColor Cyan
    $closureArgs = @('--particle-fx-cpp', $particleFxCpp, '--content-root', $contentDir,
        '--game-root', $gameDir, '--file-list', $runtimeRoots, '--report', $validationReport,
        '--emit-closure', $closureKeepList)
    if ($modernAvailable) { $closureArgs += '--require-modern' }
    & $Python $validator @closureArgs
    if ($LASTEXITCODE -ne 0) { throw "Runtime closure computation failed (see $validationReport); refusing to prune/compile." }

    $keepSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in Get-Content -LiteralPath $closureKeepList) {
        if (-not [string]::IsNullOrWhiteSpace($line)) { [void]$keepSet.Add($line.Trim().Replace('\', '/')) }
    }
    if ($keepSet.Count -lt 100) { throw "Closure keep-set suspiciously small ($($keepSet.Count) entries); refusing to prune." }
    $contentPrefix = (Resolve-Path -LiteralPath $contentDir).Path
    $pruned = 0; $kept = 0
    # Only compile-group inputs are pruned (.vpcf/.vmat/.vtex); tga/mks/dmx inputs are
    # pulled on demand by whatever kept file references them, so they can stay.
    # materials\default\ holds the compiler's own fallback textures -- never prune it.
    foreach ($sub in @('materials', 'particles\filmmaker')) {
        $dir = Join-Path $contentDir $sub
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object {
            $_.Extension -in @('.vpcf', '.vmat', '.vtex')
        } | ForEach-Object {
            $rel = $_.FullName.Substring($contentPrefix.Length + 1).Replace('\', '/')
            if ($rel.StartsWith('materials/default/', [StringComparison]::OrdinalIgnoreCase)) { $kept++; return }
            if ($keepSet.Contains($rel)) { $kept++ } else { Remove-Item -LiteralPath $_.FullName -Force; $pruned++ }
        }
    }
    Write-Host ("Pruned content tree to runtime closure: kept {0}, removed {1} unreferenced vpcf/vmat/vtex sources." -f $kept, $pruned) -ForegroundColor Green

    $compileGroups = @(
        @{ Label = 'materials'; Input = Join-Path $contentDir 'materials\*.vmat'; AllowPartial = $true },
        @{ Label = 'direct particle textures'; Input = Join-Path $contentDir 'materials\*.vtex'; AllowPartial = $false },
        @{ Label = 'models'; Input = Join-Path $contentDir 'models\*.vmdl'; AllowPartial = $false },
        @{ Label = 'insurgency particle systems'; Input = Join-Path $contentDir 'particles\filmmaker\insurgency\*.vpcf'; AllowPartial = $false },
        @{ Label = 'money-on-headshot particle systems'; Input = Join-Path $contentDir 'particles\filmmaker\money\*.vpcf'; AllowPartial = $false }
    )
    if ($modernAvailable) {
        $compileGroups += @{ Label = 'modern particle systems'; Input = Join-Path $contentDir 'particles\filmmaker\modern\*.vpcf'; AllowPartial = $false }
    }
    $compileGroups += @{ Label = 'hybrid impact smoke'; Input = Join-Path $contentDir 'particles\filmmaker\povarehok\less\impacts\impact_fx\*.vpcf'; AllowPartial = $false }
    $compileGroups += @{ Label = 'hybrid surface impact layers'; Input = Join-Path $contentDir 'particles\filmmaker\povarehok\less\impacts\impact_layered\*.vpcf'; AllowPartial = $false }
    $compileGroups += @{ Label = 'Povarehok weapon smoke'; Input = Join-Path $contentDir 'particles\filmmaker\povarehok\regular\weapons\*.vpcf'; AllowPartial = $false }
    $legacyHeGlob = Join-Path $legacyContentRoot 'particles\filmmaker\povarehok\regular\explosions_fx\*.vpcf'
    if (Test-Path -LiteralPath (Split-Path -Parent $legacyHeGlob)) {
        # Its sources live in the LEGACY content tree, so it compiles into the legacy game root
        # (the compiler maps content to game by root); stage-legacy-he.py copies it across below.
        $compileGroups += @{ Label = 'Povarehok HE explosion'; Input = $legacyHeGlob; AllowPartial = $false; Game = $legacyGameRoot }
    }
    foreach ($group in $compileGroups) {
        if (-not (Get-ChildItem (Split-Path -Parent $group.Input) -Recurse -File -ErrorAction SilentlyContinue)) {
            Write-Host "No converted $($group.Label) found; skipping." -ForegroundColor Yellow
            continue
        }
        Write-Host "Compiling converted Source 2 $($group.Label)..." -ForegroundColor Cyan
        $groupGame = if ($group.Game) { $group.Game } else { $gameDir }
        & $resourceCompilerPath -i $group.Input -r -game $groupGame -nop4 -fshallow2
        if ($LASTEXITCODE -ne 0) {
            if ($group.AllowPartial) {
                Write-Warning "resourcecompiler.exe reported failures while compiling $($group.Label); continuing so runtime validation can check the actual particle closure."
            } else {
                throw "resourcecompiler.exe failed while compiling $($group.Label) with exit code $LASTEXITCODE"
            }
        }
    }

    # The Povarehok HE (HE On/Less) compiles into the LEGACY game root above; the mounted pack is
    # built from $gameDir. Copy its closure (and any legacy-only texture a staged material names)
    # across, or the swap target is missing and HE On silently plays vanilla (2026-09-26).
    $legacyHeStager = Resolve-ExistingPath (Join-Path $PSScriptRoot 'stage-legacy-he.py') 'legacy HE stager'
    & $Python $legacyHeStager --legacy-game-root $legacyGameRoot --legacy-content-root $legacyContentRoot --game-root $gameDir
    if ($LASTEXITCODE -ne 0) { throw "Staging the Povarehok HE explosion failed with exit code $LASTEXITCODE" }

    $compiledCounts = @{
        Particles = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vpcf_c').Count
        Materials = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vmat_c').Count
        Textures = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vtex_c').Count
        Models = @(Get-ChildItem $gameDir -Recurse -File -Filter '*.vmdl_c').Count
    }
    foreach ($required in @('Particles', 'Materials', 'Textures')) {
        if ($compiledCounts[$required] -le 0) {
            throw "Compiled asset validation failed: no $required were produced under $gameDir"
        }
    }
    Write-Host ("Compiled assets: {0} particles, {1} materials, {2} textures, {3} models" -f `
        $compiledCounts.Particles, $compiledCounts.Materials, $compiledCounts.Textures, $compiledCounts.Models) -ForegroundColor Green

    Write-Host "Validating runtime FX resource closure..." -ForegroundColor Cyan
    $validatorArgs = @('--particle-fx-cpp', $particleFxCpp, '--content-root', $contentDir,
        '--game-root', $gameDir, '--file-list', $runtimeRoots, '--report', $validationReport,
        '--validate-compiled')
    if ($modernAvailable) { $validatorArgs += '--require-modern' }
    & $Python $validator @validatorArgs
    if ($LASTEXITCODE -ne 0) {
        throw "FX runtime validation failed. See $validationReport"
    }

    # Publish the validated pack. Render materials (materials\filmmaker) are owned by
    # build-render-materials.ps1; game-written cfg\ and the shader cache never belong in it.
    $publishDir = Join-Path $repoRoot 'FX\compiled\source_mvm_fx'
    # Never replace a fuller pack with a smaller one. Per-surface impact layers and smoke
    # variants are now generated above, but retain the guard against other missing content:
    # a /MIR publish would remove it. Pass -AllowSmallerPack to override.
    if (Test-Path -LiteralPath (Join-Path $publishDir 'particles')) {
        $publishedParticles = @(Get-ChildItem (Join-Path $publishDir 'particles') -Recurse -File -Filter '*.vpcf_c').Count
        if ($compiledCounts.Particles -lt [int]($publishedParticles * 0.9) -and -not $AllowSmallerPack) {
            throw ("Refusing to publish: this build has {0} particle systems, the published pack {1}. " +
                "The compiled output stays in {2}.") -f $compiledCounts.Particles, $publishedParticles, $gameDir
        }
    }
    New-Item -ItemType Directory -Force -Path $publishDir | Out-Null
    robocopy $gameDir $publishDir /MIR /XD (Join-Path $gameDir 'cfg') (Join-Path $publishDir 'cfg') (Join-Path $gameDir 'materials\filmmaker') (Join-Path $publishDir 'materials\filmmaker') /XF socache.dt /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Publishing the compiled pack to $publishDir failed (robocopy exit $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0 # robocopy reports copied files as 1-7
    Write-Host "Published compiled pack: $publishDir (commit it; large files go through git LFS)" -ForegroundColor Green
}

Write-Host ""
Write-Host "Converted content: $contentDir" -ForegroundColor Green
Write-Host "Compiled game dir: $gameDir" -ForegroundColor Green
Write-Host "Runtime resource namespaces: particles/filmmaker/insurgency/<pcf>/..."
Write-Host "                             particles/filmmaker/money/impact_fxmoney/... (moneyshot toggle)"
if ($modernAvailable) {
    Write-Host "                             particles/filmmaker/modern/{arc9_fas_muzzleflashes,arc9_fas_explosions,mw2019_tracer}/..."
}
