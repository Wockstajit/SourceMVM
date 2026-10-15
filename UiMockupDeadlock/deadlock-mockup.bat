@echo off
setlocal EnableDelayedExpansion
rem ---------------------------------------------------------------------------
rem  SOURCE:MVM -- Deadlock in-game UI mockup
rem
rem  Double-click to preview the real UI scripts. Source edits refresh on save.
rem  Pending generated-JS drafts are preserved until explicitly applied.
rem
rem  Optional first argument overrides the port:  deadlock-mockup.bat 9000
rem ---------------------------------------------------------------------------

pushd "%~dp0"
title SOURCE:MVM Deadlock UI mockup

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8824"

echo.
echo  SOURCE:MVM -- Deadlock in-game UI mockup
echo  =======================================
echo.

rem ---- node is required: the extractor is a Node script ------------------------
where node >nul 2>nul
if errorlevel 1 (
  echo  [X] Node.js was not found on PATH.
  echo      Install it from https://nodejs.org and run this again.
  echo.
  pause & popd & exit /b 1
)

rem ---- one-time: Deadlock's own fonts and the native bar's icons ---------------
if not exist "fonts\retaildemo-regular.otf" (
  echo.
  echo  [+] First run: staging Deadlock's fonts and icons ...
  call node fetch-assets.mjs
  if errorlevel 1 (
    echo.
    echo  [!] Asset staging failed -- the mockup still runs, but it will draw in a
    echo      fallback typeface, which measures differently from the game. Needs a
    echo      Deadlock install; override with SOURCEMVM_DEADLOCK_DIR.
    echo.
  )
)

rem ---- Node serves locally and refreshes shared source changes on save --------
call node ..\Automation\ui\preview.mjs deadlock --port %PORT% --open
if errorlevel 1 pause

popd
endlocal
