#requires -Version 5
<#
============================================================
  Filmmaker "Demos" tab - screenshot/verification harness
============================================================
  Preflights the staging build, copies the console-alias cfg
  into the CS2 cfg dir, launches SOURCE:MVM, and prints the exact
  in-game console checklist used to drive + screenshot the
  Filmmaker tab.

  Navigation is driven by `mirv_filmmaker` console commands
  (which call Panorama RunScript internally) - NOT by
  pixel-clicking the Panorama UI.

  Usage:
    powershell -ExecutionPolicy Bypass -File .\Automation\verify\test-filmmaker.ps1
    ... -Cs2Dir "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive"
    ... -NoLaunch          (preflight + copy cfg only, don't start SOURCE:MVM)
============================================================
#>
[CmdletBinding()]
param(
    [string]$Cs2Dir = '',
    [switch]$NoLaunch
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$stagingCandidates = @(
    (Join-Path $root 'build\source2-release\bin'),
    (Join-Path $root 'build\staging-release\bin')
)
$staging = $stagingCandidates | Where-Object {
    Test-Path -LiteralPath (Join-Path $_ 'x64\AfxHookSource2.dll')
} | Select-Object -First 1
if (-not $staging) { $staging = $stagingCandidates[0] }
$dll     = Join-Path $staging 'x64\AfxHookSource2.dll'
$helper  = Join-Path $staging 'x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe'
$launcherCandidates = @(
    (Join-Path $root 'SourceMovieMakerLauncher\release\win-unpacked\SourceMovieMaker.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher\release\SourceMovieMaker.exe'),
    (Join-Path $staging 'SourceMovieMaker.exe')
)
$launcher = $launcherCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $launcher) {
    $launcher = Join-Path $staging 'HLAE.exe'
}
$cfgSrc  = Join-Path $automationRoot 'config\filmmaker_test.cfg'

function Ok($m)   { Write-Host "[ OK ] $m"   -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m"   -ForegroundColor Yellow }
function Err($m)  { Write-Host "[FAIL] $m"   -ForegroundColor Red }

Write-Host "=== Filmmaker test harness ===" -ForegroundColor Cyan

# --- Preflight -------------------------------------------------------------
$fatal = $false

if (Test-Path $dll) {
    $age = (Get-Date) - (Get-Item $dll).LastWriteTime
    Ok ("AfxHookSource2.dll present (built {0:N1}h ago)" -f $age.TotalHours)
} else {
    Err "AfxHookSource2.dll NOT found at $dll  -> run build.bat first."
    $fatal = $true
}

if (Test-Path $helper) {
    Ok "FilmmakerDemoInfo.exe present (real player names available)."
} else {
    Warn "FilmmakerDemoInfo.exe NOT found -> demo player names will show [unknown]."
    Warn "  expected at: $helper"
}

if (Test-Path $launcher) { Ok "$(Split-Path $launcher -Leaf) present." }
else {
    Err "Launcher NOT found. Build SourceMovieMakerLauncher\build-app.bat -Package first."
    $fatal = $true
}

# Is CS2 already running? (staging DLL is locked while it is, and a rebuild would fail)
$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if ($cs2) { Warn "cs2.exe is already running (pid $($cs2.Id)). Close it before rebuilding." }

# --- Copy the console-alias cfg into the CS2 cfg dir -----------------------
if (Test-Path $cfgSrc) {
    $cfgDir = Join-Path $Cs2Dir 'game\csgo\cfg'
    if (Test-Path $cfgDir) {
        Copy-Item $cfgSrc (Join-Path $cfgDir 'filmmaker_test.cfg') -Force
        Ok "Copied filmmaker_test.cfg -> $cfgDir"
        Write-Host "      Add  +exec filmmaker_test  to SOURCE:MVM launch options to load the aliases." -ForegroundColor DarkGray
    } else {
        Warn "CS2 cfg dir not found: $cfgDir  (pass -Cs2Dir to point at your install)"
    }
} else {
    Warn "Automation\filmmaker_test.cfg missing (aliases skipped)."
}

if ($fatal) { Err "Preflight failed. Fix the above and re-run."; exit 1 }

# --- In-game console checklist ---------------------------------------------
$checklist = @'

------------------------------------------------------------
 IN-GAME CONSOLE CHECKLIST (open console with `~` / backtick)
------------------------------------------------------------
 1) mirv_filmmaker ui_status     # Panorama bridge resolved? (engine + RunScript + context)
 2) mirv_filmmaker scan          # rescan CS2 install + saved folders for demos
 3) mirv_filmmaker list          # demos found? real names vs [unknown]?
 4) mirv_filmmaker ui            # open Watch -> Downloaded (programmatic nav)
    -> close console, SCREENSHOT the Downloaded view.
 5) click native "Your Matches"  # reproduce the regression, SCREENSHOT (expected blank).

 (If you exec'd filmmaker_test.cfg, aliases exist: fm_status / fm_scan / fm_list / fm_ui / fm_rebuild)
------------------------------------------------------------
'@
Write-Host $checklist -ForegroundColor Cyan

# --- Launch ----------------------------------------------------------------
if ($NoLaunch) { Ok "-NoLaunch set; not starting SOURCE:MVM."; exit 0 }

Ok "Starting SOURCE:MVM ($(Split-Path $launcher -Leaf))..."
Write-Host "      Launch CS2 from SOURCE:MVM (windowed + -console recommended)." -ForegroundColor DarkGray
Start-Process -FilePath $launcher




