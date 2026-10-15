#requires -Version 5
<#
One command: scan for weapon-pickup moments (people holding a weapon that is not their
own), then verify the "current" steamId keyword correctly re-skins the weapon in the
HOLDER's hand instead of silently filing the override under the holder's own steamId
(the historical bug -- see verify-cosmetics-pickup-current.ps1's header comment).

  run-cosmetics-pickup-current.ps1              # fast scan + fast verify
  run-cosmetics-pickup-current.ps1 -Thorough     # slow, exhaustive (more probe ticks + transitions)
  run-cosmetics-pickup-current.ps1 -ScanOnly
  run-cosmetics-pickup-current.ps1 -VerifyOnly -ScanRawFile ...
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "",
    [string]$OutDir = "",
    [string]$ScanRawFile = "",
    [int]$MaxProbeTicks = 0,
    [int]$MaxCases = 12,
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$ScanOnly,
    [switch]$VerifyOnly,
    [switch]$NoLaunch
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot

if (-not $Thorough -and -not $PSBoundParameters.ContainsKey('Fast')) { $Fast = $true }

$scanScript = Join-Path $PSScriptRoot 'scan-cosmetics-weapon-moments.ps1'
$verifyScript = Join-Path $PSScriptRoot 'verify-cosmetics-pickup-current.ps1'

$scanRawPath = $null
if (-not $VerifyOnly) {
    Write-Host "======== SCAN for pickups (fast=$Fast) ========" -ForegroundColor Cyan

    $scanCmd = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scanScript,
        '-Port', $Port,
        '-Cs2Dir', $Cs2Dir,
        '-LeaveCs2Open'
    )
    if ($Fast) { $scanCmd += '-Fast' }
    if ($Thorough) {
        # Pickups are rarer than owned-weapon moments and often show up around weapon
        # switches shortly after a kill -- ScanTransitions widens the net for them.
        $scanCmd += @('-Thorough', '-ScanTransitions')
    }
    if ($Demo) { $scanCmd += @('-Demo', $Demo) }
    if ($OutDir) { $scanCmd += @('-OutDir', $OutDir) }
    if ($MaxProbeTicks -gt 0) { $scanCmd += @('-MaxProbeTicks', $MaxProbeTicks) }
    if ($NoLaunch) { $scanCmd += '-NoLaunch' }

    & powershell.exe @scanCmd
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($OutDir) {
        $base = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
        $scanRawPath = Join-Path $base 'scan_raw.json'
    } else {
        $scanRawPath = (Get-ChildItem (Join-Path $root 'Automation\output\cosmetics_weapon_moments') -Recurse -Filter 'scan_raw.json' |
            Sort-Object LastWriteTime -Desc | Select-Object -First 1).FullName
    }

    if ($ScanOnly) {
        Write-Host "Scan-only done: $scanRawPath" -ForegroundColor Green
        exit 0
    }
}

if ($ScanRawFile) { $scanRawPath = (Resolve-Path -LiteralPath $ScanRawFile).Path }

Write-Host ""
Write-Host "======== VERIFY (pickup / current) ========" -ForegroundColor Cyan

$verifyCmd = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $verifyScript,
    '-Port', $Port,
    '-Cs2Dir', $Cs2Dir,
    '-MaxCases', $MaxCases
)
if ($Fast) { $verifyCmd += '-Fast' }
if ($Thorough) { $verifyCmd += '-Thorough' }
if ($scanRawPath) { $verifyCmd += @('-ScanRawFile', $scanRawPath) }
if (-not $VerifyOnly) {
    $verifyCmd += '-NoLaunch'
} elseif ($NoLaunch) {
    $verifyCmd += '-NoLaunch'
}

& powershell.exe @verifyCmd
exit $LASTEXITCODE
