@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\live-preview.ps1" %*
exit /b %errorlevel%
