#requires -Version 5
<#
Live verifier for the Customize Player modal's 3D preview + current-loadout display.

Opens the modal the SAME way the in-game button does (openCustomize only -- deliberately NO
previewModel/previewRebuild "pokes"), so it proves the 3D agent preview renders standalone, not just
when external automation re-pokes it. Produces:
  Automation/output/customize_player/03_customize_modal_open.png
  Automation/output/customize_player/04_customize_modal_agent_picker.png
  Automation/output/customize_player/08_preview_after_agent_change.png
  Automation/output/customize_player/09_preview_after_primary_change.png
  Automation/output/customize_player/10_preview_after_secondary_change.png
  Automation/output/customize_player/11_preview_after_knife_change.png
  Automation/output/customize_player/12_preview_after_gloves_change.png
  Automation/output/customize_player/verify_preview_state.json
  Automation/output/customize_player/verify_preview_states.json
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
        -OutDir (Join-Path $outAbs 'verify_preview_launch')
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('verify_preview_netcon_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Add-Content -LiteralPath (Join-Path $outAbs 'verify_preview.log') -Value $text
    return $text
}

function EditorEval([string]$Js) { return 'mirv_filmmaker editor eval ' + $Js }

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path
    return $path
}

function Get-CustomizeState {
    $cmd = EditorEval '$.Msg($.CamEditor.customizeState()+String.fromCharCode(10))'
    $text = Invoke-Netcon -Commands @($cmd) -ReadSeconds 2.0
    $jsonLine = [regex]::Split($text, "\r?\n") | ForEach-Object {
        $idx = $_.IndexOf('{"visible":'); if ($idx -ge 0) { $_.Substring($idx).Trim() }
    } | Where-Object { $_ } | Select-Object -Last 1
    if (-not $jsonLine) { return $null }
    return $jsonLine | ConvertFrom-Json
}

function Set-CustomizePickFirst([string]$Slot) {
    $slotChars = (($Slot.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
    $cmd = EditorEval ('$.Msg($.CamEditor.customizePickFirst(String.fromCharCode(' + $slotChars + '))+String.fromCharCode(10))')
    Invoke-Netcon -Commands @($cmd) -ReadSeconds 1.5 | Out-Null
}

function Save-State([string]$Name) {
    $s = Get-CustomizeState
    if (-not $s) { throw "Customize state unavailable for $Name." }
    $script:states += [pscustomobject]@{
        step = $Name
        sel = $s.sel
        preview = $s.preview
        touched = $s.touched
        dirty = $s.dirty
    }
    return $s
}

Write-Host "=== Customize preview verifier (demo=$Demo tick=$Tick) ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 8.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $Tick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
Start-Sleep -Seconds 1
Invoke-Netcon -Commands @('mirv_filmmaker editor on') -ReadSeconds 2.0 | Out-Null
Start-Sleep -Milliseconds 800

# Open the modal exactly like the button does -- no preview pokes.
Invoke-Netcon -Commands @((EditorEval '$.Msg($.CamEditor.openCustomize()+String.fromCharCode(10))')) -ReadSeconds 1.5 | Out-Null
Start-Sleep -Seconds 3   # let render()'s per-frame maintainPreview() composite the scene

$states = @()
$state = Save-State 'open'
$state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outAbs 'verify_preview_state.json') -Encoding UTF8
Write-Host ("target={0} team={1} preview={2}" -f $state.name, $state.team, $state.preview) -ForegroundColor Green
Write-Host ("loadout: primary={0} secondary={1} knife={2} gloves={3}" -f `
    ($state.loadout.primary.defIndex), ($state.loadout.secondary.defIndex), `
    ($state.loadout.knife.defIndex), ($state.loadout.gloves.defIndex)) -ForegroundColor Green

Capture '03_customize_modal_open.png' | Write-Host

# Open the agent picker (screenshot the styled dropdown).
Invoke-Netcon -Commands @((EditorEval '$.CamEditor.customizeOpenPicker(String.fromCharCode(97,103,101,110,116))')) -ReadSeconds 1.0 | Out-Null
Start-Sleep -Milliseconds 600
Capture '04_customize_modal_agent_picker.png' | Write-Host

# Drive compact in-UI picks. These commands keep under netcon's short-line limit and prove the
# preview refreshes for each staged category without demo seek/playback pokes.
Write-Host "Picking first non-default agent" -ForegroundColor Cyan
Set-CustomizePickFirst 'agent'
Start-Sleep -Seconds 2
Save-State 'agent' | Out-Null
Capture '08_preview_after_agent_change.png' | Write-Host

$primaryDef = [int]($state.loadout.primary.defIndex)
if ($primaryDef -gt 0) {
    Write-Host "Picking first non-default primary skin" -ForegroundColor Cyan
    Set-CustomizePickFirst 'primary'
    Start-Sleep -Seconds 2
    Save-State 'primary' | Out-Null
    Capture '09_preview_after_primary_change.png' | Write-Host
} else {
    Write-Host "No known primary pick for defIndex $primaryDef; skipping primary shot." -ForegroundColor Yellow
}

$secondaryDef = [int]($state.loadout.secondary.defIndex)
if ($secondaryDef -gt 0) {
    Write-Host "Picking first non-default secondary skin" -ForegroundColor Cyan
    Set-CustomizePickFirst 'secondary'
    Start-Sleep -Seconds 2
    Save-State 'secondary' | Out-Null
    Capture '10_preview_after_secondary_change.png' | Write-Host
} else {
    Write-Host "No known secondary pick for defIndex $secondaryDef; skipping secondary shot." -ForegroundColor Yellow
}

Write-Host "Picking first non-default knife" -ForegroundColor Cyan
Set-CustomizePickFirst 'knife'
Start-Sleep -Seconds 2
Save-State 'knife' | Out-Null
Capture '11_preview_after_knife_change.png' | Write-Host

Write-Host "Picking first non-default gloves" -ForegroundColor Cyan
Set-CustomizePickFirst 'gloves'
Start-Sleep -Seconds 2
Save-State 'gloves' | Out-Null
Capture '12_preview_after_gloves_change.png' | Write-Host

$states | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outAbs 'verify_preview_states.json') -Encoding UTF8

Write-Host "Artifacts written to $outAbs" -ForegroundColor Green




