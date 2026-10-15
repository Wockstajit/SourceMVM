#requires -Version 5
<#
Verifies the experimental Andromeda-style direct composite refresh path.

The test keeps the normal demo cosmetics write path intact, then explicitly runs:
  mirv_filmmaker cosmetics composite once [ownerOffsetHex]

That command resolves the researched client.dll functions and calls:
  UpdateCompositeMaterial(weapon + ownerOffset)
  UpdateCompositeMaterialSet(weapon)
  UpdateSkin(weapon)

Artifacts: Automation/output/cosmetics_composite_direct/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [string]$OwnerOffset = "0x608",
    [string]$OutDir = "Automation\output\cosmetics_composite_direct",
    # PASS requires the weapon-region change to clear BOTH this absolute floor AND a multiple of the
    # same-tick TAA noise floor. The diff is restricted to the viewmodel crop (the demo stays paused,
    # so the only legitimate change is the weapon material).
    [double]$MinMeanDiff = 1.0,
    [double]$NoiseMultiple = 3.0,
    [string]$WeaponCrop = "760,560,1500,1180",
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
$logFile = Join-Path $outAbs 'composite_direct.log'
"=== Cosmetics direct composite verify $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

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
    Invoke-Netcon -Commands @("demo_gototick $GotoTick") -ReadSeconds 4.0 | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 1.0 | Out-Null

    $def = $null; $steam = $null; $diag = ''
    for ($tryIndex = 0; $tryIndex -lt 6; $tryIndex++) {
        $diag = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0
        $def = Get-LastMatch $diag 'defIndex=(\d+)'
        $steam = Get-LastMatch $diag 'spectateSteam=(\d+)'
        if ($def -and [int]$def -gt 0 -and $steam -and $steam -ne '0') { break }
        Invoke-Netcon -Commands @('spec_mode 4','spec_next') -ReadSeconds 1.5 | Out-Null
        Start-Sleep -Milliseconds 600
    }
    if (-not $def -or [int]$def -le 0 -or -not $steam -or $steam -eq '0') {
        throw "Could not resolve a spectated player weapon. See $logFile"
    }

    $paintMap = @{ '7'='44'; '9'='279'; '1'='37'; '40'='222'; '61'='504'; '60'='282' }
    $paint = if ($paintMap.ContainsKey($def)) { $paintMap[$def] } else { '282' }
    Write-Host "Spectated weapon defIndex=$def steam=$steam -> override paintKit=$paint" -ForegroundColor Green
    Add-Content -LiteralPath $logFile -Value "CHOSEN def=$def steam=$steam paint=$paint ownerOffset=$OwnerOffset"

    # Real-skin baseline: cosmetics OFF so the authoritative demo skin renders. Demo STAYS PAUSED the
    # whole test -- the only legitimate frame change is the weapon material, so two paused real-skin
    # frames measure the TAA/temporal noise floor we must beat.
    Invoke-Netcon -Commands @(
        'mirv_filmmaker cosmetics paintkitbridge 0',
        'cl_paintkit_override 0',
        'mirv_filmmaker cosmetics recompose 0',
        'mirv_filmmaker cosmetics rebuildflags all 0',
        'mirv_filmmaker cosmetics clear',
        'mirv_filmmaker cosmetics enabled 0'
    ) -ReadSeconds 2.0 | Out-Null
    Start-Sleep -Milliseconds 600
    $realA = Capture '00_real_a.png'
    Start-Sleep -Milliseconds 500
    $realB = Capture '01_real_b.png'

    # Apply the per-player override on the ACTUAL held weapon's defIndex (NOT a hardcoded slot), with
    # fallback identity ON (m_iItemIDHigh = -1) so the composite build consults our fallback fields.
    Invoke-Netcon -Commands @(
        'mirv_filmmaker cosmetics enabled 1',
        'mirv_filmmaker cosmetics fallback 1',
        "mirv_filmmaker cosmetics player current weapon $def paint=$paint wear=0.02 seed=0"
    ) -ReadSeconds 2.0 | Out-Null
    Start-Sleep -Milliseconds 500

    $compositeOut = Invoke-Netcon -Commands @("mirv_filmmaker cosmetics composite once $OwnerOffset") -ReadSeconds 2.5
    $touched = Get-LastMatch $compositeOut 'composite once -> touched (\d+)'
    $resolved = Get-LastMatch $compositeOut 'resolved=(\d+)'
    $calls = Get-LastMatch $compositeOut 'calls=(\d+)'
    $faulted = Get-LastMatch $compositeOut 'faulted=(\d+)'
    Add-Content -LiteralPath $logFile -Value "COMPOSITE touched=$touched resolved=$resolved calls=$calls faulted=$faulted"

    if (-not (Test-Netcon $Port)) {
        throw "CS2/netcon died after direct composite call; likely crash. See $logFile"
    }
    if (-not $touched -or [int]$touched -le 0) {
        throw "Direct composite touched no matched weapons. See $logFile"
    }
    if ($resolved -ne '1' -or -not $calls -or [int]$calls -le 0) {
        throw "Direct composite function patterns did not resolve/call (resolved=$resolved calls=$calls). See $logFile"
    }
    if ($faulted -eq '1') {
        throw "Direct composite call faulted (caught); owner offset/signature likely stale. See $logFile"
    }

    Start-Sleep -Milliseconds 900
    $after = Capture '02_override.png'

    # Compare ONLY the weapon-viewmodel crop. PASS needs the skin change to beat both the absolute
    # floor and a multiple of the same-tick noise floor (so HUD/TAA jitter cannot fake a pass).
    $noiseJson = & python (Join-Path $automationRoot 'tools\image_diff.py') $realA $realB --crop $WeaponCrop
    $skinJson  = & python (Join-Path $automationRoot 'tools\image_diff.py') $realA $after --crop $WeaponCrop
    Add-Content -LiteralPath $logFile -Value "NOISE $noiseJson"
    Add-Content -LiteralPath $logFile -Value "SKIN  $skinJson"
    $noiseMean = [double]((Get-LastMatch $noiseJson '"mean":\s*([0-9.]+)'))
    $skinMean  = [double]((Get-LastMatch $skinJson  '"mean":\s*([0-9.]+)'))
    $gate = [Math]::Max($MinMeanDiff, $noiseMean * $NoiseMultiple)
    Write-Host "Weapon-crop mean diff: skin=$skinMean noise=$noiseMean gate=$gate" -ForegroundColor Cyan
    if ($skinMean -lt $gate) {
        throw "Weapon-region change ($skinMean) did not beat the gate ($gate = max($MinMeanDiff, noise*$NoiseMultiple)). Composite did not re-render the skin."
    }

    Write-Host "PASS: direct composite re-rendered the weapon skin (def=$def paint=$paint)." -ForegroundColor Green
} finally {
    if (Test-Netcon $Port) {
        Invoke-Netcon -Commands @('mirv_filmmaker cosmetics paintkitbridge 0', 'cl_paintkit_override 0') -ReadSeconds 1.0 | Out-Null
    }
}
