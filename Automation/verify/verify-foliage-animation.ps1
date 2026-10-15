param([switch]$Join,[string]$OutputDir='Automation/output/foliage-animation',
    [string]$TestPreset='foliage_re_before_20260917_0250')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -OutputDir $OutputDir -LibraryOnly
if($Join){
    [void](Wait-Map 'de_mirage')
    [void](Send-Netcon @('exec dev','jointeam 3','mp_team_intro_time 0') .15)
    Start-Sleep -Seconds 12
    [void](Send-Netcon @('bot_kick','bot_quota 0','noclip 1') .15)
}
$restorePreset='foliage_motion_restore_'+(Get-Date -Format 'yyyyMMdd_HHmmss')
$poseReply=Send-Netcon @('getpos',"mirv_filmmaker environment save $restorePreset") .2
$pose=[regex]::Match($poseReply,'setpos ([-\d.]+) ([-\d.]+) ([-\d.]+);setang ([-\d.]+) ([-\d.]+) ([-\d.]+)')
if(-not $pose.Success){throw 'Could not save the current camera position; no test changes were made.'}
$culture=[Globalization.CultureInfo]::InvariantCulture
# getpos reports eye position; setpos sets the standing player origin (64 units lower).
$originZ=([double]::Parse($pose.Groups[3].Value,$culture)-64).ToString($culture)
$restorePosition="setpos $($pose.Groups[1].Value) $($pose.Groups[2].Value) $originZ"
$restoreAngles="setang $($pose.Groups[4].Value) $($pose.Groups[5].Value) $($pose.Groups[6].Value)"
try {
    [void](Send-Netcon @('mvm_debug start',
        "mirv_filmmaker environment load $TestPreset",
        'mirv_filmmaker environment reload-geometry',
        'setpos -646.548035 -2371.239014 19.524261','setang 25.600204 77.578247 0') .2)
    Start-Sleep -Seconds 4
    for($frameIndex=0;$frameIndex -lt 4;$frameIndex++){
        Capture-Frame 'de_mirage' "motion-$frameIndex"
        Start-Sleep -Milliseconds 800
    }
    # Keep the view aimed down at the shadow receivers: foliage must still animate offscreen.
    [void](Send-Netcon @('setang 50 77.578247 0') .2)
    for($frameIndex=0;$frameIndex -lt 3;$frameIndex++){
        Capture-Frame 'de_mirage' "offscreen-$frameIndex"
        Start-Sleep -Milliseconds 800
    }
    Send-Netcon @('getpos','mirv_filmmaker environment status','mirv_filmmaker health') .2 |
        Set-Content -LiteralPath (Join-Path $output 'state.txt')
} finally {
    [void](Send-Netcon @('mirv_streams record end','mvm_debug stop',
        "mirv_filmmaker environment load $restorePreset",$restorePosition,$restoreAngles) .2)
}
