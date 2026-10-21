<#
.SYNOPSIS
    Stages everything a self-contained SourceMovieMaker installer carries.

.DESCRIPTION
    `npm run package:bundled` builds ONE installer exe with nothing left to download. This script
    stages what goes inside it, under bundle\ (git-ignored), which electron-builder copies to
    resources\bundle\:

        bundle\runtime\...               hook DLL, injector, demo helpers, resources (as the payload)
        bundle\fx\source_mvm_fx\         the FX pack as ONE file (source_mvm_fx.vpk, FX/tools/pack-fx-vpk.py)
                                         beside its loose gameinfo.gi and ui_override\ (the SOURCE:MVM
                                         intro, which must stay loose to outrank csgo/)
        bundle\dlss5\...                 Streamline, both nvngx_dlssnr runtimes, the RenoDX add-on,
                                         ReShade64.dll (with its license)
        bundle\bundle.json               version + a stamp of the contents

    On first start (and after every update) the launcher copies runtime and fx into its payload
    folder (electron/runtime-payload.cjs installBundled), and DLSS 5 installs from dlss5 with the
    runtime picked from the GPU (electron/dlss-tool.cjs detectVariant).

    It only READS an already-built tree. Build the hook first (build-source2.bat, or the CMake
    install steps in CLAUDE.md). It never compiles anything.

.PARAMETER StageRoot
    Staged CS2 build. Defaults to <repo>\build\source2-release.

.PARAMETER FxRoot
    Compiled FX pack. Defaults to <repo>\FX\compiled\source_mvm_fx.

.PARAMETER DlssDir
    The DLSS 5 payload folder ("dlss 5" at the repo root): streamline\, addons\, the two runtime
    folders, and reshade\ReShade_Setup_*_Addon.exe (or reshade\ReShade64.dll) + its license.
#>
[CmdletBinding()]
param(
    [string]$StageRoot,
    [string]$FxRoot,
    [string]$DlssDir
)

$ErrorActionPreference = 'Stop'

$launcherRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $launcherRoot
if (-not $StageRoot) { $StageRoot = Join-Path $repoRoot 'build\source2-release' }
if (-not $FxRoot) { $FxRoot = Join-Path $repoRoot 'FX\compiled\source_mvm_fx' }
if (-not $DlssDir) { $DlssDir = Join-Path $repoRoot 'dlss 5' }
$version = (Get-Content -LiteralPath (Join-Path $launcherRoot 'package.json') -Raw | ConvertFrom-Json).version

