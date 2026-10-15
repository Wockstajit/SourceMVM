@echo off
setlocal
REM One-click offline FX testing: CS2 + hook + bot practice + mvm_test.
REM In-game: press INSERT for the FX test menu.

cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "launch-cs2-test.ps1" %*
if errorlevel 1 (
    echo.
    echo ERROR: launch-cs2-test failed.
    pause
    exit /b 1
)
