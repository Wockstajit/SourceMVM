<#
.SYNOPSIS
  Finds Deadlock on this machine.

.DESCRIPTION
  The Deadlock counterpart of Automation/lib/Resolve-Cs2Dir.ps1, and a thin `&`-invoked wrapper for
  the same reason that one is: AutomationCommon.ps1 sets `Set-StrictMode -Version Latest`, and
  dot-sourcing it would leak that into every caller.

  Resolution order:
    1. $env:SOURCEMVM_DEADLOCK_DIR  - explicit override, wins outright.
    2. Steam's registered install path, then every library in steamapps/libraryfolders.vdf.

  Returns '' rather than throwing when Deadlock is not found, so callers keep their own error text.

.EXAMPLE
  $deadlock = & "$PSScriptRoot\..\lib\Resolve-DeadlockDir.ps1"
#>
[CmdletBinding()]
param(
    [string]$Fallback = ''
)

Set-StrictMode -Off

$relativeExe = 'game\bin\win64\deadlock.exe'
$appFolder = 'Deadlock'

function Get-SteamRoots {
    $roots = @()

    foreach ($entry in @(
        @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Value = 'InstallPath' },
        @{ Path = 'HKCU:\SOFTWARE\Valve\Steam'; Value = 'SteamPath' }
    )) {
        try {
            $found = (Get-ItemProperty -Path $entry.Path -Name $entry.Value -ErrorAction Stop).($entry.Value)
            if ($found) { $roots += ($found -replace '/', '\') }
        } catch { }
    }

    # Deliberately generic. No drive letter here is anyone's in particular; the registry above is
    # the real answer and this is only for a Steam that is not registered.
    foreach ($conventional in @(
        'C:\Program Files (x86)\Steam',
        'C:\Program Files\Steam'
    )) {
        if ($roots -notcontains $conventional) { $roots += $conventional }
    }

    return $roots
}

function Get-SteamLibraries {
    $libraries = @()
    foreach ($root in (Get-SteamRoots)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        if ($libraries -notcontains $root) { $libraries += $root }

        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $vdf)) { continue }
        try { $text = Get-Content -LiteralPath $vdf -Raw -ErrorAction Stop } catch { continue }
        # Paths inside a .vdf are C-escaped: a real E:\SteamLibrary is stored "E:\\SteamLibrary".
        foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
            $library = $m.Groups[1].Value -replace '\\\\', '\'
            if ($libraries -notcontains $library) { $libraries += $library }
        }
    }
    return $libraries
}

if ($env:SOURCEMVM_DEADLOCK_DIR) {
    $override = $env:SOURCEMVM_DEADLOCK_DIR.TrimEnd('\')
    if (Test-Path -LiteralPath (Join-Path $override $relativeExe)) {
        return $override
    }
}

foreach ($library in (Get-SteamLibraries)) {
    $candidate = Join-Path $library "steamapps\common\$appFolder"
    if (Test-Path -LiteralPath (Join-Path $candidate $relativeExe)) {
        return $candidate
    }
}

return $Fallback
