param(
    [int]$Port=29010,
    [string]$OutputDir='Automation/output/lighting-replacement',
    [string]$Metadata='Automation/output/lighting-replacement/mirage-authored-lights.json',
    [string]$BaselinePreset='lighting_before_fixture_separation'
)
$ErrorActionPreference='Stop'
# Join OFFLINE Mirage and run exec dev once before this script. Do not restart
# the match between captures. The proof remains opt-in and is disabled on exit.
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
[void](Wait-Map de_mirage)
$metadataPath=(Resolve-Path -LiteralPath $Metadata).Path.Replace('\','/')
$restore='owned_light_restore_'+[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')
$events=[Collections.Generic.List[object]]::new()
$captures=[Collections.Generic.List[object]]::new()
$recordName="$runName/owned-light-motion"
$clock=[Diagnostics.Stopwatch]::StartNew()
function Proof-Command([string[]]$commands) {
    foreach($command in $commands){
        $events.Add(@{milliseconds=$clock.ElapsedMilliseconds;command=$command})
        $reply=Send-Netcon @($command) .15
        if($reply -match 'environment: error|Unknown command|proof: error'){throw $reply}
    }
}
[void](Send-Netcon @("mirv_filmmaker environment save $restore") .2)
try {
    Proof-Command @("mirv_filmmaker environment load $BaselinePreset",'mirv_filmmaker world exposure 1',
        'fov_cs_debug 0','mirv_fov 110','mvm_debug start','noclip 1',
        'setpos -1690 -690 -155','setang 0 155 0','mirv_filmmaker environment reset props',
        'mirv_filmmaker environment proof off','mirv_filmmaker environment off')
    $captures.Add((Capture-Frame de_mirage owned-stock))
    Proof-Command @('mirv_filmmaker environment on','mirv_filmmaker environment climate sky 1',
        'mirv_filmmaker environment climate relighting 1','mirv_filmmaker environment climate sunElevation -5',
        "mirv_filmmaker environment proof load `"$metadataPath`" `"maps/de_mirage/entities/default_ents.vents_c:hammer:2:60417:6655`"",
        'mirv_filmmaker environment proof on','mirv_filmmaker environment proof intensity 4',
        'mirv_filmmaker environment proof shadows 1')
    $captures.Add((Capture-Frame de_mirage owned-shadowed))
    Proof-Command @('mirv_filmmaker environment proof shadows 0')
    $captures.Add((Capture-Frame de_mirage owned-unshadowed))
    Proof-Command @('mirv_filmmaker environment proof fixture 0')
    $captures.Add((Capture-Frame de_mirage owned-dark))
    Proof-Command @('mirv_filmmaker environment proof fixture 1','mirv_filmmaker environment proof shadows 1',
        'mirv_filmmaker environment add box','mirv_filmmaker environment object 1 x -1775',
        'mirv_filmmaker environment object 1 y -600','mirv_filmmaker environment object 1 z -90',
        'mirv_filmmaker environment object 1 sx 22','mirv_filmmaker environment object 1 sy 22',
        'mirv_filmmaker environment object 1 sz 22')
    [void](Send-Netcon @((Get-RecordCommand $recordName),'mirv_streams record format bmp',
        'mirv_streams record screen enabled 1','mirv_streams record start') .04)
    foreach($y in @(-600,-580,-560,-540,-520,-540,-560,-580,-600)){
        Proof-Command @("mirv_filmmaker environment object 1 y $y")
    }
    Proof-Command @('mirv_filmmaker environment proof shadows 0','mirv_filmmaker environment proof shadows 1')
} finally {
    [void](Send-Netcon @('mirv_streams record end','mirv_filmmaker environment proof off',
        "mirv_filmmaker environment load $restore",'mirv_filmmaker world exposure 1') .15)
}
$take=Get-ChildItem -LiteralPath (Join-Path $recordRoot $recordName) -Directory | Sort-Object Name | Select-Object -Last 1
$frameCount=@(Get-ChildItem -LiteralPath $take.FullName -Filter '*.bmp').Count
Compress-Take $take.FullName
@{recordDirectory=$take.FullName;fov=110;exposure=1;restorePreset=$restore;events=$events;captures=$captures;
    frames=$frameCount;
    limitation='Only hash-verified opaque receivers are replaced. Native fallback is not a passed dark baseline.'} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'owned-lighting.json')
