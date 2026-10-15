[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Map,
    [string]$OutputDir = '',
    [int]$Port = 29010,
    [string[]]$Kinds = @(),
    [int]$Limit = 0,
    [int]$Skip = 0,
    [string]$PosePlan = '',
    [switch]$ToggleShadows,
    [switch]$CompareBaked,
    [switch]$NoSheet
)
# Visits every authored light of the loaded map and records one HUD-free frame of what it lights.
# The camera stands beside the light's footprint looking along its aim (down for ceiling fixtures,
# across for sconces), so the capture shows the lit surface, its shadow edges and anything leaking
# through the wall the light hangs on. Lights sharing a position (Office pairs a barn with a rect
# panel) are one stop. -ToggleShadows adds a second frame with `mirv_relight shadows 0` so a
# too-dark footprint can be told apart from a blocked one; -CompareBaked adds the map's own baked
# lighting (Environment and Relight off) for the same pose. A contact sheet and index.html follow.
#
# Requires: CS2 running with the hook, the map loaded, `exec dev` done (sv_cheats for noclip/setpos)
# and the native map cache present (it is written on first map load).
$ErrorActionPreference = 'Stop'
if (-not $OutputDir) { $OutputDir = "Automation/output/light-audit/$Map" }
# The library resolves -OutputDir against the repo root into $output and owns Send-Netcon/Capture-Frame.
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
$outputRoot = $output
$mapReply = Send-Netcon @('mirv_filmmaker environment status') .3
if ($mapReply -notmatch "current='$([regex]::Escape($Map))'") { throw "Load $Map before starting its authored-light audit." }
[void](Send-Netcon @('mirv_filmmaker environment bind','mirv_filmmaker environment on') .2)
Wait-MapLighting $Map
[void](Send-Netcon @('mirv_filmmaker environment climate enabled 1','mirv_filmmaker environment climate relighting 1','mirv_relight on','mirv_relight gi 1') .3 '  reflections=')
$posePath = Join-Path $outputRoot 'interior-poses.json'
if ($PosePlan) {
    $posePath = [IO.Path]::GetFullPath((Join-Path $repo $PosePlan))
} else {
    & python (Join-Path $PSScriptRoot 'interior-camera.py') $Map --plan $posePath
    if ($LASTEXITCODE -ne 0) { throw 'Interior camera planning failed.' }
}
$plan = Get-Content -LiteralPath $posePath -Raw | ConvertFrom-Json
$plannedInteriors = $plan.poses


$cacheDir = Join-Path $env:APPDATA 'HLAE\source_mvm\environment\occlusion'
$manifestPath = Join-Path $cacheDir "$Map.json"
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No native map cache for $Map at $manifestPath; load the map once first." }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($plan.map -ne $Map -or $plan.physicsDigest -ne $manifest.physicsDigest) {
    throw 'Camera plan belongs to a different map or collision revision; regenerate and review it.'
}
$lightsPath = Join-Path $cacheDir ($manifest.lightsDigest + '.lights.json')
$lights = (Get-Content -LiteralPath $lightsPath -Raw | ConvertFrom-Json).lights |
    Where-Object { $_.kind -ne 'light_environment' -and (-not $Kinds.Count -or $Kinds -contains $_.kind) }

# One stop per position cluster (8 units), in authored order.
$stops = @()
foreach ($light in $lights) {
    $hit = $null
    foreach ($stop in $stops) {
        $d2=0
        for ($a=0;$a -lt 3;$a++) { $d2 += [math]::Pow($stop.position[$a]-$light.origin[$a],2) }
        if ($d2 -lt 64) { $hit=$stop; break }
    }
    if ($hit) { $hit.lights += $light }
    else { $stops += [pscustomobject]@{ position=@($light.origin); lights=@($light) } }
}
if ($Skip) { $stops = $stops | Select-Object -Skip $Skip }
if ($Limit) { $stops = $stops | Select-Object -First $Limit }
Write-Host ("{0}: {1} lights in {2} stops" -f $Map, @($lights).Count, @($stops).Count)

