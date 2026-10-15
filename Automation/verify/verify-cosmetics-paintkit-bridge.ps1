#requires -Version 5
<#
Verifies the experimental cosmetics paintkit bridge.

The bridge wraps CS2's proven global cl_paintkit_override path. That cvar is
consulted only while a weapon composite is built, so this verifier captures the
same paused demo tick twice:
  1. bridge OFF / cl_paintkit_override 0
  2. bridge FORCE <paint> before a demo seek/recreate

It then diffs the screenshots. This does not prove true per-player skins; it
proves the deploy-time/global render path that the bridge intentionally exposes.

Artifacts: Automation/output/cosmetics_paintkit_bridge/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [string]$OutDir = "Automation\output\cosmetics_paintkit_bridge",
    [double]$MinMeanDiff = 0.10,
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
$logFile = Join-Path $outAbs 'paintkit_bridge.log'
"=== Cosmetics paintkit bridge verify $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
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

try {
    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Write-Host "Launching CS2 (netcon $Port)..." -ForegroundColor Cyan
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 6 `
            -OutDir (Join-Path $outAbs 'launch')
    }

    Write-Host "Loading demo..." -ForegroundColor Cyan
    Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 9.0 | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @("demo_gototick $GotoTick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
    Start-Sleep -Seconds 1

    $def = $null
    $diag = ''
    for ($try = 0; $try -lt 6; $try++) {
        $diag = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0
        $def = Get-LastMatch $diag 'defIndex=(\d+)'
        if ($def -and [int]$def -gt 0) { break }
        Invoke-Netcon -Commands @('spec_mode 4','spec_next') -ReadSeconds 1.5 | Out-Null
        Start-Sleep -Milliseconds 600
    }
    if (-not $def -or [int]$def -le 0) {
        throw "Could not resolve a spectated weapon defIndex. See $logFile"
    }

    $paintMap = @{ '7'='44'; '9'='279'; '1'='37'; '40'='222'; '61'='504'; '60'='282' }
    $paint = if ($paintMap.ContainsKey($def)) { $paintMap[$def] } else { '282' }
    Write-Host "Using paintKit=$paint for active defIndex=$def" -ForegroundColor Green
    Add-Content -LiteralPath $logFile -Value "CHOSEN def=$def paint=$paint"

    Invoke-Netcon -Commands @('sv_cheats 1', 'mirv_filmmaker cosmetics paintkitbridge 0', 'cl_paintkit_override 0') -ReadSeconds 1.5 | Out-Null
    Invoke-Netcon -Commands @("demo_gototick $GotoTick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
    Start-Sleep -Milliseconds 900
    $baseline = Capture '00_baseline.png'

    $bridgeOut = Invoke-Netcon -Commands @("mirv_filmmaker cosmetics paintkitbridge force $paint") -ReadSeconds 1.5
    $statusOut = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 1.5
    $bridgeValue = Get-LastMatch ($bridgeOut + "`n" + $statusOut) 'paintkitbridge=1 cvarFound=1 value=(\d+) forced='
    if ($bridgeValue -ne $paint) {
        throw "paintkitbridge did not set cl_paintkit_override to $paint (reported value='$bridgeValue')."
    }
    Invoke-Netcon -Commands @("demo_gototick $GotoTick", 'demo_pause') -ReadSeconds 4.0 | Out-Null
    Start-Sleep -Milliseconds 900
    $bridge = Capture '01_bridge.png'

    $diffJson = & python (Join-Path $automationRoot 'tools\image_diff.py') $baseline $bridge --min-mean $MinMeanDiff
    $exit = $LASTEXITCODE
    Add-Content -LiteralPath $logFile -Value "DIFF $diffJson"
    Write-Host "Diff: $diffJson" -ForegroundColor Cyan
    if ($exit -ne 0) {
        throw "Screenshot diff below threshold MinMeanDiff=$MinMeanDiff. Bridge did not visibly change the frame."
    }

    Write-Host "PASS: paintkit bridge changed the rendered frame." -ForegroundColor Green
} finally {
    if (Test-Netcon $Port) {
        Invoke-Netcon -Commands @('mirv_filmmaker cosmetics paintkitbridge 0', 'cl_paintkit_override 0') -ReadSeconds 1.0 | Out-Null
    }
}
