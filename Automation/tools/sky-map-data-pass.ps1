[CmdletBinding()]
param(
    [string]$GameRoot = '',
    # Source2Viewer-CLI (ValveResourceFormat). Not a build dependency -- only the asset
    # conversion/research tools below need it. Resolved as a checkout sitting NEXT TO this
    # repository, which is the documented layout; set SOURCEMVM_VRF_CLI to point elsewhere.
    [string]$VrfCli = $(if ($env:SOURCEMVM_VRF_CLI) { $env:SOURCEMVM_VRF_CLI } else { Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) 'ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe' }),
    [string]$Output = 'Docs/effects/sky-map-data.md',
    [string]$WorkDirectory = 'Automation/runs/sky-map-data-pass'
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($GameRoot)) { $GameRoot = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $VrfCli)) { throw "Source 2 Viewer CLI not found: $VrfCli" }
$mapsRoot = Join-Path $GameRoot 'game\csgo\maps'
if (-not (Test-Path -LiteralPath $mapsRoot)) { throw "CS2 map directory not found: $mapsRoot" }

# These are the non-vanity maps shown by the current Practice/Competitive browser.  The
# additional standard CS/ar maps are included so the output is useful when a user switches
# practice pools without changing the pass itself.
$mapNames = @(
    'de_ancient', 'de_anubis', 'de_cache', 'de_dust2', 'de_inferno', 'de_mirage',
    'de_nuke', 'de_overpass', 'de_train', 'de_vertigo', 'cs_office', 'cs_italy',
    'de_boulder', 'de_fachwerk', 'cs_shelter', 'ar_baggage', 'ar_pool_day', 'ar_shoots'
)

function Invoke-Vrf([string]$vpk, [string]$path, [string]$extension, [string]$destination) {
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if ((Test-Path -LiteralPath $destination) -and (Get-Item -LiteralPath $destination).Length -gt 0) { return }
    & $VrfCli -i $vpk -a --vpk_filepath $path --vpk_extensions $extension *> $destination
    if ($LASTEXITCODE -ne 0) { throw "Source 2 Viewer failed for $vpk ($path), exit $LASTEXITCODE" }
}

