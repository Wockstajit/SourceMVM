param([switch]$Join,[string]$OutputDir='Automation/output/lighting-layers/game',
    [string]$RestorePreset='lighting_compare_before_20260917')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -OutputDir $OutputDir -LibraryOnly
if($Join){
    [void](Send-Netcon @('exec dev','map de_mirage') .15)
    [void](Wait-Map 'de_mirage')
    [void](Send-Netcon @('exec dev','jointeam 3','mp_team_intro_time 0') .15)
    Start-Sleep -Seconds 12
    [void](Send-Netcon @('bot_kick','bot_quota 0','noclip 1','mvm_test start','mirv_filmmaker config off',
        'setpos -1744.744507 -1867.229614 -267.402954','setang 7.520828 -75.566711 0') .15)
}
function Shot([string]$phase,[string[]]$commands){
    [void](Send-Netcon $commands .15)
    Start-Sleep -Milliseconds 1800
    Capture-Frame 'de_mirage' $phase
    Send-Netcon @('getpos','mirv_filmmaker config state','mirv_filmmaker environment status') .2 |
        Set-Content -LiteralPath (Join-Path $output "$phase-state.txt")
}
try {
    [void](Send-Netcon @('mvm_debug start','mirv_filmmaker config off',
        'mirv_filmmaker environment reset all','mirv_filmmaker world light reset',
        'mirv_filmmaker world exposure 1 quiet','mirv_filmmaker world shadows on') .15)
    Shot 'native' @('mirv_filmmaker world light enabled on')
    Shot 'no-cast-shadows' @('mirv_filmmaker world shadows off')
    Shot 'baked-shadows-only' @('mirv_filmmaker world bakedshadows on')
    Shot 'live-shadows-only' @('mirv_filmmaker world bakedshadows off','mirv_filmmaker world dynamicshadows on')
    Shot 'no-engine-lights' @('mirv_filmmaker world shadows on','mirv_filmmaker world light enabled off')
    Shot 'no-baked-lighting' @('mirv_filmmaker world light enabled on',
        'mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate bakedSuppressed 1')
    Shot 'unlit' @('mirv_filmmaker world light enabled off','mirv_filmmaker world shadows off')
    Shot 'environment-baked' @("mirv_filmmaker environment load $RestorePreset",
        'mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate relighting 1',
        'mirv_filmmaker environment climate bakedSuppressed 0')
    Shot 'environment-no-baked' @('mirv_filmmaker environment climate bakedSuppressed 1')
    [void](Send-Netcon @('mirv_filmmaker config on') .2)
    Shot 'ui-sun-owned' @("mirv_filmmaker config eval $.ConfigHud.gotoTab('effects')",
        "mirv_filmmaker config eval $.ConfigHud.card('light.card',1)",
        "mirv_filmmaker config eval $.ConfigHud.cardPage('light.card',1)",
        'mirv_filmmaker config eval $.ConfigHud.scrollTo(430)')
    Shot 'ui-sun-native' @('mirv_filmmaker environment climate relighting 0',
        'mirv_filmmaker environment climate bakedSuppressed 0','mirv_filmmaker world shadows on',
        'mirv_filmmaker world light enabled on')
    Shot 'ui-environment' @("mirv_filmmaker config eval $.ConfigHud.gotoTab('environment')",
        "mirv_filmmaker config eval $.ConfigHud.card('env.light',1)",
        "mirv_filmmaker config eval $.ConfigHud.cardPage('env.light',0)",
        'mirv_filmmaker config eval $.ConfigHud.scrollTo(156)')
    # Releasing takeover restores independent layer choices, not the old all-on master or
    # an obsolete tint snapshot. Changes made to the sun controls must survive the handoff.
    [void](Send-Netcon @('mirv_filmmaker config off',
        'mirv_filmmaker world light enabled off','mirv_filmmaker world bakedshadows on',
        'mirv_filmmaker world dynamicshadows off','mirv_filmmaker environment climate relighting 1') .2)
    [void](Send-Netcon @('mirv_filmmaker world light tint 181 207 233',
        'mirv_filmmaker world light amount 0.37','mirv_filmmaker world light mode filtered',
        'mirv_filmmaker environment climate relighting 0') .2)
    $handoff=Send-Netcon @('mirv_filmmaker world light','mirv_filmmaker config state') .3
    $handoff | Set-Content -LiteralPath (Join-Path $output 'handoff-state.txt')
    $flat=$handoff -replace '[\r\n]',''
    foreach($expected in @('Current: tint 181 207 233, amount 0.37, mode filtered',
        '"engineLights":false','"bakedShadows":true','"dynamicShadows":false')) {
        if(-not $flat.Contains($expected)){throw "Lighting takeover regression: missing $expected"}
    }
    Write-Output 'PASS: independent lighting layers and edited sun tint survive takeover release.'
} finally {
    [void](Send-Netcon @('mirv_streams record end','mirv_filmmaker config off',
        "mirv_filmmaker environment load $RestorePreset",'mvm_debug stop') .2)
}
