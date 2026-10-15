#requires -Version 5
<#
Compares the Customize Player 3D preview while demo playback is running vs paused.

Artifacts:
  Automation/output/customize_pause_check/01_playing_open.png
  Automation/output/customize_pause_check/02_playing_after_3s.png
  Automation/output/customize_pause_check/03_paused.png
  Automation/output/customize_pause_check/04_paused_after_3s.png
  Automation/output/customize_pause_check/05_resumed.png
  Automation/output/customize_pause_check/state_*.json
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = '',
    [int]$Tick = 26000,
    [string]$OutDir = "Automation\output\customize_pause_check",
    [switch]$NoLaunch
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($Demo)) { $Demo = Join-Path $Cs2Dir 'game\csgo\yashfartt.dem' }

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false }
    finally { $t.Dispose() }
}

if (-not $NoLaunch -or -not (Test-Netcon $Port)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 `
        -OutDir (Join-Path $outAbs 'launch')
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('netcon_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Add-Content -LiteralPath (Join-Path $outAbs 'verify_pause.log') -Value $text
    return $text
}

function EditorEval([string]$Js) { return 'mirv_filmmaker editor eval ' + $Js }

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path
    return $path
}

function Save-State([string]$Name) {
    $text = Invoke-Netcon -Commands @((EditorEval '$.Msg($.CamEditor.customizeState()+String.fromCharCode(10))')) -ReadSeconds 2.0
    $jsonLine = [regex]::Split($text, "\r?\n") | ForEach-Object {
        $idx = $_.IndexOf('{"visible":'); if ($idx -ge 0) { $_.Substring($idx).Trim() }
    } | Where-Object { $_ } | Select-Object -Last 1
    if ($jsonLine) { $jsonLine | Set-Content -LiteralPath (Join-Path $outAbs ("state_$Name.json")) -Encoding UTF8 }
}

Write-Host "=== Customize preview pause verifier (demo=$Demo tick=$Tick) ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 8.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $Tick", 'demo_resume') -ReadSeconds 4.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0 | Out-Null
Start-Sleep -Milliseconds 800

Invoke-Netcon -Commands @((EditorEval '$.Msg($.CamEditor.openCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3
Save-State 'playing_open'
Capture '01_playing_open.png' | Write-Host

Start-Sleep -Seconds 3
Save-State 'playing_after_3s'
Capture '02_playing_after_3s.png' | Write-Host

Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3
Save-State 'paused'
Capture '03_paused.png' | Write-Host

Start-Sleep -Seconds 3
Save-State 'paused_after_3s'
Capture '04_paused_after_3s.png' | Write-Host

Invoke-Netcon -Commands @('demo_resume') -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3
Save-State 'resumed'
Capture '05_resumed.png' | Write-Host

Write-Host "Artifacts written to $outAbs" -ForegroundColor Green
