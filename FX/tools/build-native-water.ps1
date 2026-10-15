# Compile authored geometry against the installed CS2 water material. No Valve shader copies.
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-f0-9]{16}$')][string]$Key,
    [Parameter(Mandatory=$true)][string]$WorkRoot,
    [ValidateRange(0,1)][float]$Roughness = 0.08,
    [ValidateRange(0,2)][float]$Ripple = 0.35,
    [ValidateRange(0,10)][float]$Absorption = 0.02
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cs=& (Join-Path $repo 'Automation/lib/Resolve-Cs2Dir.ps1')
$compiler=Join-Path $cs 'game/bin/win64/resourcecompiler.exe'
if(!(Test-Path -LiteralPath $compiler)){throw 'CS2 Workshop Tools resourcecompiler is required for native water.'}
$content=Join-Path $WorkRoot 'content/source_mvm_water'
$game=Join-Path $WorkRoot 'game/source_mvm_water'
New-Item -ItemType Directory -Force -Path $game | Out-Null
foreach($part in 'csgo','csgo_imported','csgo_core','core'){
    $link=Join-Path $WorkRoot "game/$part"
    if(!(Test-Path -LiteralPath $link)){New-Item -ItemType Junction -Path $link -Target (Join-Path $cs "game/$part") | Out-Null}
}
'"GameInfo" { game "SOURCE MVM Water" FileSystem { SearchPaths { Game source_mvm_water Game csgo Game csgo_imported Game csgo_core Game core Mod source_mvm_water Mod csgo } } }' | Set-Content -LiteralPath (Join-Path $game 'gameinfo.gi') -Encoding ASCII
$folder='models/filmmaker/native_water'
$materialFolder='materials/filmmaker/native_water'
New-Item -ItemType Directory -Force -Path (Join-Path $content $materialFolder) | Out-Null
# Author neutral source textures: no debris/foam, flat normals, black fallback cube.
foreach($kind in 'black','normal') {
    $bytes=New-Object byte[] (18+16*16*3)
    $bytes[2]=2;$bytes[12]=16;$bytes[14]=16;$bytes[16]=24;$bytes[17]=32
    if($kind -eq 'normal'){for($i=18;$i -lt $bytes.Length;$i+=3){$bytes[$i]=255;$bytes[$i+1]=128;$bytes[$i+2]=128}}
    [IO.File]::WriteAllBytes((Join-Path $content "$materialFolder/$kind.tga"),$bytes)
}
$cubeHeader=[Text.Encoding]::ASCII.GetBytes("PF`n32 16`n-1.0`n")
$cube=New-Object byte[] ($cubeHeader.Length+32*16*3*4)
[Array]::Copy($cubeHeader,$cube,$cubeHeader.Length)
[IO.File]::WriteAllBytes((Join-Path $content "$materialFolder/black.pfm"),$cube)
$culture=[Globalization.CultureInfo]::InvariantCulture
$rough=$Roughness.ToString($culture);$rain=$Ripple.ToString($culture);$absorb=$Absorption.ToString($culture)
# Fields verified against Ancient's installed csgo_water_fancy material with VRF. Disable
# deep-water effects and inherited jungle cubemap tint on a film only .75 units thick.
$material=@"
"Layer0"
{
    "shader" "csgo_water_fancy.vfx"
    "F_REFLECTION_TYPE" "2"
    "F_REFRACTION" "1"
    "F_INTERACTION_EFFECTS" "1"
    "F_CAUSTICS" "0"
    "g_bFogEnabled" "1"
    "g_flReflectance" "0.0204"
    "g_flWaterRoughnessMin" "$rough"
    "g_flWaterRoughnessMax" "$rough"
    "g_flRainStrength" "$rain"
    "g_flWaterEffectsRippleStrength" "$rain"
    "g_flWaterFogStrength" "$absorb"
    "g_flWaterDecayStrength" "0"
    "g_flWaterPlaneOffset" "0"
    "g_flWavesHeightOffset" "0"
    "g_flLowFreqWeight" "0"
    "g_flMedFreqWeight" "0"
    "g_flHighFreqWeight" "0"
    "g_flWavesNormalStrength" "0"
    "g_flWaterEffectFoamStrength" "0"
    "g_flWaterEffectSiltStrength" "0"
    "g_flLowEndCubeMapIntensity" "0"
    "g_flEnvironmentMapBrightness" "1"
    "g_flSSRBrightness" "1"
    "g_nSSRMaxForwardSteps" "16"
    "g_flSSRSampleJitter" "0"
    "g_flSSRMaxThickness" "4"
    "g_flSSRStepSize" "0.2"
    "g_vColorTint" "[1 1 1 0]"
    "TextureDebris" "$materialFolder/black.tga"
    "TextureDebrisHeight" "$materialFolder/black.tga"
    "TextureDebrisNormal" "$materialFolder/normal.tga"
    "TextureFoamNormal" "$materialFolder/normal.tga"
    "TextureFoam" "$materialFolder/black.tga"
    "TextureLowEndCubeMap" "$materialFolder/black.pfm"
    "TextureWavesNormal" "$materialFolder/normal.tga"
    "TextureWavesHeight" "$materialFolder/black.tga"
    "SystemAttributes" { "PhysicsSurfaceProperties" "water" }
}
"@
$material | Set-Content -LiteralPath (Join-Path $content "$materialFolder/$Key.vmat") -Encoding ASCII
$header='<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:modeldoc41:version{12fc9d44-453a-4ae4-b4d9-7e2ac0bbd4e0} -->'
$surface=@"
$header
{rootNode={_class="RootNode" children=[
{_class="MaterialGroupList" children=[{_class="DefaultMaterialGroup" remaps=[{from="water.vmat" to="$materialFolder/$Key.vmat"}]}]},
{_class="RenderMeshList" children=[{_class="RenderMeshFile" filename="$folder/$Key.obj" import_scale=1.0 import_filter={exclude_by_default=false exception_list=[]}}]}
] model_archetype="" primary_associated_entity="" anim_graph_name="" document_sub_type="ModelDocSubType_None"}}
"@
$volume=@"
$header
{rootNode={_class="RootNode" children=[{_class="PhysicsShapeList" children=[{_class="PhysicsHullFile" filename="$folder/${Key}_volume.obj" parent_bone="" surface_prop="default" collision_tags="" name="water"}]}]}}
"@
$surface | Set-Content -LiteralPath (Join-Path $content "$folder/$Key.vmdl") -Encoding ASCII
$volume | Set-Content -LiteralPath (Join-Path $content "$folder/${Key}_volume.vmdl") -Encoding ASCII
$log=Join-Path $WorkRoot "$Key.log"
& $compiler -i (Join-Path $content "$materialFolder/$Key.vmat") -game $game -nop4 -fshallow *> $log
if($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath (Join-Path $game "$materialFolder/$Key.vmat_c"))){throw "Native water material compilation failed: $log"}
& $compiler -i (Join-Path $content "$folder/$Key.vmdl") -game $game -nop4 -fshallow *> $log
if($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath (Join-Path $game "$folder/$Key.vmdl_c"))){throw "Native surface compilation failed: $log"}
& $compiler -i (Join-Path $content "$folder/${Key}_volume.vmdl") -game $game -nop4 -fshallow *>> $log
if($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath (Join-Path $game "$folder/${Key}_volume.vmdl_c"))){throw "Native volume compilation failed: $log"}
$stage=Join-Path $repo "build/source2-release/fx/source_mvm_fx/$folder"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$materialStage=Join-Path $repo "build/source2-release/fx/source_mvm_fx/$materialFolder"
New-Item -ItemType Directory -Force -Path $materialStage | Out-Null
Get-ChildItem -LiteralPath (Join-Path $game $materialFolder) -File | Where-Object { $_.Extension -in @(".vmat_c", ".vtex_c") } | Copy-Item -Destination $materialStage
foreach($suffix in '.vmdl_c','_volume.vmdl_c'){
    Copy-Item -LiteralPath (Join-Path $game "$folder/$Key$suffix") -Destination (Join-Path $stage "$Key$suffix")
}
