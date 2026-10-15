@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo  SOURCE:MVM - Local DLSS 5 Installer
echo ============================================================
echo.
echo This installs the files from the local "dlss 5" folder into
echo a private SOURCE:MVM CS2 binary directory. It does not download
echo anything, and normal Steam launches will not load ReShade.
echo.

choice /C YN /N /M "Do you have an older NVIDIA GPU (RTX 20, 30, or 40 series)? [Y/N]: "
if errorlevel 2 goto current_gpu
if errorlevel 1 goto older_gpu

:older_gpu
set "DLSS_VARIANT=RTX 40, 30, 20 compatible"
goto install

:current_gpu
set "DLSS_VARIANT=NVIDIA original"

:install
echo.
echo Selected: %DLSS_VARIANT%
echo Starting local installation...
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Automation\dlss\install-isolated.ps1" -RuntimeVariant "%DLSS_VARIANT%"
set "INSTALL_EXIT=%ERRORLEVEL%"
echo.
if not "%INSTALL_EXIT%"=="0" (
    echo DLSS 5 installation failed. Review the error above.
    echo No internet download was attempted.
    pause
    exit /b %INSTALL_EXIT%
)

echo DLSS 5 installation completed successfully.
echo Use SOURCE:MVM's isolated DLSS launcher; normal Steam CS2 will not load ReShade.
pause
exit /b 0
