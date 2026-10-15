#requires -Version 5
<#
One command: scan demo (fast) then verify skins - single CS2 session when possible.

  run-cosmetics-weapon-moments.ps1              # fast scan + fast verify
  run-cosmetics-weapon-moments.ps1 -Thorough    # slow, exhaustive
  run-cosmetics-weapon-moments.ps1 -ScanOnly
  run-cosmetics-weapon-moments.ps1 -VerifyOnly -RunFile ...
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "",
    [string]$OutDir = "",
    [string]$RunFile = "",
    [int]$MaxProbeTicks = 0,
    [int]$MaxWeaponsToTest = 40,
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$ScanOnly,
    [switch]$VerifyOnly,
    [switch]$PreviewOnly,
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
$verifyScript = Join-Path $PSScriptRoot 'verify-cosmetics-weapon-moments.ps1'

$runPath = $null
if (-not $VerifyOnly) {
    Write-Host "======== SCAN (fast=$Fast) ========" -ForegroundColor Cyan

    $scanCmd = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scanScript,
        '-Port', $Port,
        '-Cs2Dir', $Cs2Dir,
        '-LeaveCs2Open'
    )
    if ($Fast) { $scanCmd += '-Fast' }
    if ($Thorough) { $scanCmd += '-Thorough' }
    if ($Demo) { $scanCmd += @('-Demo', $Demo) }
    if ($OutDir) { $scanCmd += @('-OutDir', $OutDir) }
    if ($MaxProbeTicks -gt 0) { $scanCmd += @('-MaxProbeTicks', $MaxProbeTicks) }
    if ($NoLaunch) { $scanCmd += '-NoLaunch' }

    & powershell.exe @scanCmd
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($OutDir) {
        $base = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
        $runPath = Join-Path $base 'verification_run.json'
    } else {
        $runPath = (Get-ChildItem (Join-Path $root 'Automation\output\cosmetics_weapon_moments') -Recurse -Filter 'verification_run.json' |
            Sort-Object LastWriteTime -Desc | Select-Object -First 1).FullName
    }

    if ($ScanOnly) {
        Write-Host "Scan-only done: $runPath" -ForegroundColor Green
        exit 0
    }
}

if ($RunFile) { $runPath = (Resolve-Path -LiteralPath $RunFile).Path }

Write-Host ""
Write-Host "======== VERIFY ========" -ForegroundColor Cyan

$verifyCmd = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $verifyScript,
    '-Port', $Port,
    '-Cs2Dir', $Cs2Dir,
    '-MaxWeaponsToTest', $MaxWeaponsToTest
)
if ($Fast) { $verifyCmd += '-Fast' }
if ($Thorough) { $verifyCmd += '-Thorough' }
if ($runPath) { $verifyCmd += @('-RunFile', $runPath) }
if ($PreviewOnly) { $verifyCmd += '-PreviewOnly' }
if (-not $VerifyOnly) {
    $verifyCmd += '-NoLaunch'
} elseif ($NoLaunch) {
    $verifyCmd += '-NoLaunch'
}

& powershell.exe @verifyCmd
exit $LASTEXITCODE
