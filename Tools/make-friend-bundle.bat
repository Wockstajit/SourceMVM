@echo off
rem Assembles the shareable SOURCE:MVM zip (portable launcher + pre-installed runtime & FX).
rem Prerequisites (this script does not build anything):
rem   SourceMovieMakerLauncher\build-app.bat -Package
rem   SourceMovieMakerLauncher\scripts\build-release-payload.ps1
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Import-Module Microsoft.PowerShell.Utility; & '%~dp0make-friend-bundle.ps1' %*"
exit /b %errorlevel%
