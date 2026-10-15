[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$OutputDir = 'Automation/output/lighting-live',
    [int]$Port = 29010,
    [string[]]$Before = @()
)
# Records ONE HUD-free frame of the running game through mirv_streams and writes it as a PNG.
# Unlike capture-game-window.ps1 this excludes every Panorama panel, so two captures of the same
# paused pose differ only in the lighting that changed between them.
#   -Before  netcon commands to send (and settle) ahead of the recording, e.g. 'mirv_relight off'
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
if ($Before.Count) { [void](Send-Netcon $Before 0.12); Start-Sleep -Milliseconds 700 }
$result = Capture-Frame 'frame' $Name
Write-Host ("{0} ({1} frame(s))" -f $result.path, $result.frames)
