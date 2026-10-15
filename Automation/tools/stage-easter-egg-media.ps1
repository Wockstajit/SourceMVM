<#
.SYNOPSIS
Encodes the easter-egg video masters (Assets\easter-eggs\*.mp4) into the content mount's
head-priority ui_override\panorama\videos\ folder, where the easter-egg overlay plays them as
file://{resources}/videos/<name>.webm.

.DESCRIPTION
Idempotent: a clip is re-encoded only when its master is newer than the staged webm, so the build
step costs nothing on a normal build. VP9, never upscaled, and VORBIS stereo audio: CS2's Movie
panel plays an attached audio stream only for Vorbis (Valve's trailer_*.webm, which play with
sound, are Vorbis; an Opus track -- what the muted intro uses -- played the clip in silence).

Masters and their staged names:
    sony-vegas-sucks.mp4  ->  mvm_egg_vegas.webm   (the render-start Sony Vegas intervention)

.EXAMPLE
powershell -File Automation\tools\stage-easter-egg-media.ps1 -MountDir build\source2-release\fx\source_mvm_fx
#>
[CmdletBinding()]
param(
    [string]$MountDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($MountDir)) {
    $MountDir = Join-Path $repoRoot 'build\source2-release\fx\source_mvm_fx'
}
$videoDir = Join-Path $MountDir 'ui_override\panorama\videos'
$clips = @(
    @{ Master = 'sony-vegas-sucks.mp4'; Staged = 'mvm_egg_vegas.webm' }
)

function Get-FFmpeg {
    foreach ($candidate in @(
        (Join-Path $repoRoot 'build\source2-release\bin\ffmpeg\ffmpeg.exe'),
        (Join-Path $repoRoot 'build\staging-release\bin\ffmpeg\ffmpeg.exe')
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $onPath = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'ffmpeg not found (build\source2-release\bin\ffmpeg, build\staging-release\bin\ffmpeg, or PATH).'
}

# Bundled osu! easter-egg beatmaps (Assets\easter-eggs\osu\<set>\) go beside the hook, in
# <stage root>\bin\osu\Songs\ -- the hook reads them from GetHlaeFolder()\osu\Songs. The stage root
# is two levels above the content mount (build\source2-release\fx\source_mvm_fx).
$stageRoot = Split-Path -Parent (Split-Path -Parent $MountDir)
$osuSrc = Join-Path $repoRoot 'Assets\easter-eggs\osu'
if (Test-Path -LiteralPath $osuSrc) {
    $osuDest = Join-Path $stageRoot 'bin\osu\Songs'
    New-Item -ItemType Directory -Force -Path $osuDest | Out-Null
    Get-ChildItem -LiteralPath $osuSrc -Directory | ForEach-Object {
        $dest = Join-Path $osuDest $_.Name
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Get-ChildItem -LiteralPath $_.FullName -File | Copy-Item -Destination $dest -Force
        Write-Host "  osu map: $($_.Name)" -ForegroundColor DarkGray
    }
}

# The bundled default osu! skin (Assets\easter-eggs\osu-skin\<skin>\) goes beside the hook in
# <stage root>\bin\osu\Skin\<skin>\ -- OsuSkin.cpp uses it when the player has no osu! skin of their own.
$skinSrc = Join-Path $repoRoot 'Assets\easter-eggs\osu-skin'
if (Test-Path -LiteralPath $skinSrc) {
    $skinDest = Join-Path $stageRoot 'bin\osu\Skin'
    Get-ChildItem -LiteralPath $skinSrc -Directory | ForEach-Object {
        $dest = Join-Path $skinDest $_.Name
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Get-ChildItem -LiteralPath $_.FullName -File | Copy-Item -Destination $dest -Force
        Write-Host "  osu skin: $($_.Name)" -ForegroundColor DarkGray
    }
}

# The UwU egg's sounds (Assets\easter-eggs\uwu\) go beside the hook in <stage root>\bin\easter-eggs\uwu\.
$uwuSrc = Join-Path $repoRoot 'Assets\easter-eggs\uwu'
if (Test-Path -LiteralPath $uwuSrc) {
    $uwuDest = Join-Path $stageRoot 'bin\easter-eggs\uwu'
    New-Item -ItemType Directory -Force -Path $uwuDest | Out-Null
    Get-ChildItem -LiteralPath $uwuSrc -File -Filter *.mp3 | Copy-Item -Destination $uwuDest -Force
    Write-Host '  uwu sounds staged' -ForegroundColor DarkGray
}

New-Item -ItemType Directory -Force -Path $videoDir | Out-Null
foreach ($clip in $clips) {
    $master = Join-Path $repoRoot ('Assets\easter-eggs\' + $clip.Master)
    $staged = Join-Path $videoDir $clip.Staged
    if (-not (Test-Path -LiteralPath $master)) { throw "Missing easter-egg master: $master" }
    if (-not $Force -and (Test-Path -LiteralPath $staged) -and
        (Get-Item -LiteralPath $staged).LastWriteTime -ge (Get-Item -LiteralPath $master).LastWriteTime) {
        Write-Host "  $($clip.Staged) current" -ForegroundColor DarkGray
        continue
    }
    $ffmpeg = Get-FFmpeg
    Write-Host "  encoding $($clip.Staged)..." -ForegroundColor Cyan
    & $ffmpeg -y -hide_banner -loglevel error -i $master `
        -c:v libvpx-vp9 -b:v 0 -crf 32 -row-mt 1 -pix_fmt yuv420p `
        -vf "scale='min(iw,1920)':'min(ih,1080)':force_original_aspect_ratio=decrease:force_divisible_by=2" `
        -c:a libvorbis -q:a 5 -ac 2 -ar 48000 $staged
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed encoding $($clip.Staged) ($LASTEXITCODE)." }
    Write-Host ("  staged {0} ({1:N1} MB)" -f $staged, ((Get-Item -LiteralPath $staged).Length / 1MB)) -ForegroundColor Green
}
