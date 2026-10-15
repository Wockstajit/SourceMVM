#requires -Version 5
<#
Live verifier for Customize Player wear selection.

Produces the acceptance screenshots:
  Automation/output/customize_player/13_weapon_wear_dropdown.png
  Automation/output/customize_player/14_weapon_custom_float.png
  Automation/output/customize_player/15_demo_view_after_wear_change.png

It drives the existing CS2 netcon + game-window capture helpers and records console output to
verify that Apply (customizeApply(), the same commit path the action-bar button calls) sends the
selected wear value -- the modal now stages Finish/Wear picks and only writes to the demo on Apply.
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
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
        -OutDir (Join-Path $outAbs 'verify_wear_launch')
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('verify_wear_netcon_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Add-Content -LiteralPath (Join-Path $outAbs 'verify_wear.log') -Value $text
    return $text
}

function EditorEvalCommand([string]$Js) {
    return 'mirv_filmmaker editor eval ' + $Js
}

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path
    return $path
}

function Get-CustomizeState {
    $cmd = EditorEvalCommand '$.Msg($.CamEditor.customizeState()+String.fromCharCode(10))'
    $text = Invoke-Netcon -Commands @($cmd) -ReadSeconds 2.0
    $jsonLines = [regex]::Split($text, "\r?\n") | ForEach-Object {
        $idx = $_.IndexOf('{"visible":')
        if ($idx -ge 0) { $_.Substring($idx).Trim() }
    } | Where-Object { $_ }
    $jsonLine = $jsonLines | Select-Object -Last 1
    if (-not $jsonLine) { return $null }
    return $jsonLine | ConvertFrom-Json
}

Write-Host "=== Customize wear verifier ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 8.0 | Write-Host
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0 | Write-Host
Start-Sleep -Milliseconds 700

$state = Get-CustomizeState
if (-not $state) { throw "Customize state was unavailable after opening editor." }

$slot = if ($state.activeWeaponSlot -eq 'primary' -or $state.activeWeaponSlot -eq 'secondary') { [string]$state.activeWeaponSlot } else { 'secondary' }
$paint = switch ([int]$state.activeWeaponDef) {
    7 { '282' }
    9 { '279' }
    40 { '222' }
    61 { '504' }
    default { if ($slot -eq 'primary') { '282' } else { '504' } }
}

Write-Host "Using slot=$slot paintKit=$paint activeDef=$($state.activeWeaponDef) key=$($state.key)" -ForegroundColor Cyan
$slotChars = (($slot.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
$paintChars = (($paint.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')

Invoke-Netcon -Commands @((EditorEvalCommand '$.Msg($.CamEditor.openCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.0 | Write-Host
Invoke-Netcon -Commands @((EditorEvalCommand ('$.Msg($.CamEditor.customizePick(String.fromCharCode(' + $slotChars + '),String.fromCharCode(' + $paintChars + '))+String.fromCharCode(10))'))) -ReadSeconds 2.0 | Write-Host
Start-Sleep -Milliseconds 500

$pickedState = Get-CustomizeState
if (-not $pickedState) { throw "Customize state was unavailable after selecting paint kit $paint." }
$pickedValue = [string]$pickedState.sel.$slot
if ($pickedValue -notmatch ('(^|:)' + [regex]::Escape($paint) + '$')) {
    throw "Selected $slot paint kit did not persist. Expected $paint, got '$pickedValue'."
}

Invoke-Netcon -Commands @((EditorEvalCommand ('$.Msg($.CamEditor.customizeOpenWear(String.fromCharCode(' + $slotChars + '))+String.fromCharCode(10))'))) -ReadSeconds 1.0 | Write-Host
Capture '13_weapon_wear_dropdown.png' | Write-Host

Invoke-Netcon -Commands @((EditorEvalCommand ('$.Msg($.CamEditor.customizeWear(String.fromCharCode(' + $slotChars + '),String.fromCharCode(99,117,115,116,111,109),0.42)+String.fromCharCode(10))'))) -ReadSeconds 1.5 | Write-Host
Start-Sleep -Milliseconds 300

# The redesigned modal stages Finish/Wear/Agent/Gloves picks until committed (matches the reference
# mock's Apply/Cancel action bar) -- customizeWear() above only updates local state + the preview.
# customizeApply() is the same commit path the "APPLY TO <target>" button calls; only after this does
# the backend "mirv_filmmaker cosmetics player ..." command (and its console echo) actually fire.
$applyText = Invoke-Netcon -Commands @((EditorEvalCommand '$.Msg($.CamEditor.customizeApply()+String.fromCharCode(10))')) -ReadSeconds 2.5
$applyText | Write-Host
if ($applyText -notmatch ('paint=' + [regex]::Escape($paint)) -or $applyText -notmatch 'wear=0\.4200') {
    throw "Apply did not send paint kit $paint with wear 0.4200 to the backend."
}
Start-Sleep -Milliseconds 500
Capture '14_weapon_custom_float.png' | Write-Host

Invoke-Netcon -Commands @((EditorEvalCommand '$.Msg($.CamEditor.closeCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.0 | Write-Host
Start-Sleep -Milliseconds 500
Capture '15_demo_view_after_wear_change.png' | Write-Host

$finalState = Get-CustomizeState
$finalState | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outAbs 'verify_wear_state.json') -Encoding UTF8
Write-Host "Artifacts written to $outAbs" -ForegroundColor Green




