<#
.SYNOPSIS
Install / restore / export the SOURCE:MVM startup intro -- WITHOUT touching the CS2 install.

.DESCRIPTION
CS2's launch intro is Panorama, not engine: panorama/scripts/intromovie.vts does

    $("#IntroMoviePlayer").SetMovie("file://{resources}/videos/intro.webm")

on the CSGOShowIntroMovie event.

HOW THIS WORKS (verified frame-by-frame on build 2000879, 2026-07-29):

We do NOT play the video ourselves -- CS2's own intro player loads our file, because our copy
outranks Valve's in the filesystem search order. AfxHookSource2 (main.cpp) registers

    <content mount>\ui_override     with PATH_ADD_TO_HEAD

so `ui_override\panorama\videos\intro.webm` wins the `file://{resources}/videos/intro.webm`
lookup. No timing race, no second panel, no overlap.

THE TRAP THAT COST TWO ROUNDS: the content mount itself is registered with the SDK default,
PATH_ADD_TO_TAIL -- literally "Last path searched". Dropping an intro.webm in there therefore
loses to csgo/ every time, no matter what the mount's own gameinfo.gi search order says. Two
earlier attempts died on this (a same-name override, then a uniquely-named file played by our
own Panorama panel, which raced Valve's intro and landed on top of it mid-playback). Only the
head-priority search path actually replaces the intro.

`-novid` is NOT a fix either: confirmed present in the cs2.exe command line and ignored by the
game on this build.

The CS2 install is never modified. Launch CS2 without the hook and Valve's intro plays exactly
as before: the override exists only while the mod is loaded.

The project's intro master is committed at Assets/startup-video/source-mvm-intro.mp4 and is what
`-Install` uses when no -Video is given. build.bat runs `-Install` on every build, so a fresh
clone or a wiped build/ tree re-encodes it automatically.

.PARAMETER Install
Install the committed intro (Assets/startup-video/source-mvm-intro.mp4).

.PARAMETER Video
Source video to install instead of the committed one (mp4/mov/webm/anything ffmpeg reads).

.PARAMETER Force
Re-encode even when the installed override is already newer than the source.

.PARAMETER Extract
Export the currently active intro to this path as MP4 (H.264 + AAC).

.PARAMETER Restore
Delete the override so Valve's intro plays again.

.PARAMETER Status
Show what is currently installed and exit.

.PARAMETER SkipAudio
Encode without an audio stream.

.EXAMPLE
powershell -File Automation\tools\set-startup-video.ps1 -Status

.EXAMPLE
powershell -File Automation\tools\set-startup-video.ps1 -Video "C:\media\my-intro.mp4"

.EXAMPLE
powershell -File Automation\tools\set-startup-video.ps1 -Restore

.NOTES
The override shares the mount with the FX pack; each feature owns only
its own subtree. build.bat's staging robocopy /MIR excludes ui_override\ for the same reason it
excludes unrelated trees -- and that exclusion only works with ABSOLUTE paths (build.bat builds
them from %~dp0; a relative path silently fails to match and /MIR deletes the tree).
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Install')]
    [switch]$Install,

    [Parameter(ParameterSetName = 'Install')]
    [string]$Video,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$Force,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$SkipAudio,

    [Parameter(ParameterSetName = 'Extract', Mandatory = $true)]
    [string]$Extract,

    [Parameter(ParameterSetName = 'Restore', Mandatory = $true)]
    [switch]$Restore,

    [Parameter(ParameterSetName = 'Status')]
    [switch]$Status,

    [string]$Cs2Dir = '',
    [string]$MountDir
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if ([string]::IsNullOrWhiteSpace($MountDir)) {
    $source2Mount = Join-Path $repoRoot 'build\source2-release\fx\source_mvm_fx'
    $legacyMount = Join-Path $repoRoot 'build\staging-release\fx\source_mvm_fx'
    if (Test-Path -LiteralPath $source2Mount) { $MountDir = $source2Mount }
    else { $MountDir = $legacyMount }
}
# ui_override/ is added to the search path at PATH_ADD_TO_HEAD by AfxHookSource2 (main.cpp), so
# it OUTRANKS csgo/ and we can use Valve's own filename -- CS2's own intro player then loads our
# file with no timing race at all. The content mount one level up is added at the TAIL and can
# never win this, which is what broke the first two attempts.
$overrideDir = Join-Path $MountDir 'ui_override\panorama\videos'
$overrideFile = Join-Path $overrideDir 'intro.webm'
# Files from the earlier (failed) approaches, in the tail-priority content mount. They never took
# effect; remove them so the mount cannot mislead a future session.
$staleDir = Join-Path $MountDir 'panorama\videos'
$staleNames = @('intro.webm', 'intro720p.webm', 'mvmtest.webm', 'mvm_intro.webm')
$stockIntro = Join-Path $Cs2Dir 'game\csgo\panorama\videos\intro.webm'
$repoSource = Join-Path $repoRoot 'Assets\startup-video\source-mvm-intro.mp4'

function Get-FFmpeg {
    foreach ($candidate in @(
        (Join-Path $repoRoot 'build\source2-release\bin\ffmpeg\ffmpeg.exe'),
        (Join-Path $repoRoot 'build\staging-release\bin\ffmpeg\ffmpeg.exe')
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $onPath = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "ffmpeg not found (looked in build\source2-release\bin\ffmpeg, build\staging-release\bin\ffmpeg, and on PATH)."
}

function Remove-StaleOverrides {
    foreach ($n in $staleNames) {
        $p = Join-Path $staleDir $n
        if (Test-Path -LiteralPath $p) {
            Remove-Item -LiteralPath $p -Force
            Write-Host "  removed stale $n (never took effect -- lost filename precedence to csgo)" -ForegroundColor DarkGray
        }
    }
}

switch ($PSCmdlet.ParameterSetName) {

    'Status' {
        Write-Host "SOURCE:MVM startup video" -ForegroundColor Cyan
        Write-Host "  mount  : $overrideDir"
        if (Test-Path -LiteralPath $overrideFile) {
            $f = Get-Item -LiteralPath $overrideFile
            Write-Host ("  ACTIVE : intro.webm ({0:N1} MB, {1})" -f ($f.Length / 1MB), $f.LastWriteTime) -ForegroundColor Green
            Write-Host "           head-priority override; CS2's own intro player loads it"
        } else {
            Write-Host "  ACTIVE : Valve's stock intro (nothing installed)" -ForegroundColor Yellow
            Write-Host "  install: -Install   (uses Assets\startup-video\source-mvm-intro.mp4)"
        }
        foreach ($n in $staleNames) {
            if (Test-Path -LiteralPath (Join-Path $staleDir $n)) {
                Write-Host "  note   : stale '$n' present; -Install or -Restore clears it." -ForegroundColor Yellow
            }
        }
        if (-not (Test-Path -LiteralPath $MountDir)) {
            Write-Host "  WARNING: mount dir missing -- build/stage first." -ForegroundColor Yellow
        }
        break
    }

    'Restore' {
        $removed = 0
        if (Test-Path -LiteralPath $overrideFile) { Remove-Item -LiteralPath $overrideFile -Force; $removed++ }
        Remove-StaleOverrides
        if ($removed -gt 0) {
            Write-Host "Removed the override. Valve's stock intro plays again." -ForegroundColor Green
        } else {
            Write-Host "Nothing to restore -- no override was installed." -ForegroundColor Yellow
        }
        break
    }

    'Extract' {
        $ffmpeg = Get-FFmpeg
        $source = if (Test-Path -LiteralPath $overrideFile) { $overrideFile } else { $stockIntro }
        if (-not (Test-Path -LiteralPath $source)) { throw "Source intro not found: $source" }
        $outDir = Split-Path -Parent $Extract
        if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        }
        Write-Host "Exporting $source" -ForegroundColor Cyan
        & $ffmpeg -y -hide_banner -loglevel error -i $source `
            -c:v libx264 -crf 16 -preset slow -pix_fmt yuv420p -movflags +faststart `
            -c:a aac -b:a 192k $Extract
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE)." }
        $f = Get-Item -LiteralPath $Extract
        Write-Host ("Wrote {0} ({1:N1} MB)" -f $f.FullName, ($f.Length / 1MB)) -ForegroundColor Green
        break
    }

    'Install' {
        if ([string]::IsNullOrWhiteSpace($Video)) { $Video = $repoSource }
        if (-not (Test-Path -LiteralPath $Video)) { throw "Source video not found: $Video" }
        $src = Get-Item -LiteralPath $Video

        # Idempotent: build.bat calls this every build, so skip the VP9 encode when the
        # installed override is already newer than the master. -Force re-encodes anyway.
        if (-not $Force -and (Test-Path -LiteralPath $overrideFile)) {
            if ((Get-Item -LiteralPath $overrideFile).LastWriteTime -ge $src.LastWriteTime) {
                Write-Host "Startup video already current ($($src.Name)); skipping encode." -ForegroundColor DarkGray
                Remove-StaleOverrides
                break
            }
        }

        $ffmpeg = Get-FFmpeg
        New-Item -ItemType Directory -Force -Path $overrideDir | Out-Null
        Remove-StaleOverrides
        Write-Host "Source: $($src.FullName)" -ForegroundColor Cyan

        # One file only: our own Panorama player loads it, so there is no need to mirror
        # Valve's two-variant (4K VP9 / 720p VP8) split.
        #
        # Resolution deliberately differs from Valve's 4K master: cap at 4K, NEVER upscale.
        # Upscaling a 1080p master spends encode time and bitrate re-describing pixels a scaler
        # invented; it cannot look better than the source, and Panorama scales to the panel
        # anyway. min(iw,3840) leaves a smaller source untouched.
        Write-Host "Encoding intro.webm (VP9, <=4K, 24 fps, $(if ($SkipAudio) { 'no audio' } else { 'Opus stereo' }))..." -ForegroundColor Cyan
        $encodeArgs = @(
            '-y', '-hide_banner', '-loglevel', 'error', '-i', $Video,
            '-c:v', 'libvpx-vp9', '-b:v', '0', '-crf', '30', '-row-mt', '1',
            '-pix_fmt', 'yuv420p', '-r', '24',
            '-vf', "scale='min(iw,3840)':'min(ih,2160)':force_original_aspect_ratio=decrease:force_divisible_by=2"
        )
        if ($SkipAudio) {
            $encodeArgs += @('-an')
        } else {
            $encodeArgs += @('-c:a', 'libopus', '-b:a', '128k', '-ac', '2', '-ar', '48000')
        }
        $encodeArgs += $overrideFile
        & $ffmpeg @encodeArgs
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed encoding the intro ($LASTEXITCODE)." }

        $out = Get-Item -LiteralPath $overrideFile
        Write-Host ""
        Write-Host ("Installed {0} ({1:N1} MB)" -f $out.FullName, ($out.Length / 1MB)) -ForegroundColor Green
        Write-Host "CS2 install untouched. Undo any time with -Restore." -ForegroundColor Green
        break
    }
}
