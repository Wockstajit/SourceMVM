@echo off
REM ============================================================
REM  SOURCE:MVM launch script
REM  Runs the Source 2 staging build produced by build.bat
REM ============================================================

setlocal
cd /d "%~dp0"

REM The injector reads its arguments as ANSI. A UTF-8 (65001) console hands it a
REM BOM and the injection fails, so pin the legacy codepage before launching.
chcp 1252 >nul

set "EXE=SourceMovieMakerLauncher\release\win-unpacked\SourceMovieMaker.exe"

if not exist "%EXE%" (
    echo SourceMovieMaker.exe not found at "%EXE%".
    echo Build the launcher first with SourceMovieMakerLauncher\build-app.bat -Package.
    pause
    exit /b 1
)

REM Window placement is opt-in. 0 means "leave it on the primary display", which is
REM where Windows already puts a new window, so the default does no work at all.
REM Set SOURCEMVM_LAUNCH_MONITOR=3 to target \\.\DISPLAY3 on a multi-monitor desk.
set "MVM_MONITOR=%SOURCEMVM_LAUNCH_MONITOR%"
if not defined MVM_MONITOR set "MVM_MONITOR=0"

start "" "%EXE%"

if "%MVM_MONITOR%"=="0" goto :done

set "MOVER=%~dp0Automation\tools\move-process-window-to-monitor.ps1"
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%MOVER%" -ProcessName SourceMovieMaker -Monitor %MVM_MONITOR% -TimeoutSeconds 20
if errorlevel 1 echo WARNING: SOURCE:MVM launcher could not be moved to monitor %MVM_MONITOR%. Continuing launch.

REM The launcher starts CS2 later, so one helper waits for the game window. Capped at
REM 300s: long enough to cover browsing demos first, short enough not to leave a
REM hidden PowerShell sitting around for a quarter of an hour.
start "" powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "%MOVER%" -ProcessName cs2 -Monitor %MVM_MONITOR% -TimeoutSeconds 300

:done
endlocal
exit /b 0
