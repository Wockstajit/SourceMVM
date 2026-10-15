@echo off
setlocal
REM ============================================================
REM  Legacy full HLAE multi-arch package build (Win32 GUI + x64
REM  Source 1 + Source 2). Prefer build-source2.bat / build.bat for
REM  day-to-day SOURCE:MVM CS2 work.
REM ============================================================

cd /d "%~dp0"

set "NoDefaultCurrentDirectoryInExePath="
set "PATH=%USERPROFILE%\.cargo\bin;%LOCALAPPDATA%\Programs\gettext-iconv\bin;%ProgramFiles%\Go\bin;%PATH%"

echo === Legacy HLAE multi-build ===
echo This builds HLAE.exe, GoldSrc, Source 1, and Source 2.
echo For CS2-only work use build.bat or build-source2.bat instead.
echo.

echo Closing CS2 / HLAE so staged DLLs are not locked...
taskkill /f /im cs2.exe >nul 2>nul
taskkill /f /im hlae.exe >nul 2>nul
taskkill /f /im SourceMovieMaker.exe >nul 2>nul
ping -n 3 127.0.0.1 >nul

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    pause
    exit /b 1
)
set "VSINSTALL="
"%VSWHERE%" -latest -version "[17.0,18.0)" -property installationPath > "%TEMP%\_hlae_vsinstall.txt"
set /p VSINSTALL=<"%TEMP%\_hlae_vsinstall.txt"
del "%TEMP%\_hlae_vsinstall.txt" 2>nul
if not defined VSINSTALL (
    echo ERROR: Visual Studio 2022 ^(17.x^) not found.
    pause
    exit /b 1
)
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 (
    echo ERROR: failed to initialize the VS developer environment.
    pause
    exit /b 1
)

set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
cmake -DAFX_MULTIBUILD_STAGING=ON -P CMake/MultiBuild.cmake
if errorlevel 1 (
    echo.
    echo ERROR: legacy HLAE build failed.
    pause
    exit /b 1
)

echo.
echo === LEGACY HLAE BUILD OK ===  %~dp0build\staging-release\bin\HLAE.exe
exit /b 0
