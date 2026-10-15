#requires -Version 5
<#
Live A/B of the 3 MapPlayerPreviewPanel scene presets to find which renders the agent inside the
in-game HUD. Opens the modal naturally, then cycles previewTry(0/1/2), screenshotting each.
Outputs: Automation/output/customize_player/scene0_match_mvp.png, scene1_vanity.png, scene2_loadout.png
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "poop1",
    [int]$Tick = 26000,
    [string]$OutDir = "Automation\output\customize_player",
    [switch]$NoLaunch
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try { $c = $t.BeginConnect('127.0.0.1', $p, $null, $null); $ok = $c.AsyncWaitHandle.WaitOne(500); if ($ok) { $t.EndConnect($c) }; return $ok }
    catch { return $false } finally { $t.Dispose() }
}
if (-not $NoLaunch -or -not (Test-Netcon $Port)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 -OutDir (Join-Path $outAbs 'scenes_launch')
}
function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('scenes_netcon_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
}
function EditorEval([string]$Js) { return 'mirv_filmmaker editor eval ' + $Js }
function Capture([string]$Name) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out (Join-Path $outAbs $Name) | Out-Null
    Join-Path $outAbs $Name
}

Write-Host "=== Preview scene A/B (demo=$Demo tick=$Tick) ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 8.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $Tick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
Start-Sleep -Seconds 1
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0 | Out-Null
Start-Sleep -Milliseconds 800
Invoke-Netcon -Commands @((EditorEval '$.Msg($.CamEditor.openCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3

$names = @('scene0_match_mvp.png','scene1_vanity.png','scene2_loadout.png')
for ($i = 0; $i -le 2; $i++) {
    $r = Invoke-Netcon -Commands @((EditorEval ('$.Msg($.CamEditor.previewTry(' + $i + ')+String.fromCharCode(10))'))) -ReadSeconds 2.0
    ($r -split "\r?\n" | Where-Object { $_ -match 'scene=' } | Select-Object -Last 1) | Write-Host -ForegroundColor Green
    Start-Sleep -Seconds 3
    Capture $names[$i] | Write-Host
}
Write-Host "Done. Compare $outAbs\scene0..2" -ForegroundColor Green




