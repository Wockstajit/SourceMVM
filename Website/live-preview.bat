@echo off
rem Live preview of the download page: opens it in your browser and updates it on every save.
rem CSS edits restyle in place; HTML, JS and asset edits reload. Close this window to stop.
setlocal
where node >nul 2>nul
if errorlevel 1 (
  echo Node.js is not on PATH. Install it from https://nodejs.org and run this again.
  pause
  exit /b 1
)
node "%~dp0tools\live-server.mjs" %*
if errorlevel 1 pause
