Set-StrictMode -Version Latest

function Get-AutomationRoot {
    return (Split-Path -Parent $PSScriptRoot)
}

function Get-AutomationRepositoryRoot {
    return (Split-Path -Parent (Get-AutomationRoot))
}

# The checkouts that may hold the PRIVATE local assets: this one first, then the main checkout when
# this is a git worktree. FX\compiled (the VFX packs), FX\death-animations (the ragdoll clips) and
# a full build's FX stage are git-ignored, so a worktree has none of them; a launch from one started
# CS2 with no VFX pack mounted and no ragdoll clips (2026-10-17).
function Get-PrivateAssetRoots {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    $roots = @([IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\'))
    $common = ''
    try { $common = [string](& git -C $RepositoryRoot rev-parse --path-format=absolute --git-common-dir 2>$null) } catch { }
    $global:LASTEXITCODE = 0
    if ($common) {
        $main = [IO.Path]::GetFullPath((Split-Path -Parent $common.Trim())).TrimEnd('\')
        if ((Test-Path -LiteralPath $main) -and -not ($roots | Where-Object { $_ -ieq $main })) { $roots += $main }
    }
    return $roots
}

# The first of Get-PrivateAssetRoots that has `RelativePath`, or ''.
function Find-PrivateAsset {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    foreach ($assetRoot in (Get-PrivateAssetRoots -RepositoryRoot $RepositoryRoot)) {
        $candidate = Join-Path $assetRoot $RelativePath
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return ''
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

# The hook a checkout would launch on its own, and when it was built: its staged DLL, or a newer
# completed build the launch stages first (Copy-NewerCs2HookBuildDll). Null when the checkout
# cannot launch by itself (no staged hook and injector, or no native host): a launch from there
# would bootstrap a downloaded runtime instead.
function Get-Cs2CheckoutHook {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    $bin = @('build\source2-release\bin', 'build\staging-release\bin', 'build\portable-runtime\bin') |
        ForEach-Object { Join-Path $RepositoryRoot $_ } |
        Where-Object { (Test-Path -LiteralPath (Join-Path $_ 'x64\AfxHookSource2.dll')) -and (Test-Path -LiteralPath (Join-Path $_ 'x64\injector.exe')) } |
        Select-Object -First 1
    $nativeHost = @('SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe',
        'SourceMovieMakerLauncher\release\win-unpacked\resources\host\SourceMovieMakerHost.exe') |
        ForEach-Object { Join-Path $RepositoryRoot $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $bin -or -not $nativeHost) { return $null }
    $built = (Get-Item -LiteralPath (Join-Path $bin 'x64\AfxHookSource2.dll')).LastWriteTimeUtc
    $newestBuild = Get-NewestCs2HookBuildDll -RepositoryRoot $RepositoryRoot
    if ($newestBuild) {
        $buildTime = (Get-Item -LiteralPath $newestBuild).LastWriteTimeUtc
        if ($buildTime -gt $built) { $built = $buildTime }
    }
    return [pscustomobject]@{ Root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\'); Built = $built }
}

# This repository's checkouts a hook may be tested from: the main checkout and every worktree on a
# branch, as { Root, Branch }. A detached worktree is a release or bisect tree, not work under test.
function Get-Cs2RepositoryCheckouts {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    $lines = @()
    try { $lines = @(& git -C $RepositoryRoot worktree list --porcelain 2>$null) } catch { }
    $global:LASTEXITCODE = 0
    $checkouts = @()
    $path = $null; $branch = $null
    foreach ($line in ($lines + '')) {
        if ($line -like 'worktree *') { $path = $line.Substring(9); $branch = $null }
        elseif ($line -like 'branch refs/heads/*') { $branch = $line.Substring(18) }
        elseif ($line -eq '') {
            if ($path -and $branch -and (Test-Path -LiteralPath $path)) {
                $checkouts += [pscustomobject]@{ Root = [IO.Path]::GetFullPath($path).TrimEnd('\'); Branch = $branch }
            }
            $path = $null; $branch = $null
        }
    }
    return $checkouts
}

# The checkout whose hook is the newest launchable build, when that is another checkout than
# `RepositoryRoot`; null when this checkout's own is newest (ties stay here) or nothing else can
# launch. `Checkouts` defaults to Get-Cs2RepositoryCheckouts; each candidate is { Root, Branch }.
# Returns { Root, Branch, Built, OwnBuilt }.
function Find-NewerCs2HookCheckout {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,
        [object[]]$Checkouts
    )

    if ($null -eq $Checkouts) { $Checkouts = @(Get-Cs2RepositoryCheckouts -RepositoryRoot $RepositoryRoot) }
    $here = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
    $own = Get-Cs2CheckoutHook -RepositoryRoot $here
    $best = $null
    foreach ($checkout in $Checkouts) {
        if ([IO.Path]::GetFullPath($checkout.Root).TrimEnd('\') -ieq $here) { continue }
        $hook = Get-Cs2CheckoutHook -RepositoryRoot $checkout.Root
        if (-not $hook) { continue }
        if ($own -and $hook.Built -le $own.Built) { continue }
        if ($best -and $hook.Built -le $best.Built) { continue }
        $best = [pscustomobject]@{ Root = $hook.Root; Branch = $checkout.Branch; Built = $hook.Built; OwnBuilt = $(if ($own) { $own.Built } else { $null }) }
    }
    return $best
}

# live.bat's -NewestHook. When another checkout of this repository holds a newer launchable hook
# build, run the same launcher from there, with its own stage, shaders and resources, and return
# its exit code. Returns $null when this checkout's hook is the newest, so the caller launches as
# usual. Only live.bat passes the switch: automation launches the checkout it runs in, because an
# agent's verification has to run the code it just built.
function Invoke-NewestHookCheckout {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]
        [string]$ScriptName,
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$BoundParameters,
        [object[]]$Checkouts   # Find-NewerCs2HookCheckout's; tests pass fixtures
    )

    $newer = Find-NewerCs2HookCheckout -RepositoryRoot $RepositoryRoot -Checkouts $Checkouts
    if (-not $newer) { return $null }
    $target = Join-Path $newer.Root (Join-Path 'Automation\launch' $ScriptName)
    if (-not (Test-Path -LiteralPath $target)) { return $null }
    $own = if ($newer.OwnBuilt) { $newer.OwnBuilt.ToLocalTime().ToString('yyyy-MM-dd HH:mm') } else { 'none' }
    Write-Host ("The newest hook build is on {0}, built {1} (this checkout's: {2}). Launching from {3}" -f `
        $newer.Branch, $newer.Built.ToLocalTime().ToString('yyyy-MM-dd HH:mm'), $own, $newer.Root) -ForegroundColor Cyan
    $forward = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $target)
    foreach ($key in $BoundParameters.Keys) {
        if ($key -eq 'NewestHook') { continue }
        $value = $BoundParameters[$key]
        if ($value -is [System.Management.Automation.SwitchParameter]) {
            if ($value.IsPresent) { $forward += "-$key" }
        } elseif (-not [string]::IsNullOrEmpty([string]$value)) {
            # Windows PowerShell drops an empty native argument, which would leave the name without its value.
            $forward += "-$key"; $forward += [string]$value
        }
    }
    & powershell.exe @forward | Out-Host
    return $LASTEXITCODE
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
