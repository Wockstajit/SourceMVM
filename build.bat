@echo off
REM ============================================================
REM  SOURCE:MVM default build entry point.
REM  Delegates to the CS2 / Source 2-only stack (not legacy HLAE).
REM  For the old Win32+x64 HLAE package, run build-hlae.bat.
REM ============================================================
cd /d "%~dp0"
call "%~dp0build-source2.bat" %*
exit /b %ERRORLEVEL%
