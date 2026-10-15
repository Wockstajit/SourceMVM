@echo off
setlocal
REM Non-interactive demo launcher for people and agents.
REM With no argument, opens the newest demo in game\csgo\replays.

REM Force the legacy console codepage before launching. On a UTF-8 (65001) console .NET
REM Framework derives the redirected-stdin encoding from the console codepage, so the host
REM writes a UTF-8 BOM ahead of the injector's serialization header; injector.exe then dies
REM with an unhandled SerializationException (0xe0434352), the host terminates the suspended
REM cs2.exe, and the launch fails 75 seconds later as "CS2 did not expose a responsive window
REM and netcon port" -- which reads like a game/Steam problem and is not one. Codex and Claude
REM Code both run their shells at 65001, so this cannot be left to the caller. Same guard as
REM tools.bat; the caller's console codepage is restored on the way out.
set "MVM_PREV_CP="
for /f "tokens=2 delims=:" %%c in ('chcp 2^>nul') do set "MVM_PREV_CP=%%c"
if defined MVM_PREV_CP set "MVM_PREV_CP=%MVM_PREV_CP: =%"
chcp 1252 >nul 2>&1

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0launch-demo.ps1" %*
set "MVM_EXIT=%ERRORLEVEL%"

if defined MVM_PREV_CP chcp %MVM_PREV_CP% >nul 2>&1
exit /b %MVM_EXIT%
