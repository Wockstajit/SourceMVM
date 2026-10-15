@echo off
setlocal
REM Non-interactive Deadlock launcher for people and agents.
REM
REM Force the legacy console codepage before launching. On a UTF-8 (65001) console .NET Framework
REM derives the redirected-stdin encoding from the console codepage, so SourceMovieMakerHost writes
REM a UTF-8 BOM ahead of the injector's serialization header; injector.exe then dies with an
REM unhandled SerializationException (0xe0434352), the host terminates the suspended game, and the
REM launch fails a minute later as "did not answer on port" -- which reads like a game or Steam
REM problem and is not one. Agent shells run at 65001, so this cannot be left to the caller.
REM Same guard as demo.bat and tools.bat; the caller's codepage is restored on the way out.
set "MVM_PREV_CP="
for /f "tokens=2 delims=:" %%c in ('chcp 2^>nul') do set "MVM_PREV_CP=%%c"
if defined MVM_PREV_CP set "MVM_PREV_CP=%MVM_PREV_CP: =%"
chcp 1252 >nul 2>&1

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0launch-deadlock.ps1" %*
set "MVM_EXIT=%ERRORLEVEL%"

if defined MVM_PREV_CP chcp %MVM_PREV_CP% >nul 2>&1
exit /b %MVM_EXIT%
