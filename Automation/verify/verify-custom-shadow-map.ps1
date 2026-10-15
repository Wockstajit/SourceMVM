# Standalone Mirage A/B for Phase B custom directional shadow maps.
#
# Own script — not folded into verify-contact-shadows / verify-shadow-separation.
#
# Protocol: Docs/effects/sun.md §3.5
# Steps: baseline (engine on) -> engine off -> custom map on (engine still off) -> restore
#
#   powershell -ExecutionPolicy Bypass -File Automation/verify/verify-custom-shadow-map.ps1
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/runs/custom-shadow-map-verify'
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
$softCrop = '700,320,900,600'
$fenceCrop = '200,700,500,1000'

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

Write-Host 'verify-custom-shadow-map: locking Mirage protocol...'
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
Invoke-Net 'mirv_filmmaker world shadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow status' 0.6 | Out-Null
Start-Sleep -Milliseconds 800

$baseline = Invoke-Cap '01-baseline-engine-on.png'

Invoke-Net 'mirv_filmmaker world shadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 1000
$engineOff = Invoke-Cap '02-engine-shadows-off.png'

Invoke-Net 'mirv_filmmaker world customshadow on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow strength 0.65' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow reach 2.50' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow bias 0.00080' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world customshadow steps 16' 0.4 | Out-Null
Start-Sleep -Milliseconds 1500
$shaderOn = Invoke-Cap '03-shader-map-on.png'
Invoke-Net 'mirv_filmmaker world customshadow status' 0.5 | Out-Null

Invoke-Net 'mirv_filmmaker world customshadow off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 1000
$restored = Invoke-Cap '04-restored.png'
Invoke-Net 'mvm_debug stop' 0.4 | Out-Null

function Measure-Set([string]$Label, [string]$Image) {
    return [ordered]@{
        label = $Label
        stairs = Get-DiffMean $baseline $Image $stairsCrop
        palm = Get-DiffMean $baseline $Image $palmCrop
        soft = Get-DiffMean $baseline $Image $softCrop
        fence = Get-DiffMean $baseline $Image $fenceCrop
    }
}

$sets = @(
    (Measure-Set 'engine_off' $engineOff),
    (Measure-Set 'shader_on' $shaderOn),
    (Measure-Set 'restored' $restored)
)

$report = [ordered]@{
    protocol = 'mirage-tick-2936'
    phase = 'b-shadowmap'
    baseline = $baseline
    sets = $sets
}
$reportPath = Join-Path $OutputDir 'report.json'
($report | ConvertTo-Json -Depth 8) | Set-Content -Path $reportPath -Encoding UTF8
Write-Host "wrote $reportPath"
foreach ($s in $sets) {
    Write-Host ("{0}: stairs={1:n2} palm={2:n2} soft={3:n2} fence={4:n2}" -f `
        $s.label, $s.stairs.mean, $s.palm.mean, $s.soft.mean, $s.fence.mean)
}
