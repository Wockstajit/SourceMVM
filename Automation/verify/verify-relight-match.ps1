[CmdletBinding()]
param(
    [int]$Port=29010,
    [string]$OutputDir='Automation/output/relight-native-match',
    [ValidateRange(2,30)][int]$SettleSeconds=5
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
[void](Wait-Map de_mirage)
$status=Send-Netcon @('mirv_relight status') .2
if($status -notmatch "lights=72 map='de_mirage'"){throw 'Load the Mirage authored-light export first.'}
$events=[Collections.Generic.List[object]]::new()
function Command([string[]]$Commands) {
    $reply=Send-Netcon $Commands .2
    $events.Add(@{commands=$Commands;reply=$reply})
    if($reply -match 'Unknown command|mirv_relight:|environment: error'){throw $reply}
    return $reply
}
# T-spawn lane: both sun-facing and shaded stone, recesses, rails and lamps.
# Set the pose once, then only switch rendering. setpos_exact specifies player feet;
# validate getpos (eye position) after server/client interpolation has settled.
[void](Command @('sv_cheats 1','sv_gravity 0','r_drawviewmodel 0','cl_drawhud 0',
    'setpos_exact 1216 -211 -100.53125','setang_exact 0 -90 0','mirv_fov 110',
    'mirv_filmmaker environment proof off','mirv_filmmaker environment off',
    'mirv_relight view off','mirv_relight reference 0','mvm_debug start'))
Start-Sleep -Seconds $SettleSeconds
$pose=Command @('getpos')
if($pose -notmatch 'setpos 1216\.\d+ -211\.\d+ ' -or $pose -notmatch 'setang 0\.\d+ -90\.\d+'){
    throw "The fixed camera did not take effect. Join a team and finish the round intro first. $pose"
}
$posePattern='setpos [-\d.]+ [-\d.]+ [-\d.]+;setang [-\d.]+ [-\d.]+ [-\d.]+'
$expectedPose=[regex]::Match($pose,$posePattern).Value
if(!$expectedPose){throw 'Could not read the settled camera pose.'}
try {
    foreach($phase in @('native','native-repeat','reference','relight','relight-repeat')) {
        if($phase -like 'native*'){[void](Command @('mirv_relight off'))}
        elseif($phase -eq 'reference'){[void](Command @('mirv_relight reference 1','mirv_relight on'))}
        else{[void](Command @('mirv_relight reference 0','mirv_relight on'))}
        Start-Sleep -Seconds $SettleSeconds
        Capture-Frame de_mirage $phase | Out-Null
        $capturedState=Command @('getpos','mirv_relight status')
        if([regex]::Match($capturedState,$posePattern).Value -ne $expectedPose){
            throw "Camera changed during $phase; reject this comparison. $capturedState"
        }
    }
} finally {
    # Never leave the native-input diagnostic enabled and mistake it for relighting.
    [void](Send-Netcon @('mirv_relight reference 0','mirv_relight off','mvm_debug stop') .2)
    $events | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'comparison-state.json')
    $log=Get-ChildItem -LiteralPath (Join-Path $env:APPDATA 'HLAE/debuglogs') -Filter 'mvm_debug_*.log' |
        Sort-Object LastWriteTimeUtc | Select-Object -Last 1
    if($log){Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $output 'comparison-debug.log')}
}
