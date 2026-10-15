# Standalone Mirage A/B verifier for baked vs live shadow separation.
#
# Requires a live CS2+HLAE session already playing the repaired Mirage demo with
# netcon on -Port (default 29010). Does not launch CS2 and does not share build
# logic with other verifiers.
#
# Protocol (locked):
#   demo_gototick 2936, pause, freecam (-1561.1 -2215.5 -164.3) ang (10 -70 0) fov 90
#   sun yaw 140 / pitch 30, mirv_filmmaker mouse free
#
# Captures baseline / baked-off / live-off / both-off / restored and reports
# regional pixel metrics for the stairs plant crop and ground-palm crop.
#
#   powershell -ExecutionPolicy Bypass -File Automation/verify/verify-shadow-separation.ps1
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/runs/shadow-separation-verify'
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot

$net = Join-Path $repoRoot 'Automation\netcon\cs2-netcon.ps1'
$cap = Join-Path $repoRoot 'Automation\capture\capture-game-window.ps1'
$diff = Join-Path $repoRoot 'Automation\tools\image_diff.py'
if (-not (Test-Path $net)) { throw "missing $net" }
if (-not (Test-Path $cap)) { throw "missing $cap" }
if (-not (Test-Path $diff)) { throw "missing $diff" }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$stairsCrop = '650,650,950,950'
$palmCrop = '1000,750,1400,1050'

function Invoke-Net([string]$Command, [double]$ReadSeconds = 0.55) {
    pwsh -File $net -Port $Port -ReadSeconds $ReadSeconds -CommandBatch $Command
}

function Invoke-Cap([string]$Name) {
    $path = Join-Path $OutputDir $Name
    powershell.exe -ExecutionPolicy Bypass -File $cap -Out $path | Out-Null
    return $path
}

function Get-DiffMean([string]$Before, [string]$After, [string]$Crop) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $json = & python $diff $Before $After --crop $Crop 2>$null | Select-Object -Last 1
    $ErrorActionPreference = $prev
    if (-not $json) { throw "image_diff produced no JSON for $After crop=$Crop" }
    return ($json | ConvertFrom-Json)
}

Write-Host 'verify-shadow-separation: locking Mirage protocol pose...'
Invoke-Net 'mvm_debug start' 0.4 | Out-Null
Invoke-Net 'demo_pause' 0.3 | Out-Null
Invoke-Net 'demo_gototick 2936' 0.4 | Out-Null
Start-Sleep -Seconds 1
Invoke-Net 'demo_pause' 0.3 | Out-Null
Invoke-Net 'mirv_input camera' 0.3 | Out-Null
Invoke-Net 'mirv_input position -1561.1 -2215.5 -164.3' 0.3 | Out-Null
Invoke-Net 'mirv_input angles 10 -70 0' 0.3 | Out-Null
Invoke-Net 'mirv_input fov 90' 0.3 | Out-Null
Invoke-Net 'mirv_filmmaker world light yaw 140' 0.3 | Out-Null
Invoke-Net 'mirv_filmmaker world light pitch 30' 0.3 | Out-Null
Invoke-Net 'mirv_filmmaker mouse free' 0.3 | Out-Null
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 800

$baseline = Invoke-Cap '01-baseline-both-on.png'
Invoke-Net 'mirv_filmmaker world bakedshadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$bakedOff = Invoke-Cap '02-baked-off-live-on.png'
Invoke-Net 'mirv_filmmaker world bakedshadows probe get lb_enable_baked_shadows' 0.4 | Out-Null

Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$liveOff = Invoke-Cap '03-baked-on-live-off.png'
Invoke-Net 'mirv_filmmaker world bakedshadows probe get csm_max_num_cascades_override' 0.4 | Out-Null

Invoke-Net 'mirv_filmmaker world bakedshadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$bothOff = Invoke-Cap '04-both-off.png'

Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$restored = Invoke-Cap '05-restored-both-on.png'

$results = [ordered]@{
    protocol = [ordered]@{
        tick = 2936
        pos = '-1561.1 -2215.5 -164.3'
        ang = '10 -70 0'
        sunYaw = 140
        sunPitch = 30
        stairsCrop = $stairsCrop
        palmCrop = $palmCrop
        cs2ClientVersion = '2000879'
    }
    bakedOffStairs = Get-DiffMean $baseline $bakedOff $stairsCrop
    bakedOffPalm = Get-DiffMean $baseline $bakedOff $palmCrop
    liveOffStairs = Get-DiffMean $baseline $liveOff $stairsCrop
    liveOffPalm = Get-DiffMean $baseline $liveOff $palmCrop
    bothOffStairs = Get-DiffMean $baseline $bothOff $stairsCrop
    bothOffPalm = Get-DiffMean $baseline $bothOff $palmCrop
    restoredStairs = Get-DiffMean $baseline $restored $stairsCrop
    restoredPalm = Get-DiffMean $baseline $restored $palmCrop
}

# Proven thresholds from Mirage repaired-demo A/B (2026-08-06):
# baked OFF stairs/palm mean >> 10; live OFF stairs/palm mean < 5; restore ~0.
$checks = [ordered]@{
    bakedOffStairsHigh = [double]$results.bakedOffStairs.mean -ge 10.0
    bakedOffPalmHigh = [double]$results.bakedOffPalm.mean -ge 10.0
    liveOffStairsLow = [double]$results.liveOffStairs.mean -lt 5.0
    liveOffPalmLow = [double]$results.liveOffPalm.mean -lt 5.0
    restoredStairsNearZero = [double]$results.restoredStairs.mean -lt 0.5
    restoredPalmNearZero = [double]$results.restoredPalm.mean -lt 0.5
}
$results.checks = $checks
$results.pass = -not ($checks.Values -contains $false)

$summaryPath = Join-Path $OutputDir 'summary.json'
($results | ConvertTo-Json -Depth 6) | Set-Content -Path $summaryPath -Encoding utf8
Write-Host ($results | ConvertTo-Json -Depth 6)

if (-not $results.pass) {
    throw "shadow separation verify FAILED - see $summaryPath"
}
Write-Host "shadow separation verify PASSED - $summaryPath"
exit 0
