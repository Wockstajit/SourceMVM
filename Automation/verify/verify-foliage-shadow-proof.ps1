param([ValidateSet('before','after')][string]$Phase='after',[string]$OutputDir='Automation/output/foliage-shadow-proof')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -OutputDir $OutputDir -LibraryOnly
if($Phase -eq 'before'){
    Send-Netcon @('getpos','mirv_filmmaker environment status','mirv_filmmaker environment save foliage_re_before_20260917_0250') .2 |
        Set-Content -LiteralPath (Join-Path $output 'before-state.txt')
    Capture-Frame 'de_mirage' 'before'
    return
}
$casterPath=Join-Path $env:APPDATA 'HLAE/source_mvm/environment/occlusion/de_mirage.mvmcasters'
$disabledPath=$casterPath+'.disabled-proof'
if(Test-Path -LiteralPath $disabledPath){throw 'A previous disabled-proof cache exists; restore it before testing.'}
try {
    [void](Send-Netcon @('mvm_debug start','mirv_filmmaker environment reload-geometry') .2)
    Start-Sleep -Seconds 3
    Move-Item -LiteralPath $casterPath -Destination $disabledPath
    [void](Send-Netcon @('mirv_filmmaker environment reload-geometry') .2)
    Start-Sleep -Seconds 2
    Capture-Frame 'de_mirage' 'without-foliage'
    Move-Item -LiteralPath $disabledPath -Destination $casterPath
    [void](Send-Netcon @('mirv_filmmaker environment reload-geometry') .2)
    Start-Sleep -Seconds 2
    Capture-Frame 'de_mirage' 'foliage'
    Send-Netcon @('getpos','mirv_filmmaker environment status','mirv_filmmaker health') .2 |
        Set-Content -LiteralPath (Join-Path $output 'foliage-state.txt')
    [void](Send-Netcon @('mirv_filmmaker environment climate sunAzimuth 328') .2)
    Start-Sleep -Seconds 2
    Capture-Frame 'de_mirage' 'opposite'
    [void](Send-Netcon @('mirv_filmmaker environment climate sunAzimuth 148') .2)
    Start-Sleep -Seconds 1
    Capture-Frame 'de_mirage' 'restored'
} finally {
    if(Test-Path -LiteralPath $disabledPath){Move-Item -LiteralPath $disabledPath -Destination $casterPath}
    [void](Send-Netcon @('mirv_streams record end','mvm_debug stop') .2)
}
