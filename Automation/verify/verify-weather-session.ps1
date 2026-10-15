# Captures a substantially complete implementation in ONE running CS2 demo session.
# All interaction is NetCon. No mouse, keyboard, focus automation, quality changes or launches.
param(
    [int]$Port=29010,
    [string]$OutDir='build/verification/weather',
    [ValidateSet('wide','smoke','puddles','lightning','all')][string]$Phase='all'
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$net=Join-Path $repo 'Automation/netcon/cs2-netcon.ps1'
$demo=Join-Path $repo 'Automation/netcon/cs2-demo.ps1'
$capture=Join-Path $repo 'Automation/capture/capture-game-window.ps1'
$out=Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Send([string]$batch,[double]$seconds=.5){
    $text=((& $net -Port $Port -CommandBatch $batch -ReadSeconds $seconds 6>&1)|Out-String -Width 8192)
    [IO.File]::AppendAllText((Join-Path $out 'commands.txt'),"`n$batch`n$text")
    return $text
}
function Shot([string]$name){
    $null=Send 'mirv_filmmaker environment status;;mirv_filmmaker health' 1
    & $capture -Out (Join-Path $out "$name.png")
}
function Camera([double[]]$p,[double[]]$a){ & $demo -Port $Port -Position $p -Angles $a }
function Climate([string]$settings){
    $null=Send (($settings -split ';' | ForEach-Object {'mirv_filmmaker environment climate '+$_}) -join ';;')
}
$null=Send 'demo_pause;;mvm_debug start;;mirv_filmmaker environment on;;mirv_filmmaker environment weather rain'
if($Phase -in 'all','wide'){
    Camera @(-400,-2100,250) @(12,120,0)
    Shot 'rain-wide'
    Camera @(-400,-2100,250) @(-88,120,0)
    Shot 'rain-upward'
    Camera @(-732.1,-540.9,-200.6) @(-.1,-74.7,0)
    Shot 'rain-geometry'
    $null=Send 'demo_resume' 2
    Shot 'rain-motion-a'
    $null=Send 'demo_pause'
    Shot 'rain-motion-b'
}
if($Phase -in 'all','smoke'){
    & $demo -Port $Port -Tick 111700 -PlayTo 112033
    Camera @(-732.1,-540.9,-200.6) @(-.1,-74.7,0)
    Shot 'smoke-outside'
    $null=Send 'demo_resume' 1
    $null=Send 'demo_pause'
    Shot 'smoke-pause-resume'
    & $demo -Port $Port -Tick 111700
    Shot 'smoke-rewind'
    & $demo -Port $Port -Tick 113800
    Shot 'smoke-after-seek'
    & $demo -Port $Port -Tick 111700 -PlayTo 112033
    Shot 'smoke-replayed'
    $null=Send 'demo_resume' 24
    $null=Send 'demo_pause'
    Shot 'smoke-dissipated'
}
if($Phase -in 'all','puddles'){
    Camera @(-400,-2100,250) @(25,120,0)
    Climate 'enabled 1;wet 1;puddles 0.2;surfaceRadius 0'
    $null=Send 'mirv_filmmaker environment status' 20
    Shot 'puddles-low'
    Climate 'puddles 0.85'
    $null=Send 'mirv_filmmaker environment status' 20
    Shot 'puddles-high'
}
if($Phase -in 'all','lightning'){
    Climate 'enabled 1;storm 1;stormHold 1;fogDensity 0.1;wet 1;puddles 0.65'
    Camera @(-400,-2100,250) @(-15,120,0)
    Shot 'lightning-wide'
    Camera @(-800,-1600,250) @(-30,90,0)
    Shot 'lightning-second-angle'
    Climate 'stormHold 0;stormInterval 2'
    $null=Send 'demo_resume' 8
    $null=Send 'demo_pause'
    Shot 'layered-weather'
}
$null=Send 'mirv_filmmaker environment mapinfo;;mirv_filmmaker environment status;;mirv_filmmaker health'
Write-Output "Captures and command transcript: $out. Debug log left running for additional verification."