[void](Send-Netcon @('noclip 1', 'cl_drawhud 0', 'r_drawviewmodel 0', 'mirv_relight shadows 1') 0.1)
$index = @()
$n = 0
foreach ($stop in $stops) {
    $n++
    $primary = $stop.lights | Sort-Object { $_.kind -eq 'light_omni2' } | Select-Object -First 1
    $kindLabel = [string]::Join('+', @($stop.lights | ForEach-Object { $_.kind -replace '^light_', '' }))
    $planned = $plannedInteriors | Where-Object { $_.identity -eq $primary.identity } | Select-Object -First 1
    if (-not $planned.pose -or -not $planned.pose.validation.geometryValid) {
        Write-Warning "Skipping authored stop $($Skip+$n) ($($primary.identity)): no collision-checked interior camera."
        continue
    }
    $camera = $planned.pose.camera
    $angles = $planned.pose.angles
    $authoredStop = $Skip+$n
    $label = '{0:d3}-{1}' -f $authoredStop, $kindLabel
    # setpos places the player's feet; the eye sits 64 units higher.
    $pose = @(
        ('setpos {0:f1} {1:f1} {2:f1}' -f $camera[0], $camera[1], ($camera[2] - 64)),
        ('setang {0} {1} 0' -f $angles[0], $angles[1]))
    [void](Send-Netcon $pose 0.08)
    Start-Sleep -Milliseconds 1200
    $actualReply = Send-Netcon @('getpos') .3 'setpos\s+.*;setang'
    $actual = [regex]::Match($actualReply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
    if (-not $actual.Success) { throw "No actual camera pose for stop $n." }
    $eye = @(); foreach ($axis in 1..3) { $eye += [double]::Parse($actual.Groups[$axis].Value, [Globalization.CultureInfo]::InvariantCulture) }
    $errorSquared=0
    for ($a=0;$a -lt 3;$a++) { $errorSquared += [math]::Pow($eye[$a]-$camera[$a],2) }
    if ($errorSquared -gt 1) { throw "Camera did not reach stop $n; refusing capture." }
    $validationText = & python (Join-Path $PSScriptRoot 'interior-camera.py') $Map --eye $eye[0] $eye[1] $eye[2]
    if ($LASTEXITCODE -ne 0) { throw "Actual camera failed interior validation at stop ${n}: $validationText" }
    $validation = $validationText | ConvertFrom-Json
    $shot = Capture-Frame $Map $label $eye $angles 1 1
    $entry = [ordered]@{
        stop = $authoredStop; label = $label; kinds = $kindLabel
        actualEye = $eye; validation = $validation; physicsDigest = $manifest.physicsDigest; phaseState = [ordered]@{live=$shot.relight}
        position = @($stop.position | ForEach-Object { [math]::Round($_, 1) })
        camera = @($camera | ForEach-Object { [math]::Round($_, 1) }); angles = $angles
        lumens = @($stop.lights | ForEach-Object { $_.brightnessLumens }); range = $primary.range
        identities = @($stop.lights | ForEach-Object { $_.identity })
        frame = [IO.Path]::GetFileName($shot.path)
    }
    if ($ToggleShadows) {
        [void](Send-Netcon @('mirv_relight shadows 0') 0.08); Start-Sleep -Milliseconds 900
        $noShadowShot = Capture-Frame $Map "$label-noshadow" $eye $angles 1 1
        $entry.frameNoShadows = [IO.Path]::GetFileName($noShadowShot.path)
        $entry.phaseState.noShadows = $noShadowShot.relight
        [void](Send-Netcon @('mirv_relight shadows 1') 0.08)
    }
    if ($CompareBaked) {
        [void](Send-Netcon @('mirv_filmmaker environment off','mirv_relight off') .3 '  reflections='); Start-Sleep -Milliseconds 900
        $bakedShot = Capture-Frame $Map "$label-baked" $eye $angles 0
        $entry.frameBaked = [IO.Path]::GetFileName($bakedShot.path)
        $entry.phaseState.baked = $bakedShot.relight
        [void](Send-Netcon @('mirv_filmmaker environment on','mirv_relight on') .3 '  reflections='); Start-Sleep -Milliseconds 400
    }
    $index += [pscustomobject]$entry
    ConvertTo-Json -InputObject @($index) -Depth 7 | Set-Content -LiteralPath (Join-Path $outputRoot 'index.json') -Encoding UTF8
    Write-Host ("[{0}/{1}] {2} lumens={3} -> {4}" -f $n, @($stops).Count, $label, ($entry.lumens -join ','), $entry.frame)
}
[void](Send-Netcon @('cl_drawhud 1', 'r_drawviewmodel 1') 0.1)
ConvertTo-Json -InputObject @($index) -Depth 7 | Set-Content -LiteralPath (Join-Path $outputRoot 'index.json') -Encoding UTF8
if (-not $NoSheet) {
    & python (Join-Path $PSScriptRoot 'light-audit-sheet.py') $outputRoot
}
Write-Host "Audit written to $outputRoot"
