# Offline native shadow comparison. Capture evidence, not an automatic visual pass.
param([string]$Map='de_mirage', [string]$OutputDir='Automation/output/native-sun-check', [switch]$CurrentView)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -OutputDir $OutputDir -LibraryOnly
if ($CurrentView) {
    $cameraReply = Send-Netcon @('getpos') .3
    $match = [regex]::Match($cameraReply, 'setpos\s+([-0-9.]+\s+[-0-9.]+\s+[-0-9.]+);setang\s+([-0-9.]+\s+[-0-9.]+\s+[-0-9.]+)')
    if (-not $match.Success) { throw 'Could not save the current camera; refusing to move it.' }
    # Do not round-trip getpos through setpos: player-origin/view offsets can differ.
    # CurrentView intentionally performs no camera-position or view-angle commands.
    $pose = @()
    $cameraReply | Set-Content -LiteralPath (Join-Path $output 'camera.txt')
    [void](Send-Netcon @('exec dev') .2)
    Start-Sleep -Seconds 4
} else {
[void](Send-Netcon @('mirv_filmmaker environment off','exec dev') .2)
[void](Send-Netcon @("map $Map") .1)
[void](Wait-Map $Map)
[void](Send-Netcon @('exec dev') .2)
Start-Sleep -Seconds 3
[void](Send-Netcon @('jointeam 3','mp_team_intro_time 0') .2)
Start-Sleep -Seconds 12
$pose=@('setpos_exact -1561.1 -2215.5 -164.3','setang_exact 10 -70 0')
if($Map -eq 'de_overpass') {$pose=@('setpos_exact -2199 740 620','setang_exact 10 160 0')}
if($Map -eq 'de_nuke') {$pose=@('setpos_exact -500 -4076 -280','setang_exact 10 68 0')}
}
[void](Send-Netcon (@('sv_cheats 1','noclip 1','god','r_drawviewmodel 0','fov_cs_debug 90',
    'mirv_filmmaker environment off','mirv_filmmaker world customshadow off',
    'mirv_filmmaker world light reset','mirv_filmmaker world bakedshadows on',
    'mirv_filmmaker world dynamicshadows on','mvm_debug start','version')+$pose) .15)
$steps=@(
    @{name='native';commands=@('mirv_filmmaker world light yaw 0')},
    @{name='native-yaw90';commands=@('mirv_filmmaker world light yaw 90')},
    @{name='native-yaw180';commands=@('mirv_filmmaker world light yaw 180')},
    @{name='baked-off';commands=@('mirv_filmmaker world light yaw 0','mirv_filmmaker world bakedshadows off')},
    @{name='baked-off-yaw90';commands=@('mirv_filmmaker world light yaw 90')},
    @{name='baked-off-yaw180';commands=@('mirv_filmmaker world light yaw 180')},
    @{name='baked-off-pitch30';commands=@('mirv_filmmaker world light yaw 0','mirv_filmmaker world light pitch 30')},
    @{name='both-off';commands=@('mirv_filmmaker world light pitch 0','mirv_filmmaker world dynamicshadows off')},
    @{name='restored';commands=@('mirv_filmmaker world bakedshadows on','mirv_filmmaker world dynamicshadows on','mirv_filmmaker world light reset')}
)
try {
    foreach($step in $steps){
        [void](Send-Netcon $step.commands .2)
        Start-Sleep -Milliseconds 900
        $capture=Capture-Frame $Map $step.name
        Send-Netcon @('mirv_filmmaker world light','mirv_filmmaker config state') .15 |
            Set-Content -LiteralPath (Join-Path $output "$Map-$($step.name)-state.txt")
        Write-Host "$Map $($step.name): $($capture.path)"
    }
} finally {
    [void](Send-Netcon @('mirv_streams record end','mirv_filmmaker world bakedshadows on',
        'mirv_filmmaker world dynamicshadows on','mirv_filmmaker world light reset','mvm_debug stop') .15)
}
