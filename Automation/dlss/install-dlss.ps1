# Compatibility entry point retained for old commands and documentation links.
# DLSS/ReShade must never be installed into Steam's normal CS2 executable directory.
[CmdletBinding()]
param(
    [ValidateSet('renodx', 'generic')]
    [string]$Addon = 'renodx',
    [ValidateSet('NVIDIA original', 'RTX 40, 30, 20 compatible')]
    [string]$RuntimeVariant = 'NVIDIA original',
    [string]$Cs2Dir = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

if ($Uninstall) {
    throw 'The retired shared-CS2 installer cannot uninstall files safely. DLSS is now isolated under build\dlss5-cs2; remove that owned private tree only if you intentionally want to uninstall it.'
}
if ($Addon -ne 'renodx') {
    throw 'The generic RenoDX add-on is not supported for CS2. Use the isolated RenoDX installation.'
}

Write-Warning 'The shared-CS2 DLSS installer has been retired. Redirecting to the private SOURCE:MVM installer.'
& (Join-Path $PSScriptRoot 'install-isolated.ps1') -RuntimeVariant $RuntimeVariant -Cs2Dir $Cs2Dir
