#requires -Version 5
<#
Bisects which C_CSWeaponBase vtable index is OnDataChanged (the client-side skin-rebuild
trigger) by probing each candidate in-game and screenshotting the result.

Static analysis of the live client.dll (Docs/archive/cosmetics-recompose-research.md, 2026-06-28
section) narrowed OnDataChanged to a small set of DataUpdateType_t-taking virtuals. This driver:
  1. plays a demo, spectates a player holding a weapon,
  2. sets a distinctive skin override on that weapon (which "sticks" but does NOT render
     until something re-runs the create/data-changed path),
  3. captures a baseline (override set, NOT rebuilt -> original skin still shows),
  4. for each candidate index: `cosmetics vtprobe <idx>` (calls weapon->vtable[idx](this,0)
     after marking visuals stale) and screenshots.
The FIRST probe whose screenshot flips the weapon to the override skin identifies OnDataChanged.
If none flip, Path A (out-of-band OnDataChanged call) does not work and Path B is required.

Artifacts: Automation/output/ondatachanged_bisect/{00_baseline,probe_<idx>}.png + bisect.log
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [int[]]$Candidates = @(4,11,15,18,70,108,110,124,126),
    [string]$OutDir = "Automation\output\ondatachanged_bisect",
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
$meshPaintsPath = Join-Path $automationRoot 'config\weapon_mesh_test_paints.json'
$meshPaintsDoc = if (Test-Path -LiteralPath $meshPaintsPath) {
    Get-Content -LiteralPath $meshPaintsPath -Raw -Encoding UTF8 | ConvertFrom-Json
} else { $null }
function Get-WeaponSkinTestPaint([string]$defKey) {
    if ($meshPaintsDoc -and $meshPaintsDoc.weapons.PSObject.Properties.Name -contains $defKey) {
        return [string]$meshPaintsDoc.weapons.$defKey.skinTestPaint.id
    }
    # Fallback if config missing (weapon-valid paints only — never knife Fade / Glock Fade 38)
    $fallback = @{ '7'='282'; '9'='279'; '1'='37'; '40'='222'; '61'='504'; '60'='282'; '34'='33' }
    if ($fallback.ContainsKey($defKey)) { return $fallback[$defKey] }
    return '282'
}
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'bisect.log'
"=== OnDataChanged bisect $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    Write-Host "Launching CS2 (netcon $Port)..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 `
        -OutDir (Join-Path $outAbs 'launch')
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('nc_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log | Out-Null
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    Add-Content -LiteralPath $logFile -Value $text
    return $text
}

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}

function Get-LastMatch([string]$text, [string]$pattern) {
    $m = [regex]::Matches($text, $pattern)
    if ($m.Count -gt 0) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

# Distinctive, weapon-valid paint kits per def (from weapon_mesh_test_paints.json when present).

Write-Host "=== Loading demo ===" -ForegroundColor Cyan
Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 9.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @("demo_gototick $GotoTick") -ReadSeconds 4.0 | Out-Null
Start-Sleep -Seconds 2
Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 1.0 | Out-Null

# Ensure we are spectating a player holding a weapon; advance spectate target if needed.
$def = $null; $steam = $null; $diag = ''
for ($try = 0; $try -lt 6; $try++) {
    $diag = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0
    $def = Get-LastMatch $diag 'defIndex=(\d+)'
    $steam = Get-LastMatch $diag 'spectateSteam=(\d+)'
    if ($def -and [int]$def -gt 0 -and $steam -and $steam -ne '0') { break }
    Invoke-Netcon -Commands @('spec_mode 4','spec_next') -ReadSeconds 1.5 | Out-Null
    Start-Sleep -Milliseconds 600
}
if (-not $def -or [int]$def -le 0) { throw "Could not resolve a spectated weapon (defIndex). See $logFile" }

$offResolved = Get-LastMatch $diag 'offsetsResolved=(\d)'
$paint = Get-WeaponSkinTestPaint $def
Write-Host "Spectated weapon defIndex=$def steam=$steam offsetsResolved=$offResolved -> override paintKit=$paint" -ForegroundColor Green
Add-Content -LiteralPath $logFile -Value "CHOSEN def=$def steam=$steam paint=$paint offsetsResolved=$offResolved"

# Enable cosmetics + set a distinctive override on the held weapon (def 0 = restyle whatever is held).
Invoke-Netcon -Commands @('mirv_filmmaker cosmetics enabled 1') -ReadSeconds 1.5 | Out-Null
Invoke-Netcon -Commands @("mirv_filmmaker cosmetics player current weapon 0 paint=$paint wear=0.02 seed=0") -ReadSeconds 2.0 | Out-Null
Start-Sleep -Milliseconds 800

# Baseline: override is set + written every frame, but NOTHING has re-run the rebuild yet,
# so the weapon should still render its ORIGINAL skin here.
Capture '00_baseline.png' | Out-Null
Write-Host "Baseline captured." -ForegroundColor Cyan

foreach ($idx in $Candidates) {
    if (-not (Test-Netcon $Port)) {
        Write-Host "netcon dead (likely a crash from a previous probe) -- stopping." -ForegroundColor Red
        Add-Content -LiteralPath $logFile -Value "NETCON DEAD before idx=$idx (crash)"
        break
    }
    $out = Invoke-Netcon -Commands @("mirv_filmmaker cosmetics vtprobe $idx") -ReadSeconds 2.0
    $touched = Get-LastMatch $out 'touched (\d+) weapon'
    $faulted = Get-LastMatch $out 'faulted=(\d)'
    # Let the composite-material GameSystem process the enqueued clientside reload by running a few
    # frames, then re-pause for a stable screenshot.
    Invoke-Netcon -Commands @('demo_resume') -ReadSeconds 0.2 | Out-Null
    Start-Sleep -Milliseconds 900
    Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 0.2 | Out-Null
    Start-Sleep -Milliseconds 400
    Capture ("probe_{0}.png" -f $idx) | Out-Null
    Write-Host ("probe idx={0} touched={1} faulted={2}" -f $idx, $touched, $faulted) -ForegroundColor Yellow
    Add-Content -LiteralPath $logFile -Value ("PROBE idx=$idx touched=$touched faulted=$faulted")
}

Write-Host "Done. Compare 00_baseline.png against each probe_<idx>.png in $outAbs" -ForegroundColor Green
Write-Host "The first index whose screenshot flips the weapon skin is OnDataChanged." -ForegroundColor Green