function First-Match([string]$text, [string]$pattern) {
    $m = [regex]::Match($text, $pattern, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ''
}

function Unique-Classes([string]$text, [string[]]$patterns) {
    $found = [System.Collections.Generic.List[string]]::new()
    foreach ($pattern in $patterns) {
        if ([regex]::IsMatch($text, ('classname\s*=\s*"' + [regex]::Escape($pattern) + '"'), 'IgnoreCase')) {
            if (-not $found.Contains($pattern)) { $found.Add($pattern) }
        }
    }
    return ($found -join ', ')
}

function Material-Info([string]$materialPath, [string]$pak, [string]$mapWork) {
    if ([string]::IsNullOrWhiteSpace($materialPath)) { return @{ Name = ''; Rotation = 'unknown'; Solar = $false } }
    $relative = $materialPath -replace '\\', '/'
    $out = Join-Path $mapWork ('material-' + (($relative -replace '[^A-Za-z0-9_.-]', '_')) + '.txt')
    try { Invoke-Vrf $pak ($relative + '_c') 'vmat_c' $out } catch { return @{ Name = $relative; Rotation = 'unavailable'; Solar = $false } }
    $materialText = Get-Content -LiteralPath $out -Raw
    $rotation = if ($materialText -match '(?im)(rotation|yaw|pitch|angle|transform)') { 'exposes rotation-like parameter' } else { 'no rotation parameter' }
    $solar = $materialText -match '(?im)m_name\s*=\s*"SolarPosition"'
    return @{ Name = $relative; Rotation = $rotation; Solar = $solar }
}

function First-Light-Environment([string]$text) {
    $match = [regex]::Match($text, 'classname\s*=\s*"light_environment"', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) { return @{ Summary = 'none'; Angles = ''; CastShadows = ''; Brightness = ''; Baked = '' } }
    # VRF prints one `keyValues3Data` block per entity.  Delimit on those markers so an `angles`
    # value from the previous entity cannot leak into this light's row.  The runtime probe remains
    # authoritative for the resolved scene-light object.
    $start = $text.LastIndexOf('keyValues3Data', $match.Index)
    if ($start -lt 0) { $start = [Math]::Max(0, $match.Index - 2500) }
    $next = $text.IndexOf('keyValues3Data', $match.Index)
    if ($next -lt 0) { $next = $text.Length }
    $block = $text.Substring($start, $next - $start)
    $angles = First-Match $block 'angles\s*=\s*\[\s*([^\]]+)\]'
    $cast = First-Match $block 'castshadows\s*=\s*((?:"[^"]+")|(?:true|false))'
    $brightness = First-Match $block 'brightness\s*=\s*([^\r\n]+)'
    $baked = First-Match $block 'baked_light_indexing\s*=\s*([^\r\n]+)'
    $summary = "light_environment angles=$angles castshadows=$cast brightness=$brightness baked_light_indexing=$baked"
    return @{ Summary = $summary; Angles = $angles; CastShadows = $cast; Brightness = $brightness; Baked = $baked }
}

$rows = [System.Collections.Generic.List[object]]::new()
$pak = Join-Path $GameRoot 'game\csgo\pak01_dir.vpk'
foreach ($map in $mapNames) {
    $vpk = Join-Path $mapsRoot ($map + '.vpk')
    if (-not (Test-Path -LiteralPath $vpk)) { continue }
    $mapWork = Join-Path $WorkDirectory $map
    $entityFile = Join-Path $mapWork 'entities.txt'
    $worldFile = Join-Path $mapWork 'world.txt'
    Invoke-Vrf $vpk ("maps/$map/entities/") 'vents_c' $entityFile
    Invoke-Vrf $vpk ("maps/$map/world.vwrld_c") 'vwrld_c' $worldFile
    $entityText = Get-Content -LiteralPath $entityFile -Raw
    $worldText = Get-Content -LiteralPath $worldFile -Raw

    $skyMaterial = First-Match $entityText 'skyname\s*=\s*(?:resource_name:)?"([^"]+\.vmat)"'
    if ([string]::IsNullOrWhiteSpace($skyMaterial)) {
        $skyMaterial = First-Match $entityText 'resource_name:"(materials/skybox/[^"]+\.vmat)"'
    }
    $matInfo = Material-Info $skyMaterial $pak $mapWork
    $lightInfo = First-Light-Environment $entityText
    $sunClasses = Unique-Classes $entityText @('light_environment', 'env_cascade_light', 'light_directional', 'global_light', 'C_LightEnvironmentEntity', 'C_LightDirectionalEntity', 'C_GlobalLight')
    $fogClasses = Unique-Classes $entityText @('env_cubemap_fog', 'env_gradient_fog', 'gradient_fog', 'C_GradientFog', 'env_volumetric_fog_controller')
    $fogMaterial = First-Match $entityText 'cubemapfogskymaterial\s*=\s*resource_name:"([^"]+\.vmat)"'
    $fogSkyEntity = First-Match $entityText 'cubemapfogskyentity\s*=\s*"([^"]+)"'
    $fogRefs = @()
    if ($entityText -match '(?im)classname\s*=\s*"env_cubemap_fog"') {
        if ($fogMaterial) { $fogRefs += "material=$fogMaterial" }
        if ($fogSkyEntity) { $fogRefs += "skyentity=$fogSkyEntity" }
    }
    $bakedShadow = if ($worldText -match 'm_bakedShadows\s*=\s*\[' -and $worldText -match 'direct_light_shadows\.vtex') { 'yes' } else { 'no/unknown' }
    $skySprite = if ($matInfo.Solar) { 'baked sky solar sprite (SolarPosition)' } elseif ([string]::IsNullOrWhiteSpace($sunClasses)) { 'likely baked into sky texture' } else { 'no SolarPosition; entity/engine path' }
    $rows.Add([pscustomobject]@{
        Map = $map
        SkyMaterial = if ($matInfo.Name) { $matInfo.Name } else { 'not found' }
        SkyRotation = $matInfo.Rotation
        SunEntity = if ($sunClasses) { $sunClasses } else { 'none' }
        BakedSun = $skySprite
        Fog = if ($fogClasses) { $fogClasses } else { 'none found' }
        FogReference = if ($fogRefs.Count) { $fogRefs -join '; ' } else { 'none' }
        SunLightAndCascades = $lightInfo.Summary
        BakedShadows = $bakedShadow
    })
}

$outFile = Join-Path (Get-Location) $Output
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outFile) | Out-Null
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# Per-map sky/light resource inventory')
$lines.Add('')
$lines.Add('Generated with Valve Resource Format / Source 2 Viewer CLI from the installed CS2 map VPKs. `Baked sun` is an asset-level classification; the runtime GPU probe remains authoritative for the sprite/effect path.')
$lines.Add('')
$lines.Add('| Map | Sky material | Rotation parameter | Sun entity | Baked sun | Fog | Fog reference | Sun/cascade setup | Baked shadows |')
$lines.Add('|---|---|---|---|---|---|---|---|---|')
foreach ($row in $rows) {
    $values = @($row.Map, $row.SkyMaterial, $row.SkyRotation, $row.SunEntity, $row.BakedSun, $row.Fog, $row.FogReference, $row.SunLightAndCascades, $row.BakedShadows)
    $lines.Add('| ' + (($values | ForEach-Object { ([string]$_) -replace '\|', '\\|' }) -join ' | ') + ' |')
}
$lines.Add('')
$lines.Add('## Runtime branch')
$lines.Add('')
$lines.Add('At runtime the hook selects the directional light entity when one resolves and keeps the sky/effect fallback for maps whose visible sun is a baked/material sprite. Sky and cubemap-fog transforms share one canonical quaternion; the fog-axis correction is internal and is not exposed as a second slider.')
[IO.File]::WriteAllLines($outFile, $lines)
Write-Host "Wrote $outFile ($($rows.Count) maps)"
