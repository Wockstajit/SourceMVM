[CmdletBinding()]
param(
    [string]$Cs2Dir = '',
    [string]$ResourceCompiler = '',
    [string]$GeneratedRoot,
    [string]$StageRoot
)

# Compiles the hook's own Panorama textures (FX\ui: the outliner's Lucide icons and the HC Tracker
# badge) into the runtime FX pack and stages them, the way build-render-materials.ps1 handles the
# render-pass materials. Panorama loads them as s2r://panorama/images/<path>_png.vtex, which needs
# the compiled _png.vtex_c beside the engine's own images; a loose PNG never loads in game.
#
# Sources: FX\ui\panorama\images\**\<name>.png with a <name>_png.vtex beside it (RGBA8888, "Box"
# mips, so the PNG must be a power-of-two size). Compiling needs resourcecompiler.exe, which only
# CS2 Workshop Tools install; without it the compiled pack on disk (FX\compiled, git-ignored and
# shipped with releases) is staged unchanged. A hook-only build (cmake --install) does not stage
# FX\compiled, so run this script with -StageRoot after one to put new icons into the stage.

if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\..\Automation\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($ResourceCompiler)) { $ResourceCompiler = Join-Path $Cs2Dir 'game\bin\win64\resourcecompiler.exe' }

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $GeneratedRoot) { $GeneratedRoot = Join-Path $repoRoot 'build\fx\fx-source1import\source2' }
if (-not $StageRoot) { $StageRoot = Join-Path $repoRoot 'build\source2-release\fx\source_mvm_fx' }

$sourceRoot = Join-Path $repoRoot 'FX\ui\panorama\images'
$publishedRoot = Join-Path $repoRoot 'FX\compiled\source_mvm_fx\panorama\images'
$sources = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Filter '*.vtex')
if ($sources.Count -eq 0) { throw "no .vtex sources under $sourceRoot" }

# The compiled name of a source: panorama/images/sourcemvm/ol_eye_png.vtex -> ol_eye_png.vtex_c,
# at the same path relative to panorama\images.
function RelativeTo([string]$root, [string]$path) {
    return $path.Substring($root.TrimEnd('\').Length + 1)
}

$canCompile = Test-Path -LiteralPath $ResourceCompiler
if ($canCompile) {
    $contentDir = Join-Path $GeneratedRoot 'content\source_mvm_fx'
    $gameDir = Join-Path $GeneratedRoot 'game\source_mvm_fx'
    New-Item -ItemType Directory -Force -Path $contentDir, $gameDir | Out-Null
    # Same junction and gameinfo layout as the render-material build, so this also works on a
    # machine where the particle converter has never run.
    foreach ($game in 'csgo','csgo_imported','csgo_core','core') {
        $link = Join-Path (Split-Path $gameDir -Parent) $game
        if (-not (Test-Path -LiteralPath $link)) {
            New-Item -ItemType Junction -Path $link -Target (Join-Path $Cs2Dir "game\$game") | Out-Null
        }
    }
    $gameInfo = Join-Path $gameDir 'gameinfo.gi'
    if (-not (Test-Path -LiteralPath $gameInfo)) {
        @'
"GameInfo"
{
    game "SOURCE:MVM UI Textures"
    FileSystem
    {
        SearchPaths
        {
            Game source_mvm_fx
            Game csgo
            Game csgo_imported
            Game csgo_core
            Game core
        }
    }
}
'@ | Set-Content -LiteralPath $gameInfo -Encoding ASCII
    }

    # The .vtex names its PNG relative to the content root, so both go in under panorama\images.
    $contentImages = Join-Path $contentDir 'panorama\images'
    foreach ($vtex in $sources) {
        $rel = RelativeTo $sourceRoot $vtex.DirectoryName
        $dest = Join-Path $contentImages $rel
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -LiteralPath $vtex.FullName -Destination $dest -Force
        Get-ChildItem -LiteralPath $vtex.DirectoryName -File -Filter '*.png' | Copy-Item -Destination $dest -Force
    }
    foreach ($dir in ($sources | ForEach-Object { RelativeTo $sourceRoot $_.DirectoryName } | Sort-Object -Unique)) {
        & $ResourceCompiler -i (Join-Path (Join-Path $contentImages $dir) '*.vtex') -r -game $gameDir -nop4 -fshallow2
        if ($LASTEXITCODE -ne 0) { throw "resourcecompiler.exe failed for UI textures in $dir (exit $LASTEXITCODE)." }
        $out = Join-Path $publishedRoot $dir
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        Get-ChildItem -LiteralPath (Join-Path (Join-Path $gameDir 'panorama\images') $dir) -File -Filter '*.vtex_c' |
            Copy-Item -Destination $out -Force
    }
    Write-Host "Compiled UI textures into $publishedRoot" -ForegroundColor Green
} else {
    Write-Host 'resourcecompiler.exe unavailable (CS2 Workshop Tools not installed); staging the compiled UI textures on disk.' -ForegroundColor Yellow
}

# Stage whatever the pack holds, and name anything a source expects that is not there: those
# controls draw blank in game until the pack is rebuilt or restored.
$missing = @()
foreach ($vtex in $sources) {
    $rel = RelativeTo $sourceRoot $vtex.DirectoryName
    $compiledName = $vtex.Name + '_c'
    $compiledPath = Join-Path (Join-Path $publishedRoot $rel) $compiledName
    if (-not (Test-Path -LiteralPath $compiledPath)) { $missing += (Join-Path $rel $compiledName); continue }
    $stageDir = Join-Path (Join-Path $StageRoot 'panorama\images') $rel
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    Copy-Item -LiteralPath $compiledPath -Destination $stageDir -Force
}
if ($missing.Count -gt 0) {
    Write-Warning ("compiled UI textures missing from FX\compiled (install CS2 Workshop Tools and rebuild, or restore the pack): " + ($missing -join ', '))
}
Write-Host ("Staged " + ($sources.Count - $missing.Count) + " UI textures to " + (Join-Path $StageRoot 'panorama\images')) -ForegroundColor Green
