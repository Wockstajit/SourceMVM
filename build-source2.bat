@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  SOURCE:MVM Source 2 / CS2 build
REM  Builds AfxHookSource2 + helpers/FX
REM  into build\source2-release. Does NOT build HLAE / Source 1.
REM ============================================================

cd /d "%~dp0"

set "NoDefaultCurrentDirectoryInExePath="
set "PATH=%USERPROFILE%\.cargo\bin;%LOCALAPPDATA%\Programs\gettext-iconv\bin;%ProgramFiles%\Go\bin;%PATH%"

set "FM_STAGE_ROOT=%~dp0build\source2-release"
REM The compiled FX pack is committed (git LFS), so a clone needs no Workshop Tools. Rebuilding it
REM (prompt below) needs resourcecompiler.exe and republishes into this folder.
set "FM_FX_PACK_DIR=%~dp0FX\compiled\source_mvm_fx"
set "FM_FX_STAGE_DIR=%FM_STAGE_ROOT%\fx\source_mvm_fx"
set "FM_FX_REBUILD=0"
set "FM_LAUNCH=1"

echo === SOURCE:MVM Source 2 build ===
if /I "%~1"=="-NoLaunch" set "FM_LAUNCH=0"
if /I "%~1"=="--no-launch" set "FM_LAUNCH=0"

if not exist "%FM_FX_PACK_DIR%\particles" echo   note: FX\compiled is not here ^(third-party material, kept out of the repository: FX\README.md^); launch uses vanilla particles.
choice /c YN /t 3 /d N /m "  Rebuild particle FX packs (needs CS2 Workshop Tools)"
if not errorlevel 2 set "FM_FX_REBUILD=1"
echo.

echo [1/8] Closing CS2 / SOURCE:MVM / HLAE so staged DLLs are not locked...
taskkill /f /im cs2.exe >nul 2>nul
taskkill /f /im SourceMovieMaker.exe >nul 2>nul
taskkill /f /im hlae.exe >nul 2>nul
ping -n 3 127.0.0.1 >nul

if "!FM_FX_REBUILD!"=="1" (
    echo [2/8] Rebuilding particle FX packs...
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0FX\tools\convert-fx-source1.ps1" -Compile
    if errorlevel 1 (
        echo   WARNING: FX conversion FAILED. Continuing with the local FX\compiled pack.
        echo   Scroll up for the failing step. 8s to Ctrl+C...
        timeout /t 8 >nul
    )
) else echo [2/8] Particle FX packs: keeping existing.

echo [3/8] (no asset build step)

echo [4/8] Building Source 2 stack ^(x64 AfxHookSource2 -^> source2-release^)...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found - Visual Studio 2022 is not installed. Needs the
    echo "Desktop development with C++" and ".NET desktop development" workloads plus
    echo the .NET Framework 4.6.2 targeting pack.
    pause
    exit /b 1
)
set "VSINSTALL="
"%VSWHERE%" -latest -version "[17.0,18.0)" -property installationPath > "%TEMP%\_mvm_vsinstall.txt"
set /p VSINSTALL=<"%TEMP%\_mvm_vsinstall.txt"
del "%TEMP%\_mvm_vsinstall.txt" 2>nul
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
cmake -P CMake/Source2Build.cmake
if errorlevel 1 (
    echo.
    echo ERROR: Source 2 build failed. See messages above.
    pause
    exit /b 1
)

echo [5/8] Building demo helpers ^(info + legacy repair + scrub keyframes^)...
where go >nul 2>nul
if errorlevel 1 (
    echo   WARNING: 'go' not on PATH - demo helpers NOT rebuilt. Install Go ^(https://go.dev/dl/^).
) else (
    set "FM_HELPER_DIR=%FM_STAGE_ROOT%\bin\x64\FilmmakerDemoInfo"
    if exist "!FM_HELPER_DIR!" rmdir /s /q "!FM_HELPER_DIR!"
    mkdir "!FM_HELPER_DIR!"
    pushd "%~dp0FilmmakerDemoInfoGo"
    go build -o "!FM_HELPER_DIR!\FilmmakerDemoInfo.exe" .
    if errorlevel 1 echo   WARNING: helper failed to build - demo names/sides/MVPs unavailable.
    popd

    set "FM_REPAIR_DIR=%FM_STAGE_ROOT%\bin\x64\FilmmakerDemoRepair"
    if exist "!FM_REPAIR_DIR!" rmdir /s /q "!FM_REPAIR_DIR!"
    mkdir "!FM_REPAIR_DIR!"
    pushd "%~dp0FilmmakerDemoRepairGo"
    go build -o "!FM_REPAIR_DIR!\FilmmakerDemoRepair.exe" .
    if errorlevel 1 echo   WARNING: repair helper failed to build - affected legacy demos play unrepaired.
    popd

    set "FM_KEYFRAMES_DIR=%FM_STAGE_ROOT%\bin\x64\FilmmakerDemoKeyframes"
    if exist "!FM_KEYFRAMES_DIR!" rmdir /s /q "!FM_KEYFRAMES_DIR!"
    mkdir "!FM_KEYFRAMES_DIR!"
    pushd "%~dp0FilmmakerDemoKeyframesGo"
    go build -o "!FM_KEYFRAMES_DIR!\FilmmakerDemoKeyframes.exe" .
    if errorlevel 1 echo   WARNING: keyframes helper failed to build - backward seeks stay slow.
    popd
)

echo [6/8] Staging compiled FX pack into the Source 2 release folder...
if exist "%FM_FX_PACK_DIR%\particles" (
    robocopy "%FM_FX_PACK_DIR%" "%FM_FX_STAGE_DIR%" /MIR /XD "%FM_FX_STAGE_DIR%\ui_override" "%FM_FX_STAGE_DIR%\cfg" /NFL /NDL /NJH /NJS /NP /R:1 /W:1 >nul
    if errorlevel 8 echo   WARNING: staging failed - the shipped build may launch without custom particles.
) else echo   No compiled FX pack on disk to stage ^(skipped^).

echo [7/8] Building uniform render-pass materials...
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0FX\tools\build-render-materials.ps1" -StageRoot "%FM_FX_STAGE_DIR%"
if errorlevel 1 (
    echo ERROR: render-pass material build failed.
    exit /b 1
)

echo [8/8] Startup video override ^(SOURCE:MVM intro^)...
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Automation\tools\set-startup-video.ps1" -Install -MountDir "%FM_FX_STAGE_DIR%"
if errorlevel 1 echo   WARNING: startup video not installed; CS2 will play its stock intro.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Automation\tools\stage-easter-egg-media.ps1" -MountDir "%FM_FX_STAGE_DIR%"
if errorlevel 1 echo   WARNING: easter-egg media not staged.

echo.
echo === BUILD OK ===  %FM_STAGE_ROOT%
echo Build the standalone launcher separately with SourceMovieMakerLauncher\build-app.bat -Package
echo.
if "!FM_LAUNCH!"=="1" (
    call "%~dp0Automation\launch\live.bat"
    exit /b %ERRORLEVEL%
)
exit /b 0
