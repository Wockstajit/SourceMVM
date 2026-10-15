@echo off
REM Preserve demo arguments, NetCon readiness, session.json, and exit status.
call "%~dp0Automation\launch\demo.bat" %*
exit /b %ERRORLEVEL%
