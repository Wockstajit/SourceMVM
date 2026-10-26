@echo off
setlocal
rem ---------------------------------------------------------------------------
rem  SOURCE:MVM -- Counter-Strike: Source UI mockup
rem
rem  Double-click to run the hook's UI C++ in the browser. Saving a UI source
rem  file rebuilds the WebAssembly and reloads the page.
rem
rem  Needs Node.js and Emscripten (emsdk in %USERPROFILE%\emsdk, or set
rem  SOURCEMVM_EMSDK). Optional first argument overrides the port:
rem  css-mockup.bat 9000
rem ---------------------------------------------------------------------------

pushd "%~dp0"
title SOURCE:MVM CS:S UI mockup

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8825"

where node >nul 2>nul
if errorlevel 1 (
  echo  [X] Node.js was not found on PATH.
  echo      Install it from https://nodejs.org and run this again.
  echo.
  pause & popd & exit /b 1
)

call node ..\Automation\ui\preview.mjs css --port %PORT% --open
if errorlevel 1 pause

popd
endlocal
