# Measures how the render pipeline scales with OUTPUT FPS, and with the number of passes captured
# in ONE job.
#
# WHY: a render job renders the whole selected pass set from a single source frame before the demo
# advances a tick, so N passes cost one seek and N redraws per frame -- NOT N separate jobs. This
# script proves that (one job, everything selected) and then walks the output frame rate up to
# 1000 fps to find where the pipeline stops keeping up.
#
# Preconditions: CS2 running with the hook, a demo loaded, the camera editor + render workspace
# open, and a camera source and range already chosen. The script changes ONLY the output fps and
# the stream selection.
#
#   powershell -ExecutionPolicy Bypass -File Automation\verify\verify-render-fps-scaling.ps1 `
#       -OutRoot C:\path\to\out -FpsList 120,300,600,1000
param(
    [int]$Port = 29010,
    [Parameter(Mandatory = $true)]
    [string]$OutRoot,
    [int[]]$FpsList = @(120, 300, 600, 1000),
    # "all" selects every runnable stream; "beauty" isolates the frame-rate cost from the pass cost.
    [ValidateSet('all', 'beauty')]
    [string]$Streams = 'all',
    [int]$JobTimeoutSeconds = 1800
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$netcon = Join-Path $automationRoot 'netcon\cs2-netcon.ps1'
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$renderRoot = Join-Path $OutRoot 'takes'
New-Item -ItemType Directory -Force -Path $renderRoot | Out-Null
$log = Join-Path $OutRoot 'netcon.log'

function Send-Cmds([string]$batch, [double]$read = 1.0) {
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    & $netcon -Port $Port -CommandBatch $batch -ReadSeconds $read -LogPath $log *> $null
    if (-not (Test-Path -LiteralPath $log)) { return '' }
    return (Get-Content -Raw -LiteralPath $log)
}

$setup = Send-Cmds "mirv_filmmaker render output path `"$renderRoot`"" 1.0
if ($setup -notmatch 'output folder set to') { throw "could not set output folder:`n$setup" }

$select = if ($Streams -eq 'all') {
    'mirv_filmmaker render streams clear;;mirv_filmmaker render streams selectall'
} else {
    'mirv_filmmaker render streams clear;;mirv_filmmaker render streams add beauty'
}
$out = Send-Cmds "$select;;mirv_filmmaker render streams list" 1.5
$streamCount = ([regex]::Matches($out, '(?m)^\s+\*\s')).Count
Write-Host "streams selected: $streamCount"

$rows = @()
foreach ($fps in $FpsList) {
    $before = @{}
    foreach ($d in @(Get-ChildItem -LiteralPath $renderRoot -Directory -Filter 'mvm_render_*' -ErrorAction SilentlyContinue)) {
        $before[$d.FullName] = $true
    }
    $v = Send-Cmds "mirv_filmmaker render output fps $fps;;mirv_filmmaker render validate" 1.2
    if ($v -match 'validation failed') {
        Write-Warning "[fps $fps] validation failed"
        $rows += [pscustomobject]@{ fps=$fps; streams=$streamCount; status='validation-failed' }
        continue
    }
    $started = Get-Date
    Send-Cmds 'mirv_filmmaker render start' 2.0 | Out-Null

    $takeDir = $null
    $spot = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $spot) {
        $new = @(Get-ChildItem -LiteralPath $renderRoot -Directory -Filter 'mvm_render_*' -ErrorAction SilentlyContinue |
            Where-Object { -not $before.ContainsKey($_.FullName) } | Sort-Object CreationTimeUtc -Descending)
        if ($new.Count -gt 0) { $takeDir = $new[0].FullName; break }
        Start-Sleep -Milliseconds 400
    }
    if (-not $takeDir) {
        Write-Warning "[fps $fps] no take directory appeared"
        $rows += [pscustomobject]@{ fps=$fps; streams=$streamCount; status='no-take' }
        continue
    }

    $manifest = $null
    $deadline = (Get-Date).AddSeconds($JobTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $m = Join-Path $takeDir 'manifest.json'
        if (Test-Path -LiteralPath $m) {
            try { $j = Get-Content -Raw -LiteralPath $m | ConvertFrom-Json } catch { $j = $null }
            if ($j -and @('completed','failed','canceled') -contains $j.status) { $manifest = $j; break }
        }
        Start-Sleep -Seconds 2
    }
    $elapsed = ((Get-Date) - $started).TotalSeconds
    if (-not $manifest) {
        Write-Warning "[fps $fps] timed out after $([int]$elapsed)s"
        $rows += [pscustomobject]@{ fps=$fps; streams=$streamCount; status='timeout'; seconds=[math]::Round($elapsed,1) }
        continue
    }

    # Frames actually on disk, summed across every stream folder: the honest measure of how much
    # image data the job produced, not just how many output frames the manifest counted.
    $tgaCount = 0
    foreach ($tf in @(Get-ChildItem -LiteralPath $takeDir -Directory | Where-Object { $_.Name -match '^take\d+$' })) {
        foreach ($sd in @(Get-ChildItem -LiteralPath $tf.FullName -Directory)) {
            $tgaCount += @(Get-ChildItem -LiteralPath $sd.FullName -Filter '*.tga').Count
        }
    }
    $row = [pscustomobject]@{
        fps          = $fps
        streams      = $streamCount
        status       = $manifest.status
        framesExp    = [int]$manifest.framesExpected
        framesWrote  = [int]$manifest.framesWritten
        imagesOnDisk = $tgaCount
        seconds      = [math]::Round($elapsed, 1)
        imagesPerSec = if ($elapsed -gt 0) { [math]::Round($tgaCount / $elapsed, 1) } else { 0 }
        takeDir      = $takeDir
    }
    $rows += $row
    Write-Host ("[fps {0,4}] {1} {2}/{3} frames, {4} images, {5}s ({6} img/s)" -f `
        $row.fps, $row.status, $row.framesWrote, $row.framesExp, $row.imagesOnDisk, $row.seconds, $row.imagesPerSec)
}

$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutRoot 'fps-scaling.json') -Encoding UTF8
Write-Host "`nWrote $($rows.Count) rows -> $(Join-Path $OutRoot 'fps-scaling.json')"
