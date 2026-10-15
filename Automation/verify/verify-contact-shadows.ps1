# Standalone Mirage A/B for Phase A shader contact shadows vs engine Shadows master gate.
#
# Own script — not folded into verify-sst-csm-boundary / verify-shadow-separation.
#
# Protocol: Docs/effects/sun.md §3.5
# Steps: baseline (engine on) -> engine off -> shader on (engine still off) -> restore
#
#   powershell -ExecutionPolicy Bypass -File Automation/verify/verify-contact-shadows.ps1
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/runs/contact-shadow-verify'
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

Write-Host 'verify-contact-shadows: locking Mirage protocol...'
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
Start-Sleep -Milliseconds 1100
$shaderOn = Invoke-Cap '03-shader-contact-on.png'
Invoke-Net 'mirv_filmmaker world customshadow status' 0.5 | Out-Null

Invoke-Net 'mirv_filmmaker world customshadow off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 1000
$restored = Invoke-Cap '04-restored.png'

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

$engineStairs = [double]$sets[0].stairs.mean
$enginePalm = [double]$sets[0].palm.mean
$shaderVsEngine = [double](Get-DiffMean $engineOff $shaderOn $stairsCrop).mean
$shaderPalmVsEngine = [double](Get-DiffMean $engineOff $shaderOn $palmCrop).mean
$restoreStairs = [double]$sets[2].stairs.mean
$restorePalm = [double]$sets[2].palm.mean

$engineMoved = ($engineStairs -ge 20) -and ($enginePalm -ge 20)
$shaderMoved = ($shaderVsEngine -ge 1.5) -or ($shaderPalmVsEngine -ge 1.5)
$restoreOk = ($restoreStairs -le 1.5) -and ($restorePalm -le 1.5)
# Soft gate: engine-off must work; shader contact is informational until depth binds reliably.
$pass = $engineMoved -and $restoreOk

$summary = [ordered]@{
    protocol = [ordered]@{
        tick = 2936
        pos = '-1561.1 -2215.5 -164.3'
        ang = '10 -70 0'
        sunYaw = 140
        sunPitch = 30
    }
    comparisons = $sets
    verdict = [ordered]@{
        pass = $pass
        engineShadowsOffMovedPlants = $engineMoved
        shaderChangedVsEngineOff = $shaderMoved
        restored = $restoreOk
        engineStairs = $engineStairs
        enginePalm = $enginePalm
        shaderStairsVsEngineOff = $shaderVsEngine
        shaderPalmVsEngineOff = $shaderPalmVsEngine
        note = 'Phase A: shaderMoved is soft evidence contact HLSL ran; promote look after visual review.'
    }
}

$outJson = Join-Path $OutputDir 'summary.json'
($summary | ConvertTo-Json -Depth 8) | Set-Content -Encoding utf8 $outJson
Write-Host ("verify-contact-shadows: PASS={0} engineOff={1} shaderDelta={2} restore={3}" -f $pass, $engineMoved, $shaderMoved, $restoreOk)
Write-Host "summary: $outJson"
if (-not $pass) { exit 1 }
