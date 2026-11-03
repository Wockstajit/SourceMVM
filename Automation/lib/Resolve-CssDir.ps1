<#
.SYNOPSIS
  Finds Counter-Strike: Source (the 64-bit Steam build) on this machine.

.DESCRIPTION
  $env:SOURCEMVM_CSS_DIR wins outright; otherwise every Steam library is searched. Returns '' when
  CS:S is not found, so callers keep their own error text. Invoke with `&`.

.EXAMPLE
  $css = & "$PSScriptRoot\..\lib\Resolve-CssDir.ps1"
#>
[CmdletBinding()]
param(
    [string]$Fallback = ''
)

& (Join-Path $PSScriptRoot 'Find-SteamAppDir.ps1') -AppFolder 'Counter-Strike Source' `
    -RelativeExe 'cstrike_win64.exe' -OverrideVariable 'SOURCEMVM_CSS_DIR' -Fallback $Fallback
