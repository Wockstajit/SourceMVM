[CmdletBinding()]
param(
    [ValidateRange(0, 16)][int]$Monitor = 3,
    [int]$Width = 854,
    [int]$Height = 480,
    [int]$Port = 29010,
    [int]$SettleSeconds = 10,
    [string]$OutDir = '',
    [string]$Cs2Dir = '',
    [switch]$EnableDlss
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# Compatibility entry point: standard rendering unless DLSS is explicitly requested.
# Installation is a separate manual operation, never a launch-time side effect.
if (-not $EnableDlss) {
    & (Join-Path $repo 'Automation/launch/launch-cs2-netcon.ps1') -Cs2Dir $Cs2Dir -Monitor $Monitor -Width $Width -Height $Height -Port $Port -SettleSeconds $SettleSeconds -OutDir $OutDir
    return
}
$privateRoot = Join-Path $repo 'build/dlss5-cs2'
$bin = Join-Path $privateRoot 'game/bin/win64'
if (Get-Process cs2 -ErrorAction SilentlyContinue) {
    throw 'Close your current CS2 session before launching the separate DLSS session.'
}
if (-not (Get-Command nvidia-smi -ErrorAction SilentlyContinue)) { throw 'The DLSS launch requires an NVIDIA driver. Use live.bat for the standard renderer.' }
$driver = & nvidia-smi --query-gpu=driver_version --format=csv,noheader
if ($LASTEXITCODE -ne 0 -or -not $driver) { throw 'Could not read the NVIDIA driver version.' }
if ([version](@($driver)[0].Trim()) -lt [version]'616.56') {
    throw 'Update the NVIDIA driver to 616.56 or newer before testing DLSS 5.'
}
if (-not (Test-Path -LiteralPath (Join-Path $privateRoot 'dlss5-files.json')) -or
    -not (Test-Path -LiteralPath (Join-Path $bin 'cs2.exe'))) {
    throw 'Optional DLSS setup is not installed. See Automation/dlss/README.md for manual setup, or use live.bat.'
}
& (Join-Path $repo 'Automation/launch/launch-cs2-netcon.ps1') -Cs2Dir $privateRoot -Monitor $Monitor -Width $Width -Height $Height -Port $Port -SettleSeconds $SettleSeconds -OutDir $OutDir
