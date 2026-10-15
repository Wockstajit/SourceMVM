Set-StrictMode -Version Latest

function Get-AutomationRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

function Get-AutomationRepositoryRoot {
    return (Split-Path -Parent (Get-AutomationRoot))
}

function Get-NewestCs2HookBuildDll {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    @(
        (Join-Path $RepositoryRoot 'build\x64-source2-release\AfxHookSource2\Release\AfxHookSource2.dll'),
        (Join-Path $RepositoryRoot 'build\x64-release\AfxHookSource2\Release\AfxHookSource2.dll')
    ) | Where-Object { Test-Path -LiteralPath $_ } |
        Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending |
        Select-Object -First 1
}

function Copy-NewerCs2HookBuildDll {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [AllowEmptyString()]
        [string]$SourceDll,
        [Parameter(Mandatory = $true)]
        [string]$StagedDll
    )

    if ($SourceDll) {
        $builtDllInfo = Get-Item -LiteralPath $SourceDll
        $stagedDllInfo = Get-Item -LiteralPath $StagedDll
        if ($builtDllInfo.LastWriteTimeUtc -gt $stagedDllInfo.LastWriteTimeUtc) {
            $copyDeadline = (Get-Date).AddSeconds(10)
            while ($true) {
                try {
                    Copy-Item -LiteralPath $SourceDll -Destination $StagedDll -Force
                    break
                } catch {
                    if ((Get-Date) -ge $copyDeadline) { throw }
                    Start-Sleep -Milliseconds 250
                }
            }
            Write-Host "Staged newer AfxHookSource2.dll from the x64 release build."
        }
    }
}

# Where Counter-Strike 2 is installed on THIS machine.
#
# Every launch/verify script used to carry the author's own library path as its parameter
# default, so a fresh clone could not launch the game without editing scripts. Resolution
# order, same as the Electron launcher's detectCs2Exe():
#
#   1. $env:SOURCEMVM_CS2_DIR  - explicit override, wins over everything.
#   2. Steam's registered install path, then every library in steamapps/libraryfolders.vdf.
#   3. -Fallback, for a caller that wants a last resort rather than an error.
#
# Returns the install ROOT (the folder containing gamein\win64\cs2.exe), or '' when CS2
# cannot be found; callers decide whether that is fatal.
function Get-Cs2InstallDir {
    [CmdletBinding()]
    param(
        [string]$Fallback = ''
    )

    $relativeExe = Join-Path 'game' (Join-Path 'bin' (Join-Path 'win64' 'cs2.exe'))

    # [System.IO.*] rather than Join-Path/Test-Path throughout the probing below. Those cmdlets go
    # through PowerShell's drive provider and ERROR on a path whose drive letter does not exist
    # ("Cannot find drive"), which an override naming an unplugged drive trivially produces --
    # and Join-Path's error is non-terminating, so it prints past a try/catch. The .NET calls are
    # pure string math plus a filesystem probe: no provider, no error, just $false.
    if (-not [string]::IsNullOrWhiteSpace($env:SOURCEMVM_CS2_DIR)) {
        if ([System.IO.File]::Exists([System.IO.Path]::Combine($env:SOURCEMVM_CS2_DIR, $relativeExe))) {
            return $env:SOURCEMVM_CS2_DIR
        }
    }

    $steam = ''
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKCU:\SOFTWARE\Valve\Steam')) {
        try {
            $item = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($name in @('InstallPath', 'SteamPath')) {
                if ($item.PSObject.Properties.Name -contains $name -and $item.$name) {
                    $steam = [string]$item.$name
                    break
                }
            }
        } catch {
            # Steam may be installed without registering, or the key may not exist for this user.
        }
        if ($steam) { break }
    }
    if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }
    $steam = $steam.Replace('/', '\')

    $libraries = New-Object System.Collections.Generic.List[string]
    $libraries.Add($steam)
    $vdf = [System.IO.Path]::Combine($steam, 'steamapps', 'libraryfolders.vdf')
    if ([System.IO.File]::Exists($vdf)) {
        # Paths inside a .vdf are C-escaped, so a real "F:\SteamLibrary" is stored "F:\\SteamLibrary".
        foreach ($match in [regex]::Matches([System.IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
            $library = $match.Groups[1].Value.Replace('\\', '\')
            if (-not $libraries.Contains($library)) { $libraries.Add($library) }
        }
    }

    foreach ($library in $libraries) {
        $candidate = [System.IO.Path]::Combine($library, 'steamapps', 'common', 'Counter-Strike Global Offensive')
        if ([System.IO.File]::Exists([System.IO.Path]::Combine($candidate, $relativeExe))) {
            return $candidate
        }
    }

    return $Fallback
}

function New-AutomationRunFolder {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[A-Za-z0-9._-]+$')]
        [string]$Name,
        [string]$BaseDirectory
    )

    if ([string]::IsNullOrWhiteSpace($BaseDirectory)) {
        $BaseDirectory = Join-Path (Get-AutomationRoot) 'runs'
    }

    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $path = Join-Path (Join-Path $BaseDirectory $Name) $timestamp
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return (Resolve-Path -LiteralPath $path).Path
}

function Save-AutomationRunMetadata {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunDirectory,
        [Parameter(Mandatory = $true)]
        [string]$AutomationName,
        [hashtable]$Additional = @{}
    )

    $metadata = [ordered]@{
        automation = $AutomationName
        startedAt = (Get-Date).ToString('o')
        machine = $env:COMPUTERNAME
        user = $env:USERNAME
        primaryMonitor = $null
    }

    Add-Type -AssemblyName System.Windows.Forms
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $metadata.primaryMonitor = [ordered]@{
        x = $bounds.X
        y = $bounds.Y
        width = $bounds.Width
        height = $bounds.Height
    }

    foreach ($key in $Additional.Keys) {
        $metadata[$key] = $Additional[$key]
    }

    $path = Join-Path $RunDirectory 'run.json'
    $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}
