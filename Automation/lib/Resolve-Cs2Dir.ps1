# Where is Counter-Strike 2 on this machine?
#
#   $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1')
#
# A one-call wrapper around Get-Cs2InstallDir for the many launch/verify/fx scripts that want
# the path and nothing else from Automation/lib. It exists so those scripts do NOT have to
# dot-source AutomationCommon.ps1: that file opens with `Set-StrictMode -Version Latest`, which
# dot-sourcing would leak into the caller, and most of these scripts were never written or
# tested under strict mode. Invoked with `&` this runs in its own scope, so the strict mode
# stays here.
#
# Resolution order (see Get-Cs2InstallDir): $env:SOURCEMVM_CS2_DIR, then Steam's registered
# install path and every library in libraryfolders.vdf, then -Fallback. Returns '' when CS2
# cannot be found, so the caller decides whether that is fatal.
[CmdletBinding()]
param(
    [string]$Fallback = ''
)

. (Join-Path $PSScriptRoot 'AutomationCommon.ps1')

Get-Cs2InstallDir -Fallback $Fallback