function Assert-ChildPath([string]$candidate, [string]$parent) {
    $full = [IO.Path]::GetFullPath($candidate)
    $base = [IO.Path]::GetFullPath($parent).TrimEnd('\')
    if (-not $full.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe bundle cleanup target: $full"
    }
}

function Get-TextStamp([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-', '').ToLowerInvariant().Substring(0, 16)
}
# Relative path, size and write time of every file under each root: changes when any file does.
function Get-TreeStamp([string[]]$roots) {
    $lines = foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $full = (Resolve-Path -LiteralPath $root).Path
        Get-ChildItem -LiteralPath $full -Recurse -File | Sort-Object FullName |
            ForEach-Object { "$($_.FullName.Substring($full.Length))|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
    }
    Get-TextStamp ($lines -join "`n")
}

$bundle = Join-Path $launcherRoot 'bundle'
# Outputs worth keeping between builds (the FX archive takes the longest to make); git-ignored.
$cacheDir = Join-Path $repoRoot 'build\bundle-cache'
Assert-ChildPath $bundle $launcherRoot
if (Test-Path -LiteralPath $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
New-Item -ItemType Directory -Path $bundle -Force | Out-Null

# ---- runtime: the same files the release payload ships ----------------------------------------
$binRoot = Join-Path $StageRoot 'bin'
$required = @(
    'x64\AfxHookSource2.dll', 'x64\injector.exe', 'x64\AfxHook.dat',
    'x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe',
    'x64\FilmmakerDemoRepair\FilmmakerDemoRepair.exe',
    'x64\FilmmakerDemoKeyframes\FilmmakerDemoKeyframes.exe'
)
$missing = $required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $binRoot $_)) }
if ($missing) { throw "The staged build at $StageRoot is missing:`n  $($missing -join "`n  ")`nBuild the hook first." }

$runtime = Join-Path $bundle 'runtime'
New-Item -ItemType Directory -Path (Join-Path $runtime 'x64') -Force | Out-Null
# Copy files explicitly, filtering development and unfinished Deadlock artifacts before staging.
$x64Root = (Resolve-Path -LiteralPath (Join-Path $binRoot 'x64')).Path
Get-ChildItem -LiteralPath $x64Root -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($x64Root.Length + 1)
    if ($relative -match '(?i)deadlock' -or $_.Extension -in '.pdb', '.lib', '.exp', '.i64', '.id0', '.id1', '.id2', '.nam', '.til' -or
        $_.Name -ieq 'ffmpeg.ini' -or $_.Name -like 'AfxHookSource2.*.dll') { return }
    $target = Join-Path (Join-Path $runtime 'x64') $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
# osu\ (bundled songs + skin, for players without osu!) and easter-eggs\ (UwU sounds) sit beside
# the hook where GetHlaeFolder() points; without them a launcher install has no osu! maps at all.
foreach ($extra in @('resources', 'osu', 'easter-eggs')) {
    $source = Join-Path $binRoot $extra
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $runtime -Recurse -Force }
}
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'osu\Songs'))) {
    throw "The staged build at $StageRoot has no bin\osu\Songs (run Automation\tools\stage-easter-egg-media.ps1)."
}
# No symbols, import libraries or IDA databases (a .i64 beside the DLL is 150 MB); never a developer's ffmpeg.ini (the launcher writes one); never
# a renamed-aside hook DLL (AfxHookSource2.retired-*/.fixed/.running-backup).
Get-ChildItem -LiteralPath $runtime -Recurse -File |
    Where-Object { $_.Extension -in '.pdb', '.lib', '.exp', '.i64', '.id0', '.id1', '.id2', '.nam', '.til' -or $_.Name -ieq 'ffmpeg.ini' -or
                   $_.Name -like 'AfxHookSource2.*.dll' } |
    Remove-Item -Force
# resources\map-lighting is an old per-map cache nothing reads any more (the hook builds its map
# data under %APPDATA%\HLAE on first load).
$mapLighting = Join-Path $runtime 'resources\map-lighting'
Assert-ChildPath $mapLighting $bundle
if (Test-Path -LiteralPath $mapLighting) { Remove-Item -LiteralPath $mapLighting -Recurse -Force }
# environment-shaders: ship exactly the set the current build made, straight from the build tree.
$freshShaders = Join-Path $repoRoot 'build\x64-source2-release\AfxHookSource2\Release\environment-shaders'
if (Test-Path -LiteralPath $freshShaders) {
    $shaders = Join-Path $runtime 'x64\environment-shaders'
    Assert-ChildPath $shaders $bundle
    if (Test-Path -LiteralPath $shaders) { Remove-Item -LiteralPath $shaders -Recurse -Force }
    Copy-Item -LiteralPath $freshShaders -Destination $shaders -Recurse -Force
}

# ---- FX pack ------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath (Join-Path $FxRoot 'particles'))) { throw "No compiled FX pack at $FxRoot (no particles\)." }
$fx = Join-Path $bundle 'fx\source_mvm_fx'
New-Item -ItemType Directory -Path $fx -Force | Out-Null
# Everything CS2 reads through its file system goes into ONE archive: the compiled pack plus the
# prebuilt render materials (Green Screen sky) that build-render-materials.ps1 stages beside it.
# Packed only when those inputs changed since the last build; otherwise the cached archive is
# reused as is (same bytes, same timestamp, so the fx component keeps its install stamp).
$prebuiltRoot = Join-Path $repoRoot 'FX\render-materials\prebuilt'
$packTool = Join-Path $repoRoot 'FX\tools\pack-fx-vpk.py'
$fxInputStamp = Get-TextStamp ((Get-TreeStamp $FxRoot, $prebuiltRoot) + '|' + (Get-Item -LiteralPath $packTool).LastWriteTimeUtc.Ticks)
$cachedVpk = Join-Path $cacheDir 'source_mvm_fx.vpk'
$cachedStamp = Join-Path $cacheDir 'source_mvm_fx.vpk.stamp'
$haveCached = (Test-Path -LiteralPath $cachedVpk) -and (Test-Path -LiteralPath $cachedStamp) -and
    ((Get-Content -LiteralPath $cachedStamp -Raw).Trim() -eq $fxInputStamp)
