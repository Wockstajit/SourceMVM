[CmdletBinding()]
param(
    [string[]]$Maps = @('cs_office','de_cache','de_anubis','de_inferno','de_mirage','de_dust2','de_nuke','de_ancient','de_train','de_vertigo','de_overpass','de_boulder','de_fachwerk','cs_shelter','cs_italy'),
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/output/interior-lighting-matrix',
    [int]$PlanLimit = 12,
    [int]$Rooms = 2,
    [switch]$Resume,
    [switch]$UseReviewedRoute
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir -Maps $Maps
$results = @()
$resultPath = Join-Path $output 'results.json'
if ($Resume -and (Test-Path -LiteralPath $resultPath)) { $results = @(Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json) }
$culture = [Globalization.CultureInfo]::InvariantCulture
foreach ($map in $Maps) {
    $resumeDigest = $null
    $resumeManifest = Join-Path $env:APPDATA "HLAE/source_mvm/environment/occlusion/$map.json"
    if ($Resume -and (Test-Path -LiteralPath $resumeManifest)) {
        $resumeDigest = (Get-Content -LiteralPath $resumeManifest -Raw | ConvertFrom-Json).physicsDigest
    }
    $completedRows = @($results | Where-Object {
        $_.map -eq $map -and $_.physicsDigest -and $_.physicsDigest -eq $resumeDigest -and
        $_.validation.geometryValid -and $_.phaseState.live.environmentMap -eq $map -and $_.phaseState.live.environmentEnabled -eq 1 -and $_.phaseState.live.enabled -eq 1 -and $_.phaseState.live.gi -eq 1 -and
        $_.phaseState.noGi.enabled -eq 1 -and $_.phaseState.noGi.gi -eq 0 -and $_.phaseState.baked.enabled -eq 0
    })
    if ($Resume -and $completedRows.Count -ge $Rooms) {
        Write-Host "[$map] retaining completed state-checked captures"
        continue
    }
    $results = @($results | Where-Object {$_.map -ne $map})
    Write-Host "[$map] waiting for the game client" -ForegroundColor Cyan
    [void](Send-Netcon @('mvm_debug stop', 'tv_enable 1', "map $map") .12)
    [void](Wait-Map $map)
    $joined = $false
    for ($attempt=0; $attempt -lt 20 -and -not $joined; $attempt++) {
        $reply = Send-Netcon @('sv_cheats 1','mp_team_intro_time 0','jointeam 3 1','getpos') .15
        $pose = [regex]::Match($reply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
        if ($pose.Success) {
            $joined = [math]::Abs([double]$pose.Groups[1].Value) + [math]::Abs([double]$pose.Groups[2].Value) + [math]::Abs([double]$pose.Groups[3].Value) -gt 1
        }
        if (-not $joined) { Start-Sleep -Seconds 1 }
    }
    if (-not $joined) { throw "$map has no spawned player; refusing round restart." }
    [void](Send-Netcon @('exec dev') .2)
    Start-Sleep -Seconds 4
    # A first-time map has no cache until the environment requests extraction.
    [void](Send-Netcon @('mirv_filmmaker environment bind','mirv_filmmaker environment on','mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate relighting 1') .1)
    $cacheDir = Join-Path $env:APPDATA 'HLAE/source_mvm/environment/occlusion'
    $manifestPath = Join-Path $cacheDir "$map.json"
    $ready = $false
    $until = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $until -and -not $ready) {
        if (Test-Path -LiteralPath $manifestPath) {
            try {
                $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
                $ready = (Test-Path -LiteralPath (Join-Path $cacheDir ($manifest.physicsDigest + '.mvmocclusion'))) -and
                    (Test-Path -LiteralPath (Join-Path $cacheDir ($manifest.lightsDigest + '.lights.json')))
            } catch { }
        }
        if (-not $ready) { Start-Sleep -Seconds 1 }
    }
    if (-not $ready) { throw "No complete native map cache for $map after 180 seconds." }
    Wait-MapLighting $map
    if ($UseReviewedRoute) {
        $reviewedRoutes = Get-Content (Join-Path $repo 'Automation/fixtures/environment-interior-poses.json') -Raw | ConvertFrom-Json
        $route = $reviewedRoutes | Where-Object {$_.map -eq $map} | Select-Object -First 1
        if (-not $route -or $route.visualReview -ne 'accepted' -or $route.physicsDigest -ne $manifest.physicsDigest) {
            throw "Reviewed route needs visual review against the current collision revision on $map."
        }
        if ($Rooms -ne 1) { throw '-UseReviewedRoute requires -Rooms 1; the curated route contains one accepted pose per map.' }
        $plan = [pscustomobject]@{poses=@([pscustomobject]@{identity=$route.lightIdentity;pose=[pscustomobject]@{camera=$route.eye;viewPitch=$route.angles[0];viewYaw=$route.angles[1]}})}
    } else {
        $planPath = Join-Path $output "$map-interior-poses.json"
        & python (Join-Path $PSScriptRoot 'interior-camera.py') $map --plan $planPath --limit $PlanLimit
        if ($LASTEXITCODE -ne 0) { throw "Interior planning failed on $map." }
        $plan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
    }
    $selected = @()
    foreach ($candidate in $plan.poses) {
        if (-not $candidate.pose) { continue }
        $separated = $true
        foreach ($previous in $selected) {
            $d2=0; for ($a=0; $a -lt 3; $a++) { $d2 += [math]::Pow($previous.pose.camera[$a]-$candidate.pose.camera[$a],2) }
            if ($d2 -lt 128*128) { $separated = $false }
        }
        if ($separated) { $selected += $candidate }
        if ($selected.Count -ge $Rooms) { break }
    }
    if (-not $selected.Count) { throw "No interior pose on $map; no captures accepted." }
    [void](Send-Netcon @('noclip 1','cl_drawhud 0','r_drawviewmodel 0','mirv_filmmaker hud off','mirv_filmmaker environment on','mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate relighting 1','mirv_relight on','mirv_relight probecheck 0','mirv_relight shadows 1','mirv_relight ao 1','mirv_relight skyocclusion 1','mirv_relight reflections 1','mirv_relight bounce 1','mirv_relight gi 1','mvm_debug start') .06)
    $room=0
    foreach ($candidate in $selected) {
        $room++
        $eye = $candidate.pose.camera
        # A level/downward view includes walls, floor and fixtures for inspection.
        $yaw = $candidate.pose.viewYaw
        $pitch = 12
        if ($null -ne $candidate.pose.viewPitch) { $pitch = $candidate.pose.viewPitch }
        $position = 'setpos {0} {1} {2}' -f $eye[0].ToString($culture), $eye[1].ToString($culture), ($eye[2]-64).ToString($culture)
        $reply = Send-Netcon @($position, "setang $($pitch.ToString($culture)) $($yaw.ToString($culture)) 0", 'getpos') .15 'setpos\s+.*;setang'
        $actual = [regex]::Match($reply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
        if (-not $actual.Success) { throw "$map room $room has no actual camera pose." }
        $actualEye = @(); foreach ($a in 1..3) { $actualEye += [double]::Parse($actual.Groups[$a].Value,$culture) }
        for ($a=0;$a -lt 3;$a++) { if ([math]::Abs($actualEye[$a]-$eye[$a]) -gt 1) { throw "Camera did not reach $map room $room." } }
        $validationText = & python (Join-Path $PSScriptRoot 'interior-camera.py') $map --eye $actualEye[0] $actualEye[1] $actualEye[2]
        if ($LASTEXITCODE -ne 0) { throw "Invalid interior on ${map}: $validationText" }
        Start-Sleep -Seconds 3
        # Metadata replacement during a map transition can reset enabled. Set the
        # requested phase after the camera and cache are ready, and verify it at capture.
        [void](Send-Netcon @('mirv_relight on','mirv_relight gi 1') .3 '  reflections=')
        Start-Sleep -Seconds 2
        $live = Capture-Frame $map "room-$room-live" $actualEye @($pitch,$yaw) 1 1
        [void](Send-Netcon @('mirv_relight gi 0') .3 '  reflections=')
        Start-Sleep -Seconds 1
        $noGi = Capture-Frame $map "room-$room-no-gi" $actualEye @($pitch,$yaw) 1 0
        [void](Send-Netcon @('mirv_filmmaker environment off','mirv_relight off') .3 '  reflections=')
        $baked = Capture-Frame $map "room-$room-baked" $actualEye @($pitch,$yaw) 0
        [void](Send-Netcon @('mirv_filmmaker environment on','mirv_relight on','mirv_relight gi 1') .1)
        $results += [pscustomobject]@{map=$map;physicsDigest=$manifest.physicsDigest;reviewedRoute=$UseReviewedRoute.IsPresent;room=$room;eye=$actualEye;angles=@($pitch,$yaw);lightIdentity=$candidate.identity;validation=($validationText | ConvertFrom-Json);live=[IO.Path]::GetFileName($live.path);noGi=[IO.Path]::GetFileName($noGi.path);baked=[IO.Path]::GetFileName($baked.path);phaseState=[pscustomobject]@{live=$live.relight;noGi=$noGi.relight;baked=$baked.relight};finding='pending visual review'}
        ConvertTo-Json -InputObject @($results) -Depth 7 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
        Write-Host "[$map] room $room captured; geometry valid, visual review pending"
    }
}
[void](Send-Netcon @('mvm_debug stop') .2)
Write-Host "Interior lighting evidence: $output"
