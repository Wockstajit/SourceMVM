# Compile authored native particle lights; keep the converted particle packs intact.
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cs=& (Join-Path $repo 'Automation/lib/Resolve-Cs2Dir.ps1')
$compiler=Join-Path $cs 'game/bin/win64/resourcecompiler.exe'
if(!(Test-Path -LiteralPath $compiler)){throw 'CS2 Workshop Tools are required to compile the native particle lights.'}
$work=Join-Path $repo 'build/molly-light'
$game=Join-Path $work 'game/source_mvm_fx'
$content=Join-Path $work 'content/source_mvm_fx/particles/filmmaker'
New-Item -ItemType Directory -Force -Path $game,$content | Out-Null
foreach($part in 'csgo','csgo_imported','csgo_core','core'){
    $link=Join-Path $work "game/$part"
    if(!(Test-Path -LiteralPath $link)){New-Item -ItemType Junction -Path $link -Target (Join-Path $cs "game/$part") | Out-Null}
}
'"GameInfo" { game "SOURCE MVM FX" FileSystem { SearchPaths { Game source_mvm_fx Game csgo Game csgo_imported Game csgo_core Game core Mod source_mvm_fx Mod csgo } } }' | Set-Content -LiteralPath (Join-Path $game 'gameinfo.gi') -Encoding ASCII
foreach($asset in 'molly_light','molly_area','flash_light'){
    Copy-Item -LiteralPath (Join-Path $repo "FX/native/particles/filmmaker/$asset.vpcf") -Destination $content
    $log=Join-Path $work "compile-$asset.log"
    & $compiler -i (Join-Path $content "$asset.vpcf") -game $game -nop4 -fshallow *> $log
    $compileExit=$LASTEXITCODE
    $diagnostics=Get-Content -LiteralPath $log -Raw
    if($diagnostics -match 'LoadKV3ObjectInPlace error|Error parsing|Failed to compile'){throw "Invalid native light asset: $log"}
    $compiled=Join-Path $game "particles/filmmaker/$asset.vpcf_c"
    if($compileExit -ne 0 -or !(Test-Path -LiteralPath $compiled)){throw "Native light compilation failed: $log"}
    $publish=Join-Path $repo 'FX/compiled/source_mvm_fx/particles/filmmaker'
    New-Item -ItemType Directory -Force -Path $publish | Out-Null
    Copy-Item -LiteralPath $compiled -Destination $publish
    Write-Output "Compiled native particle light: $publish/$asset.vpcf_c"
}