$packInput = Join-Path $env:TEMP ('sourcemvm-fx-pack-' + [guid]::NewGuid().ToString('N'))
if ($haveCached) {
    Write-Host 'FX pack unchanged since the last build: reusing the cached archive.' -ForegroundColor Green
    Copy-Item -LiteralPath $cachedVpk -Destination (Join-Path $fx 'source_mvm_fx.vpk') -Force
} else { try {
    Copy-Item -LiteralPath $FxRoot -Destination $packInput -Recurse -Force
    if (Test-Path -LiteralPath $prebuiltRoot) { Copy-Item -Path (Join-Path $prebuiltRoot '*') -Destination $packInput -Recurse -Force }
    # Any Python 3 runs the packer (standard library only): SOURCEMVM_PYTHON, else the py
    # launcher, else python on PATH (not every machine has the py launcher).
    $python = @($env:SOURCEMVM_PYTHON) + @((Get-Command py, python -ErrorAction SilentlyContinue).Source) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $python) { throw 'Python 3 is needed to pack the FX archive (set SOURCEMVM_PYTHON to python.exe).' }
    $pyArgs = if ((Split-Path -Leaf $python) -ieq 'py.exe') { @('-3') } else { @() }
    & $python @pyArgs $packTool --input $packInput --output (Join-Path $fx 'source_mvm_fx.vpk')
    if ($LASTEXITCODE -ne 0) { throw 'pack-fx-vpk.py failed.' }
    New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $fx 'source_mvm_fx.vpk') -Destination $cachedVpk -Force
    Set-Content -LiteralPath $cachedStamp -Value $fxInputStamp -Encoding ASCII
} finally {
    Assert-ChildPath $packInput $env:TEMP
    Remove-Item -LiteralPath $packInput -Recurse -Force -ErrorAction SilentlyContinue
} }
Copy-Item -LiteralPath (Join-Path $FxRoot 'gameinfo.gi') -Destination $fx -Force
# The SOURCE:MVM intro. main.cpp mounts ui_override\ at the HEAD of the search path so CS2's own
# intro player loads it by Valve's file name; build-source2.bat step 8 encodes it into the stage.
$stagedPack = Join-Path $StageRoot 'fx\source_mvm_fx'
$intro = Join-Path $stagedPack 'ui_override\panorama\videos\intro.webm'
if (-not (Test-Path -LiteralPath $intro)) {
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'Automation\tools\set-startup-video.ps1') -Install -MountDir $stagedPack
    if (-not (Test-Path -LiteralPath $intro)) { throw "No intro video at $intro (set-startup-video.ps1 -Install failed)." }
}
$introDir = Join-Path $fx 'ui_override\panorama\videos'
New-Item -ItemType Directory -Path $introDir -Force | Out-Null
Copy-Item -LiteralPath $intro -Destination $introDir -Force
# Easter-egg clips (the Sony Vegas intervention) live beside the intro in the same subtree.
& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'Automation\tools\stage-easter-egg-media.ps1') -MountDir $stagedPack
if ($LASTEXITCODE -ne 0) { throw 'stage-easter-egg-media.ps1 failed.' }
Get-ChildItem -LiteralPath (Join-Path $stagedPack 'ui_override\panorama\videos') -Filter 'mvm_egg_*.webm' |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $introDir -Force }

