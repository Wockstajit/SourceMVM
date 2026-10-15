# Standalone Mirage A/B for the SST->CSM copy suppress candidate.
#
# Does not launch CS2. Requires a live HLAE+CS2 session with netcon and a DLL that
# includes ShadowStageProbe. Own script (not folded into verify-shadow-separation.ps1).
#
# Locked protocol matches Docs/effects/sun.md §3.5 / shadow-separation.md.
#
# Pass criteria (vs baseline, stairs+palm crops):
#   suppress ON  : stairs/palm mean dL comparable to baked OFF (>= +25 preferred)
#   live retained: fence crop stays within +/-5 of baseline (no staticgeo override)
#   restore      : crops within +/-1.5 of baseline after suppress off
#
#   powershell -ExecutionPolicy Bypass -File Automation/verify/verify-sst-csm-boundary.ps1
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/runs/sst-csm-boundary-verify'
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
# Approximate fence/live band near the Mirage corner (refine if capture res differs).
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

Write-Host 'verify-sst-csm-boundary: locking Mirage protocol...'
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
# Keep product baked/live ON — candidate is shadowstage suppress alone.
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadowstage suppress off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadowstage logging on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadowstage status' 0.6 | Out-Null
Start-Sleep -Milliseconds 800

$baseline = Invoke-Cap '01-baseline-both-on.png'

# Reference: known-good baked OFF for plant magnitude comparison.
Invoke-Net 'mirv_filmmaker world bakedshadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$bakedOff = Invoke-Cap '02-ref-baked-off.png'
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 700

# Candidate: suppress SST->CSM copy while baked cvar stays ON.
Invoke-Net 'mirv_filmmaker world shadowstage suppress on' 0.4 | Out-Null
Start-Sleep -Milliseconds 1000
$suppress = Invoke-Cap '03-sst-copy-suppressed.png'
Invoke-Net 'mirv_filmmaker world shadowstage status' 0.5 | Out-Null

# Soft-wall / alternate gates (classification only; not the candidate).
Invoke-Net 'mirv_filmmaker world shadowstage suppress off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world bakedshadows probe lb_shadow_map_cull_empty_mixed 1' 0.4 | Out-Null
Start-Sleep -Milliseconds 800
$cullMixed = Invoke-Cap '04-cull-empty-mixed-1.png'
Invoke-Net 'mirv_filmmaker world bakedshadows probe lb_shadow_map_cull_empty_mixed 0' 0.4 | Out-Null

# Restore A'
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world shadowstage suppress off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world light bakedindex force off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$restored = Invoke-Cap '05-restored.png'

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
    (Measure-Set 'baked_off_ref' $bakedOff),
    (Measure-Set 'sst_suppress' $suppress),
    (Measure-Set 'cull_empty_mixed' $cullMixed),
    (Measure-Set 'restored' $restored)
)

$summary = [ordered]@{
    protocol = [ordered]@{
        tick = 2936
        pos = '-1561.1 -2215.5 -164.3'
        ang = '10 -70 0'
        sunYaw = 140
        sunPitch = 30
        stairsCrop = $stairsCrop
        palmCrop = $palmCrop
        softCrop = $softCrop
        fenceCrop = $fenceCrop
    }
    comparisons = $sets
}

$stairsSuppress = [double]$sets[1].stairs.mean
$palmSuppress = [double]$sets[1].palm.mean
$stairsBaked = [double]$sets[0].stairs.mean
$palmBaked = [double]$sets[0].palm.mean
$fenceSuppress = [double]$sets[1].fence.mean
$restoreStairs = [double]$sets[3].stairs.mean
$restorePalm = [double]$sets[3].palm.mean

$plantOk = ($stairsSuppress -ge 25) -and ($palmSuppress -ge 25)
$plantLikeBaked = ($stairsSuppress -ge (0.5 * $stairsBaked)) -and ($palmSuppress -ge (0.5 * $palmBaked))
$liveOk = $fenceSuppress -le 5.0
$restoreOk = ($restoreStairs -le 1.5) -and ($restorePalm -le 1.5)
$pass = $plantOk -and $liveOk -and $restoreOk

$summary.verdict = [ordered]@{
    pass = $pass
    plantMoved = $plantOk
    plantLikeBakedOff = $plantLikeBaked
    liveFenceRetained = $liveOk
    restored = $restoreOk
    stairsSuppress = $stairsSuppress
    palmSuppress = $palmSuppress
    stairsBakedOff = $stairsBaked
    palmBakedOff = $palmBaked
    fenceSuppress = $fenceSuppress
}

$outJson = Join-Path $OutputDir 'summary.json'
($summary | ConvertTo-Json -Depth 8) | Set-Content -Encoding utf8 $outJson
Write-Host ("verify-sst-csm-boundary: PASS={0} plant={1} live={2} restore={3}" -f $pass, $plantOk, $liveOk, $restoreOk)
Write-Host "summary: $outJson"
if (-not $pass) { exit 1 }
