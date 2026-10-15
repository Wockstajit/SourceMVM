param([int]$Port=29010, [string]$OutDir='build/verification/environment-resets')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$net=Join-Path $repo 'Automation/netcon/cs2-netcon.ps1'
$out=Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Send([string]$batch) { ((& $net -Port $Port -CommandBatch $batch -ReadSeconds .2 6>&1) | Out-String) }
function State([string]$name) {
    $reply=Send 'mirv_filmmaker environment status'
    [IO.File]::WriteAllText((Join-Path $out "$name.txt"),$reply)
    $start=$reply.IndexOf('{"version"'); $end=$reply.LastIndexOf('}')
    if($start -lt 0 -or $end -lt $start) { throw "Missing environment state: $reply" }
    return ($reply.Substring($start,$end-$start+1) -replace '[\r\n]','' | ConvertFrom-Json)
}
function Reset([string]$name) { $null=Send ('mirv_filmmaker config eval $.ConfigHud.reset('''+$name+''')') }
function AllOff($s) {
    foreach($key in @('sky','clouds','fogDensity','relighting','worldGi','snow','wet','puddles','storm','bloom','vignette','grain','chromatic')) {
        if($s.climate.$key -ne 0) { throw "$key turned on after reset" }
    }
    if($s.weather.type -ne 0 -or $s.climate.enabled -ne 0) { throw 'Weather or renderer turned on after reset' }
}
$null=Send 'mirv_filmmaker config on'
$null=Send 'mirv_filmmaker config eval $.ConfigHud.gotoTab(''environment'')'
Reset 'environment'
AllOff (State 'full-reset')
foreach($card in @('looks','sky','light','fog','wx')) {
    Reset ('env.'+$card)
    AllOff (State ("off-reset-$card"))
    Write-Output "$card reset from off: PASS"
}
# Fog reset must stop the unified fog without activating the latent sky or lighting preferences.
$null=Send 'mirv_filmmaker environment climate enabled 1;;mirv_filmmaker environment climate fogDensity 0.7'
Reset 'env.fog'
AllOff (State 'fog-last-pass-reset')
# An unrelated coating remains active when lighting is reset; lighting and sky remain off.
$null=Send 'mirv_filmmaker environment climate enabled 1;;mirv_filmmaker environment climate snow 0.7;;mirv_filmmaker environment climate relighting 1;;mirv_filmmaker environment climate worldGi 1'
Reset 'env.light'
$s=State 'lighting-isolated-reset'
if($s.climate.snow -ne .7 -or $s.climate.enabled -ne 1 -or $s.climate.relighting -ne 0 -or $s.climate.worldGi -ne 0 -or $s.climate.sky -ne 0) { throw 'Lighting reset changed an unrelated pass or failed to turn off' }
Reset 'environment'
$s=State 'final-all-off'; AllOff $s
if($s.enabled) { throw 'Full reset left scene renderer enabled' }
Write-Output 'All environment reset checks passed; final scene is off.'
