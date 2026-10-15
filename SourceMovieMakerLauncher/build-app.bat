@echo off
setlocal
pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-app.ps1" %*
set "exitCode=%ERRORLEVEL%"
popd
exit /b %exitCode%
