@echo off
setlocal
REM ============================================================
REM  CS2 LIVE - one click
REM  Always: restarts CS2 with netcon at 1920x1080 windowed.
REM  Optional (asked, 3s auto-No): the browser dashboard - live
REM  marker count, speed mode, segment progress, console stream
REM  and drive buttons.
REM
REM  Usage:
REM    live.bat                 (CS2 1920x1080, netcon 29010, web 8765)
REM    live.bat "F:\path\to\demo.dem"
REM  A supplied demo is always loaded, even when the dashboard is skipped.
REM  The newest hook build of any checkout of this repository launches
REM  (-NewestHook), so a hook built in a worktree is the one that starts.
REM
REM  *** WHY THE DASHBOARD IS OPT-IN ***
REM  The dashboard holds a netcon console connection open for the whole
REM  session. CS2 writes console output to netcon SYNCHRONOUSLY, so a
REM  slow reader makes the engine hitch on every console print. A live
REM  map prints constantly, so it shows up as heavy in-game stutter.
REM  Answering No still leaves the netcon PORT open with nothing attached:
REM  no stutter, but a tool can still connect on demand when needed.
REM  For LIVE-MAP testing (Insert menu, Smooth View, FX) use test.bat.
REM ============================================================

cd /d "%~dp0"

set "PORT=29010"
set "WEBPORT=8765"
set "WIDTH=1920"
set "HEIGHT=1080"
set "DEMO=%~1"
set "DASH=0"

if not exist "cs2-live.ps1" (
    echo cs2-live.ps1 not found ^(expected Automation\launch\cs2-live.ps1^).
    pause
    exit /b 1
)

REM --- ask up front, then run unattended -----------------------
choice /c YN /t 3 /d N /m "Open the live netcon dashboard (browser viewer)"
if not errorlevel 2 set "DASH=1"
if "%DASH%"=="0" (
    echo Dashboard skipped - CS2 will start with netcon port %PORT% open and no reader attached.
)
echo.

REM --- stop any stale dashboard server on the web port ----------
REM Always: a leftover dashboard from an earlier run is an attached reader (stutter).
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$url = 'http://localhost:%WEBPORT%/stop'; try { Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 2 | Out-Null; Write-Host 'Stopped an existing dashboard.'; Start-Sleep -Milliseconds 750 } catch { }"

REM --- relaunch CS2/netcon fresh (closes any existing cs2.exe) --
if defined DEMO (
    echo Launching CS2 and loading demo "%DEMO%" ...
    powershell -NoProfile -ExecutionPolicy Bypass -File "launch-demo.ps1" -Demo "%DEMO%" -Port %PORT% -Width %WIDTH% -Height %HEIGHT% -NewestHook
) else (
    echo Launching CS2 with netcon at %WIDTH%x%HEIGHT% on port %PORT% ...
    powershell -NoProfile -ExecutionPolicy Bypass -File "launch-cs2-netcon.ps1" -Port %PORT% -Width %WIDTH% -Height %HEIGHT% -NewestHook
)
if errorlevel 1 (
    echo.
    echo ERROR: failed to launch CS2 or load the demo.
    pause
    exit /b 1
)

if "%DASH%"=="0" (
    echo.
    echo CS2 is running. Netcon port %PORT% is open, nothing attached.
    echo Open the dashboard later with: Automation\launch\cs2-live.ps1 -Port %PORT% -WebPort %WEBPORT%
    exit /b 0
)

REM --- open the live dashboard (auto-retries until netcon is up)
powershell -NoProfile -ExecutionPolicy Bypass -File "cs2-live.ps1" -Port %PORT% -WebPort %WEBPORT%
if errorlevel 1 (
    echo.
    echo ERROR: live dashboard stopped with an error.
    pause
    exit /b 1
)

echo.
echo Dashboard server stopped.
exit /b 0
