param([int]$Port=29010, [string]$OutputDir='Automation/output/lighting-replacement')
$ErrorActionPreference='Stop'
# Run after joining offline practice and applying exec dev once. This script
# deliberately does not restart the match or reapply dev between captures.
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
$preset='lighting_transition_'+[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')
[void](Send-Netcon @("mirv_filmmaker environment save $preset", 'mirv_fov 110', 'fov_cs_debug 0', 'mvm_debug start') .1)
$recordName="$runName/transitions"
$events=[Collections.Generic.List[object]]::new()
$clock=[Diagnostics.Stopwatch]::StartNew()
try {
    [void](Send-Netcon @((Get-RecordCommand $recordName), 'mirv_streams record format bmp',
        'mirv_streams record screen enabled 1', 'mirv_streams record start') .04)
    foreach($command in @(
        'mirv_filmmaker environment climate sunAzimuth 140',
        'mirv_filmmaker environment climate sunAzimuth 150',
        'mirv_filmmaker environment climate sunAzimuth 145',
        'mirv_filmmaker environment climate sunAzimuth 155',
        'mirv_filmmaker environment climate sunAzimuth 147',
        'mirv_filmmaker environment off',
        'mirv_filmmaker environment on',
        'mirv_filmmaker environment off',
        'mirv_filmmaker environment on',
        'mirv_filmmaker environment reset lighting',
        "mirv_filmmaker environment load $preset",
        'mirv_filmmaker environment reset all',
        "mirv_filmmaker environment load $preset"
    )) {
        $events.Add(@{milliseconds=$clock.ElapsedMilliseconds; command=$command})
        $reply=Send-Netcon @($command) .12
        if($reply -match 'environment: error') { throw $reply }
    }
} finally {
    [void](Send-Netcon @('mirv_streams record end', "mirv_filmmaker environment load $preset") .12)
}
$take=Get-ChildItem -LiteralPath (Join-Path $recordRoot $recordName) -Directory | Sort-Object Name | Select-Object -Last 1
$frameCount=@(Get-ChildItem -LiteralPath $take.FullName -Filter '*.bmp').Count
Compress-Take $take.FullName
@{recordDirectory=$take.FullName; fov=110; preset=$preset; events=$events; frames=$frameCount} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'transitions.json')
Get-Content -LiteralPath (Join-Path $output 'transitions.json') -TotalCount 6
