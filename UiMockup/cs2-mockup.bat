@echo off
setlocal EnableDelayedExpansion
rem ---------------------------------------------------------------------------
rem  SOURCE:MVM in-game UI mockup
rem
rem  Double-click to preview the real UI scripts. Source edits refresh on save.
rem  Pending generated-JS drafts are preserved until explicitly applied.
rem
rem  Optional first argument overrides the port:  cs2-mockup.bat 9000
rem ---------------------------------------------------------------------------

pushd "%~dp0"
title SOURCE:MVM UI mockup

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8823"

echo.
echo  SOURCE:MVM -- in-game UI mockup
echo  ==============================
echo.

rem ---- node is required: the extractors are Node scripts -----------------------
where node >nul 2>nul
if errorlevel 1 (
  echo  [X] Node.js was not found on PATH.
  echo      Install it from https://nodejs.org and run this again.
  echo.
  pause & popd & exit /b 1
)

rem ---- one-time: pull the real CS2 icons/backgrounds and the Stratum2 fonts ----
if not exist "assets\panorama\images\backgrounds\bluedots_large_png.png" (
  echo.
  echo  [+] First run: pulling CS2 UI assets and fonts ...
  call node fetch-assets.mjs
  if errorlevel 1 (
    echo.
    echo  [!] Asset extraction failed -- the mockup still runs, but icons and the
    echo      dotted panel background will be missing. Needs Source2Viewer-CLI and
    echo      a CS2 install; override with the SOURCEMVM_VRF_CLI and CS2_PAK env vars.
    echo.
  )
)

rem ---- Node serves locally and refreshes shared source changes on save --------
call node ..\Automation\ui\preview.mjs cs2 --port %PORT% --open
if errorlevel 1 pause

popd
endlocal
