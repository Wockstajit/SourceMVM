<#
.SYNOPSIS
  Finds a Steam game's install folder from an override variable, the registry and every Steam library.

.DESCRIPTION
  Shared by the Source 1 game resolvers (Resolve-CssDir.ps1 now; other Source 1 games later), which
  differ only in the folder name, the executable that proves the folder is the game, and the
  override variable. Invoke it with `&` so it runs in its own scope.

  Resolution order:
    1. The environment variable named by -OverrideVariable, when it points at a folder that holds
       -RelativeExe.
    2. Steam's registered install path, then every library in steamapps/libraryfolders.vdf.

  Returns '' when the game is not found, so each caller keeps its own error text.

.EXAMPLE
  & "$PSScriptRoot\Find-SteamAppDir.ps1" -AppFolder 'Counter-Strike Source' -RelativeExe 'cstrike_win64.exe' -OverrideVariable 'SOURCEMVM_CSS_DIR'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AppFolder,
    [Parameter(Mandatory = $true)]
    [string]$RelativeExe,
    [string]$OverrideVariable = '',
    [string]$Fallback = ''
)

Set-StrictMode -Off

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
    # Only for a Steam that is not registered. The registry above is the real answer.
    foreach ($conventional in @('C:\Program Files (x86)\Steam', 'C:\Program Files\Steam')) {
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

if ($OverrideVariable) {
    $override = [Environment]::GetEnvironmentVariable($OverrideVariable)
    if ($override) {
        $override = $override.TrimEnd('\')
        if (Test-Path -LiteralPath (Join-Path $override $RelativeExe)) { return $override }
    }
}

foreach ($library in (Get-SteamLibraries)) {
    $candidate = Join-Path $library "steamapps\common\$AppFolder"
    if (Test-Path -LiteralPath (Join-Path $candidate $RelativeExe)) { return $candidate }
}

return $Fallback
