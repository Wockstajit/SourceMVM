# Standalone Mirage A/B for the per-light baked-shadow-index (+0x154) candidate.
#
# Own script — not folded into verify-sst-csm-boundary.ps1 or verify-shadow-separation.ps1.
# Requires live HLAE+CS2 with netcon and a DLL that includes LightFx bakedindex research controls.
#
# Pass criteria (vs baseline):
#   force ON   : stairs/palm mean dL comparable to baked OFF (>= +25 preferred)
#   live retained: fence crop within +/-5 of baseline
#   restore    : crops within +/-1.5 of baseline after force off
#   yaw check  : with force ON, yaw 70 vs 140 still moves fence or soft band (live CSM alive)
#
#   powershell -ExecutionPolicy Bypass -File Automation/verify/verify-bakedindex-boundary.ps1
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/runs/bakedindex-boundary-verify'
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

Write-Host 'verify-bakedindex-boundary: locking Mirage protocol...'
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
Invoke-Net 'mirv_filmmaker world shadowstage suppress off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world light bakedindex force off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world light bakedindex log on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world light bakedindex' 0.6 | Out-Null
Start-Sleep -Milliseconds 800

$baseline = Invoke-Cap '01-baseline-both-on.png'

Invoke-Net 'mirv_filmmaker world bakedshadows off' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$bakedOff = Invoke-Cap '02-ref-baked-off.png'
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Start-Sleep -Milliseconds 700

# Candidate: force baked index to 0xFF on submitted lights while product baked cvar stays ON.
Invoke-Net 'mirv_filmmaker world light bakedindex force on' 0.4 | Out-Null
Start-Sleep -Milliseconds 1100
$forced = Invoke-Cap '03-bakedindex-force-none.png'
Invoke-Net 'mirv_filmmaker world light bakedindex' 0.5 | Out-Null

# Sun-yaw live check while force remains ON.
Invoke-Net 'mirv_filmmaker world light yaw 70' 0.4 | Out-Null
Start-Sleep -Milliseconds 900
$yaw70 = Invoke-Cap '04-force-on-yaw70.png'
Invoke-Net 'mirv_filmmaker world light yaw 140' 0.4 | Out-Null
Start-Sleep -Milliseconds 800

# Restore A'
Invoke-Net 'mirv_filmmaker world light bakedindex force off' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world bakedshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world dynamicshadows on' 0.4 | Out-Null
Invoke-Net 'mirv_filmmaker world light yaw 140' 0.3 | Out-Null
Start-Sleep -Milliseconds 1000
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
    (Measure-Set 'bakedindex_force' $forced),
    (Measure-Set 'force_yaw70' $yaw70),
    (Measure-Set 'restored' $restored)
)

$stairsForce = [double]$sets[1].stairs.mean
$palmForce = [double]$sets[1].palm.mean
$stairsBaked = [double]$sets[0].stairs.mean
$palmBaked = [double]$sets[0].palm.mean
$fenceForce = [double]$sets[1].fence.mean
$restoreStairs = [double]$sets[3].stairs.mean
$restorePalm = [double]$sets[3].palm.mean
$yawFence = [double]$sets[2].fence.mean
$yawSoft = [double]$sets[2].soft.mean

$plantOk = ($stairsForce -ge 25) -and ($palmForce -ge 25)
$plantLikeBaked = ($stairsForce -ge (0.5 * $stairsBaked)) -and ($palmForce -ge (0.5 * $palmBaked))
$liveOk = $fenceForce -le 5.0
$restoreOk = ($restoreStairs -le 1.5) -and ($restorePalm -le 1.5)
$yawAlive = ($yawFence -ge 2.0) -or ($yawSoft -ge 2.0)
$pass = $plantOk -and $liveOk -and $restoreOk

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
    verdict = [ordered]@{
        pass = $pass
        plantMoved = $plantOk
        plantLikeBakedOff = $plantLikeBaked
        liveFenceRetained = $liveOk
        restored = $restoreOk
        yawResponsiveUnderForce = $yawAlive
        stairsForce = $stairsForce
        palmForce = $palmForce
        stairsBakedOff = $stairsBaked
        palmBakedOff = $palmBaked
        fenceForce = $fenceForce
        yawFenceVsBaseline = $yawFence
        yawSoftVsBaseline = $yawSoft
    }
}

$outJson = Join-Path $OutputDir 'summary.json'
($summary | ConvertTo-Json -Depth 8) | Set-Content -Encoding utf8 $outJson
Write-Host ("verify-bakedindex-boundary: PASS={0} plant={1} live={2} restore={3} yaw={4}" -f $pass, $plantOk, $liveOk, $restoreOk, $yawAlive)
Write-Host "summary: $outJson"
if (-not $pass) { exit 1 }
