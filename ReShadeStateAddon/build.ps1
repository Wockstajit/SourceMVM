# Builds mvm_reshade_state.addon64 (standalone -- no dependency on the main build).
# Usage: pwsh ReShadeStateAddon\build.ps1 [-Deploy]
#   -Deploy also copies the addon beside ReShade in the CS2 install (close CS2 first).
param([switch]$Deploy)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - is Visual Studio installed?' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with C++ tools found.' }

$out = 'mvm_reshade_state.addon64'
cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -no_logo && cl /nologo /std:c++17 /O2 /EHsc /LD mvm_reshade_state.cpp /Fe:$out"
if ($LASTEXITCODE -ne 0) { throw "cl failed with exit $LASTEXITCODE" }
Get-Item $out | Select-Object Name, Length, LastWriteTime

if ($Deploy) {
    $cs2 = & (Join-Path $PSScriptRoot '..\Automation\lib\Resolve-Cs2Dir.ps1')
    if (-not $cs2) { throw 'CS2 install not found (set SOURCEMVM_CS2_DIR).' }
    $bin = Join-Path $cs2 'game\bin\win64'
    Copy-Item $out (Join-Path $bin $out) -Force
    Write-Host "deployed to $bin"
}
