@echo off
setlocal
REM Direct CS2 launch with NetCon (29010 by default) and readiness artifacts.
set "MVM_PREV_CP="
for /f "tokens=2 delims=:" %%c in ('chcp 2^>nul') do set "MVM_PREV_CP=%%c"
if defined MVM_PREV_CP set "MVM_PREV_CP=%MVM_PREV_CP: =%"
chcp 1252 >nul 2>&1
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Automation\launch\launch-cs2-netcon.ps1" %*
set "MVM_EXIT=%ERRORLEVEL%"
if defined MVM_PREV_CP chcp %MVM_PREV_CP% >nul 2>&1
exit /b %MVM_EXIT%
