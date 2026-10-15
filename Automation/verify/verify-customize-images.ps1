#requires -Version 5
<#
Drives the Customize Player modal and screenshots the GLOVE + AGENT pickers (and the open modal) to
verify real CS2 item thumbnails load. Foregrounds CS2 before each capture so an overlapping window
can't leak into the grab. Reuses launch-cs2-netcon + cs2-netcon + capture-game-window.
  Automation/output/customize_player/img_03_modal.png
  Automation/output/customize_player/img_glove_picker.png
  Automation/output/customize_player/img_agent_picker.png
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "test all hard",
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
function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds | Out-Null
}
function EditorEval([string]$Js) { return 'mirv_filmmaker editor eval ' + $Js }
function Foreground-Cs2 {
    Add-Type -Namespace W -Name F -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);' -ErrorAction SilentlyContinue
    $p = Get-Process -Name cs2 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { [void][W.F]::ShowWindow($p.MainWindowHandle, 9); [void][W.F]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 600 }
}
function Capture([string]$Name) {
    Foreground-Cs2
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out (Join-Path $outAbs $Name) | Write-Host
}

if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6
}

Write-Host "=== loading $Demo @ $Tick ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 9.0
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $Tick", 'demo_pause') -ReadSeconds 4.0
Start-Sleep -Seconds 1
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0
Start-Sleep -Milliseconds 800
Invoke-Netcon -Commands @((EditorEval '$.CamEditor.openCustomize()')) -ReadSeconds 1.5
Start-Sleep -Seconds 3
Capture 'img_03_modal.png'

Invoke-Netcon -Commands @((EditorEval '$.CamEditor.customizeOpenPicker(String.fromCharCode(103,108,111,118,101,115))')) -ReadSeconds 1.0
Start-Sleep -Milliseconds 800
Capture 'img_glove_picker.png'

Invoke-Netcon -Commands @((EditorEval '$.CamEditor.customizeOpenPicker(String.fromCharCode(97,103,101,110,116))')) -ReadSeconds 1.0
Start-Sleep -Milliseconds 800
Capture 'img_agent_picker.png'

Write-Host "Artifacts in $outAbs" -ForegroundColor Green
