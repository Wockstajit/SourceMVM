#requires -Version 5
<#
Glove cosmetic end-to-end automation: spectate a player, turn on mvm_debug, switch their gloves to a
different model+skin, play so it renders, SEEK BACK a few seconds, and check whether the glove skin
persists or reverts. Captures screenshots (third-person + first-person) at each stage, stops mvm_debug,
and leaves a log + shots for inspection. Self-contained: launches CS2 if needed, auto-stops at the end.

Outputs:
  Automation/output/glove_skin/*.png   (00_before / 01_apply / 02_afterseek, tp+fp)
  newest %APPDATA%/HLAE/debuglogs/mvm_debug_*.log
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$ApplyTick = 4000,
    [int]$SeekBackTicks = 500,
    [int]$GloveDef = 5031, [int]$GlovePaint = 1439,  # Driver Gloves | Hand Sweaters
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$out = Join-Path $root 'Automation\output\glove_skin'
New-Item -ItemType Directory -Path $out -Force | Out-Null
Get-ChildItem $out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

function Test-Netcon([int]$p){ $t=[System.Net.Sockets.TcpClient]::new(); try{$c=$t.BeginConnect('127.0.0.1',$p,$null,$null);$ok=$c.AsyncWaitHandle.WaitOne(500);if($ok){$t.EndConnect($c)};return $ok}catch{return $false}finally{$t.Dispose()} }
function NC([string[]]$cmds,[double]$read=2.0){ $log=Join-Path $env:TEMP ('nc_'+[Guid]::NewGuid().ToString('N')+'.log'); & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null; $t=if(Test-Path $log){Get-Content $log -Raw}else{''}; Remove-Item $log -ErrorAction SilentlyContinue; return $t }
function LastMatch([string]$t,[string]$p){ $m=[regex]::Matches($t,$p); if($m.Count){$m[$m.Count-1].Groups[1].Value}else{$null} }
function Shot([string]$name){ & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out (Join-Path $out $name) | Out-Null; Write-Host "  shot: $name" }
# Play the demo briefly so renderables/streaming advance, then re-pause (cosmetics need rendered frames).
function PlayBeat([int]$sec){ NC @('demo_resume') 0.3 | Out-Null; Start-Sleep $sec; NC @('demo_pause') 0.5 | Out-Null }

if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    Write-Host "Launching CS2..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') -Port $Port -SettleSeconds 6 -OutDir (Join-Path $out 'launch')
}

Write-Host "mvm_debug on + load demo @ tick $ApplyTick" -ForegroundColor Cyan
NC @('mvm_debug start') 1.5 | Out-Null
NC @("playdemo `"$Demo`"") 9.0 | Out-Null; Start-Sleep 2
NC @("demo_gototick $ApplyTick") 4.0 | Out-Null; Start-Sleep 2; NC @('demo_pause') 1.0 | Out-Null

# Resolve the CURRENTLY spectated player's SteamID -- WITHOUT touching the camera (no spec_mode/spec_next).
# Park your own view on the player you want before running this; the automation only reads + applies.
$steam=$null
$d=NC @('mirv_filmmaker cosmetics visualdiag') 2.0; $steam=LastMatch $d 'spectateSteam=(\d+)'
if(-not $steam -or $steam -eq '0'){ throw "no spectated player resolved -- park your view on a player first" }
Write-Host "Spectating steam=$steam (camera left untouched)" -ForegroundColor Green

# BEFORE: cosmetics off, capture the player's real gloves from wherever YOUR camera is parked.
NC @('mirv_filmmaker cosmetics enabled 0') 1.0 | Out-Null
PlayBeat 2; Shot '00_before.png'

# SWITCH GLOVES.
Write-Host "Switch gloves -> def=$GloveDef paint=$GlovePaint" -ForegroundColor Cyan
NC @('mirv_filmmaker cosmetics enabled 1') 1.0 | Out-Null
NC @("mirv_filmmaker cosmetics player $steam gloves $GloveDef paint $GlovePaint wear 0.22 seed 0") 2.0 | Out-Null
PlayBeat 4   # let the glove material stream + render
Shot '01_apply.png'

# GO BACK A FEW TICKS, then check the glove skin persisted / re-applied.
Write-Host "Seek back $SeekBackTicks ticks, check persistence" -ForegroundColor Cyan
NC @("demo_gototick $([Math]::Max(1,$ApplyTick-$SeekBackTicks))") 4.0 | Out-Null; Start-Sleep 1
PlayBeat 4
Shot '02_afterseek.png'

$status = NC @('mirv_filmmaker cosmetics status') 2.0
NC @('mvm_debug stop') 1.5 | Out-Null

$log = Get-ChildItem $dbgDir -Filter '*.log' | Sort-Object LastWriteTime -Desc | Select-Object -First 1
$agentLog = Join-Path $root 'debug-af2ef9.log'
if (-not (Test-Path $agentLog)) {
    $agentLog = Join-Path $dbgDir 'debug-af2ef9.log'
}
Write-Host "`n================ DONE ================" -ForegroundColor Yellow
Write-Host "steam=$steam glove def=$GloveDef paint=$GlovePaint"
Write-Host "shots: $out"
Write-Host "log:   $($log.FullName)"
Write-Host "agent: $agentLog"
($status -split "`n") | Select-String 'glove|pawns|gloves=' | ForEach-Object { $_.Line.Trim() }
Write-Host "=====================================" -ForegroundColor Yellow
