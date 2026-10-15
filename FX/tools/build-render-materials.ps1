[CmdletBinding()]
param(
    [string]$Cs2Dir = '',
    [string]$ResourceCompiler = '',
    [string]$GeneratedRoot,
    [string]$StageRoot
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\..\Automation\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($ResourceCompiler)) { $ResourceCompiler = Join-Path $Cs2Dir 'game\bin\win64\resourcecompiler.exe' }

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $GeneratedRoot) {
    $GeneratedRoot = Join-Path $repoRoot 'build\fx\fx-source1import\source2'
}
if (-not $StageRoot) {
    $StageRoot = Join-Path $repoRoot 'build\source2-release\fx\source_mvm_fx'
}

# The compiled materials are committed under FX\compiled (the runtime FX pack). Compiling needs
# resourcecompiler.exe and the core content sources, which only CS2 Workshop Tools install; without
# them the committed build is staged unchanged.
$publishedDir = Join-Path $repoRoot 'FX\compiled\source_mvm_fx\materials\filmmaker'
$sourceDir = Join-Path $repoRoot 'FX\render-materials\materials\filmmaker'
$defaultSource = Join-Path $Cs2Dir 'content\core\materials\default'
$canCompile = (Test-Path -LiteralPath $ResourceCompiler) -and (Test-Path -LiteralPath (Join-Path $defaultSource 'default.tga'))

if ($canCompile) {
    $contentDir = Join-Path $GeneratedRoot 'content\source_mvm_fx'
    $gameDir = Join-Path $GeneratedRoot 'game\source_mvm_fx'
    New-Item -ItemType Directory -Force -Path $contentDir, $gameDir | Out-Null

    # The shared generated root normally owns these junctions already. Create only missing ones so the
    # material build also works before the much larger particle conversion has ever run.
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
    game "SOURCE:MVM Render Materials"
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

    $contentMaterials = Join-Path $contentDir 'materials\filmmaker'
    New-Item -ItemType Directory -Force -Path $contentMaterials | Out-Null
    Get-ChildItem -LiteralPath $sourceDir -File -Filter '*.vmat' |
        Copy-Item -Destination $contentMaterials -Force

    # A fresh material-only build has not run the particle converter, which normally
    # stages these shader-default texture sources. Resolve them independently here.
    $defaultDestination = Join-Path $contentDir 'materials\default'
    New-Item -ItemType Directory -Force -Path $defaultDestination | Out-Null
    Get-ChildItem -LiteralPath $defaultSource -File -Filter '*.tga' |
        Copy-Item -Destination $defaultDestination -Force

    & $ResourceCompiler -i (Join-Path $contentMaterials '*.vmat') -r -game $gameDir -nop4 -fshallow2
    if ($LASTEXITCODE -ne 0) {
        throw "resourcecompiler.exe failed for render materials (exit $LASTEXITCODE)."
    }
    New-Item -ItemType Directory -Force -Path $publishedDir | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $gameDir 'materials\filmmaker') -File -Filter 'chromakey_*' |
        Copy-Item -Destination $publishedDir -Force
    Write-Host "Compiled render materials into $publishedDir (commit them)" -ForegroundColor Green
} else {
    Write-Host "resourcecompiler.exe or core content sources unavailable (CS2 Workshop Tools not installed); staging the committed render materials." -ForegroundColor Yellow
}

$stageDir = Join-Path $StageRoot 'materials\filmmaker'
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
$compiled = @(Get-ChildItem -LiteralPath $publishedDir -File -Filter 'chromakey_*' -ErrorAction SilentlyContinue)
foreach ($required in 'chromakey_unlit.vmat_c', 'chromakey_skip.vmat_c') {
    if (-not ($compiled | Where-Object Name -eq $required)) {
        throw "compiled $required is missing from $publishedDir (run 'git lfs pull', or install CS2 Workshop Tools and rebuild)"
    }
}
$compiled | Copy-Item -Destination $stageDir -Force
Write-Host "Staged uniform render materials to $stageDir" -ForegroundColor Green

# Prebuilt render materials (FX/render-materials/prebuilt) are staged as-is. Today that is the
# Players Only / Green Screen sky (materials/skybox/sky_green.*), which cannot be compiled from
# source because the 2026-08-13 CS2 update removed resourcecompiler.exe from the retail install;
# it is derived from native compiled assets by the standalone FX/tools/build-green-sky.py.
$prebuiltRoot = Join-Path $repoRoot 'FX\render-materials\prebuilt'
if (Test-Path -LiteralPath $prebuiltRoot) {
    Copy-Item -Path (Join-Path $prebuiltRoot '*') -Destination $StageRoot -Recurse -Force
    Write-Host "Staged prebuilt render materials from $prebuiltRoot" -ForegroundColor Green
}
