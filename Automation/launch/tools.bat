@echo off
setlocal
REM Launches CS2 in WORKSHOP TOOLS mode (-tools -dx11) with the SOURCE:MVM hook + netcon.
REM Separate from demo.bat/test.bat on purpose: different preflight, different command line.
REM
REM Requires the "Counter-Strike 2 Workshop Tools" DLC content to be installed via Steam.

REM The injector fails on a UTF-8 (65001) console: the redirected command line picks up a BOM.
REM Force the legacy codepage before launching, exactly like the other agent-facing entry points.
chcp 1252 >nul

cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "launch-cs2-tools.ps1" %*
if errorlevel 1 (
    echo.
    echo ERROR: launch-cs2-tools failed.
    exit /b 1
)