# ---- DLSS 5 -------------------------------------------------------------------------------------
$dlss = Join-Path $bundle 'dlss5'
foreach ($sub in 'streamline', 'NVIDIA original', 'RTX 40, 30, 20 compatible') {
    $from = Join-Path $DlssDir $sub
    if (-not (Test-Path -LiteralPath $from)) { throw "The DLSS 5 folder is missing $sub ($from)." }
    Copy-Item -LiteralPath $from -Destination (Join-Path $dlss $sub) -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $dlss 'addons') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $DlssDir 'addons\renodx-dlss.addon64') -Destination (Join-Path $dlss 'addons') -Force

# ReShade with add-on support: the DLL itself, or read out of the official setup exe.
$reshadeDir = Join-Path $DlssDir 'reshade'
$reshadeDll = Join-Path $reshadeDir 'ReShade64.dll'
$setup = Get-ChildItem -LiteralPath $reshadeDir -Filter 'ReShade_Setup_*_Addon.exe' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (Test-Path -LiteralPath $reshadeDll) {
    Copy-Item -LiteralPath $reshadeDll -Destination (Join-Path $dlss 'ReShade64.dll') -Force
} elseif ($setup) {
    $out = Join-Path $dlss 'ReShade64.dll'
    $script = "const t=require(process.argv[1]); require('fs').writeFileSync(process.argv[3], t.extractReShadeFromSetup(process.argv[2]));"
    & node -e $script (Join-Path $launcherRoot 'electron\dlss-tool.cjs') $setup.FullName $out
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) { throw "Could not read ReShade64.dll out of $($setup.Name)." }
} else {
    throw "No ReShade found. Put ReShade64.dll or the official ReShade_Setup_*_Addon.exe in $reshadeDir."
}
$license = Join-Path $reshadeDir 'ReShade-LICENSE.md'
if (-not (Test-Path -LiteralPath $license)) { throw "ReShade's license must ship with it: $license is missing." }
Copy-Item -LiteralPath $license -Destination (Join-Path $dlss 'ReShade-LICENSE.md') -Force

# ---- FFmpeg: runtime dependency, never Python/Node/build tools on a user's PC ---------------------
& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stage-bundled-ffmpeg.ps1') -Destination (Join-Path $runtime 'ffmpeg')
if ($LASTEXITCODE -ne 0) { throw 'Staging bundled FFmpeg failed.' }
$unfinished = Get-ChildItem -LiteralPath $bundle -Recurse -File | Where-Object { $_.FullName.Substring($bundle.Length) -match '(?i)deadlock' }
if ($unfinished) { throw 'The complete installer must not contain unfinished Deadlock files.' }

# ---- path length: the NSIS uninstaller an upgrade runs cannot move files past 260 characters ------
& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shorten-bundle-paths.ps1') -Root $bundle
if ($LASTEXITCODE -ne 0) { throw 'Bundled paths are too long for the installer.' }

# ---- stamp --------------------------------------------------------------------------------------
# Changes whenever any bundled file changes, so the launcher re-copies exactly when it must.
# One stamp PER COMPONENT, so a launcher update re-installs only what changed: a new hook DLL
# does not re-copy the FX pack. (Copies keep their source timestamps, and the FX archive comes
# from the pack cache when its inputs did not change, so an unchanged component keeps its stamp.)
$runtimeStamp = Get-TreeStamp (Join-Path $bundle 'runtime')
$fxStamp = Get-TreeStamp (Join-Path $bundle 'fx')
$stamp = Get-TextStamp "$runtimeStamp|$fxStamp"
[ordered]@{
    version = $version; stamp = $stamp; builtAt = (Get-Date).ToUniversalTime().ToString('o')
    components = [ordered]@{ runtime = $runtimeStamp; fx = $fxStamp }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bundle 'bundle.json') -Encoding ASCII

$size = (Get-ChildItem -LiteralPath $bundle -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Bundle staged: {0:N0} MB, stamp {1}" -f ($size / 1MB), $stamp) -ForegroundColor Green
