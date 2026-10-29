# Headlessly launches CS2 with the AfxHookSource2 hook injected and a TCP
# console (-netconport) open, using the SOURCE:MVM standalone launcher CLI.
# No HLAE GUI, no mouse/keyboard takeover.
#
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-netcon.ps1 -Port 29010
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [int]$Width = 854,
    [int]$Height = 480,
    [ValidateRange(0, 16)]
    [int]$Monitor = 3,
    [int]$ReadyTimeoutSeconds = 75,
    [int]$SettleSeconds = 10,
    [switch]$NoWait,
    [switch]$FreeMouse,
    [string]$FxAssetsGameDir = '',
    [string]$DebugHelper = '',
    [string]$NativeHostOverride = '',
    [string]$OutDir,
    # live.bat: launch from whichever checkout of this repository has the newest hook build
    # (Invoke-NewestHookCheckout). Automation leaves it off and launches its own checkout.
    [switch]$NewestHook
)
$ErrorActionPreference = 'Stop'
# Steam's SDK IPC belongs to the signed-in Windows user. The Codex sandbox runs as
# another account; creating CS2 there opens an SDK-error dialog instead of a game.
# Fail before staging, closing a session, or creating a child process.
$launchIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
if ($launchIdentity -match '(?i)\\codexsandbox') {
    throw "CS2 cannot connect to your Steam session from $launchIdentity. Run this launcher in the signed-in Windows user's context (outside the Codex sandbox)."
}
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$agentDebugLog = Join-Path $root 'debug-af2ef9.log'
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
. (Join-Path $automationRoot 'lib\Cs2StartupStatus.ps1')
if ($NewestHook) {
    $handedOff = Invoke-NewestHookCheckout -RepositoryRoot $root -ScriptName 'launch-cs2-netcon.ps1' -BoundParameters $PSBoundParameters
    if ($null -ne $handedOff) { exit $handedOff }
}

# Counter-Strike 2 location. Empty -Cs2Dir means 'work it out', which is the normal path:
# this used to default to the author's own Steam library, so a fresh clone could not launch
# the game without editing the script. Override with -Cs2Dir or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = Get-Cs2InstallDir }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass -Cs2Dir <install root> or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'launch-cs2'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
# The host runs from the game's bin directory; its PID file must be absolute.
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

function Resolve-Source2BinDir {
    $candidates = @(
        (Join-Path $root 'build\source2-release\bin'),
        (Join-Path $root 'build\staging-release\bin'),
        (Join-Path $root 'build\portable-runtime\bin')
    )
    foreach ($candidate in $candidates) {
        $dll = Join-Path $candidate 'x64\AfxHookSource2.dll'
        if ((Test-Path -LiteralPath $dll) -and (Test-Path -LiteralPath (Join-Path $candidate 'x64/injector.exe'))) { return $candidate }
    }
    return $candidates[0]
}

# A fresh clone has source, not compiled launch tools. Bootstrap the pinned portable
# package only when a complete local developer runtime/host is unavailable.
$localBin = Resolve-Source2BinDir
$localHost = @(
    (Join-Path $root 'SourceMovieMakerLauncher/host/build/SourceMovieMakerHost.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher/release/win-unpacked/resources/host/SourceMovieMakerHost.exe')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $localHost -or -not (Test-Path -LiteralPath (Join-Path $localBin 'x64/AfxHookSource2.dll')) -or -not (Test-Path -LiteralPath (Join-Path $localBin 'x64/injector.exe'))) {
    # Standard startup never requests or installs the optional DLSS package.
    & (Join-Path $automationRoot 'setup/ensure-packages.ps1') -Dlss:$false
}
$bin = Resolve-Source2BinDir
# Prefer the native injection host. Electron --headless can exit 0 immediately without
# leaving a lasting CS2 process; SourceMovieMakerHost.exe creates/resumes CS2 directly.
$hostCandidates = @(
    (Join-Path $root 'SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher\release\win-unpacked\resources\host\SourceMovieMakerHost.exe'),
    (Join-Path $root 'build\portable-runtime\bin\SourceMovieMakerHost.exe')
)
$nativeHost = $hostCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($NativeHostOverride) { $nativeHost = $NativeHostOverride }
$launcherCandidates = @(
    (Join-Path $root 'SourceMovieMakerLauncher\release\win-unpacked\SourceMovieMaker.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher\release\SourceMovieMaker.exe'),
    (Join-Path $bin 'SourceMovieMaker.exe')
)
$launcher = $launcherCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $launcher -and -not $nativeHost) {
    # Temporary fallback while a tree still has the legacy HLAE GUI staged.
    $legacyHlae = Join-Path $bin 'HLAE.exe'
    if (Test-Path -LiteralPath $legacyHlae) { $launcher = $legacyHlae }
}
$dll     = Join-Path $bin 'x64\AfxHookSource2.dll'
$injector = Join-Path $bin 'x64\injector.exe'
# Both hook-only build trees are valid producers; stage whichever DLL is newest so a build
# in either tree is never silently skipped in favor of a stale one in the other.
$builtDll = Get-NewestCs2HookBuildDll -RepositoryRoot $root
$cs2exe  = Join-Path $Cs2Dir 'game\bin\win64\cs2.exe'
if ([string]::IsNullOrWhiteSpace($FxAssetsGameDir)) {
    # Prefer the pack staged into the build (build-source2.bat copies FX\compiled there).
    $fxStageNames = @('build\source2-release\fx\source_mvm_fx', 'build\staging-release\fx\source_mvm_fx', 'build\portable-runtime\fx\source_mvm_fx')
    $fxCandidates = @($fxStageNames | ForEach-Object { Join-Path $root $_ })
    $FxAssetsGameDir = $fxCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'particles') } | Select-Object -First 1
    # FX\compiled is private and git-ignored: a worktree reads the main checkout's (Get-PrivateAssetRoots).
    $committedFx = Find-PrivateAsset -RepositoryRoot $root -RelativePath 'FX\compiled\source_mvm_fx\particles'
    if ($committedFx) { $committedFx = Split-Path -Parent $committedFx }
    if (-not $FxAssetsGameDir) {
        # No stage of its own: mount a writable copy (the game writes cfg\ into its mount, so neither
        # FX\compiled nor another checkout's stage is ever mounted itself). A worktree seeds it from the
        # main checkout's full stage -- the packs plus the stage's render materials and intro -- so a test
        # launch has every VFX pack the user's own launch has; cfg\ stays per checkout. A clone has
        # neither: FX\README.md.
        $FxAssetsGameDir = $fxCandidates[2]
        $mainStage = Get-PrivateAssetRoots -RepositoryRoot $root | Select-Object -Skip 1 | ForEach-Object {
            $assetRoot = $_; $fxStageNames | ForEach-Object { Join-Path $assetRoot $_ }
        } | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'particles') } | Select-Object -First 1
        if ($mainStage) {
            Write-Host "Seeding this checkout's FX stage from $mainStage ..."
            robocopy $mainStage $FxAssetsGameDir /E /XO /XD cfg /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
            if ($LASTEXITCODE -ge 8) { Write-Host "FX stage seed failed (robocopy $LASTEXITCODE): $FxAssetsGameDir" -ForegroundColor Yellow }
            $global:LASTEXITCODE = 0
        }
        if ($committedFx) {
            robocopy $committedFx $FxAssetsGameDir /E /XO /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
            $global:LASTEXITCODE = 0
        }
    } else {
        # Only build-source2.bat mirrors FX\compiled into the stage; the hook-only CMake installs
        # never do. A particle added to FX\compiled after the last full build was then missing
        # from the mount, CS2 handed back an empty placeholder, and the swap drew NOTHING (the
        # MW2019 flashbang, 2026-10-09). Copy new and newer files in on every launch; no purge,
        # so the stage's own render materials, intro and cfg stay. Unchanged files cost nothing.
        if ($committedFx) {
            robocopy $committedFx $FxAssetsGameDir /E /XO /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
            if ($LASTEXITCODE -ge 8) { Write-Host "FX stage sync failed (robocopy $LASTEXITCODE): $FxAssetsGameDir" -ForegroundColor Yellow }
            $global:LASTEXITCODE = 0
        }
    }
}
$fxAssetsAvailable = Test-Path -LiteralPath $FxAssetsGameDir
if (-not (Test-Path -LiteralPath (Join-Path $FxAssetsGameDir 'particles'))) {
    Write-Host "No VFX pack found in this checkout or the main one: CS2 starts without the SOURCE:MVM particles." -ForegroundColor Yellow
}

# Death Animations reads resources\ragdoll\ragdoll_animations.mvmanim next to the hook. Only an
# install configured with the private library present puts it there, so a worktree's stage never had
# it and ragdolls had no clips. Keep the stage current from whichever checkout has the library.
$ragdollSource = Find-PrivateAsset -RepositoryRoot $root -RelativePath 'FX\death-animations\ragdoll_animations.mvmanim'
if ($ragdollSource) {
    $ragdollStaged = Join-Path $bin 'resources\ragdoll\ragdoll_animations.mvmanim'
    $ragdollFrom = Get-Item -LiteralPath $ragdollSource
    $ragdollTo = Get-Item -LiteralPath $ragdollStaged -ErrorAction SilentlyContinue
    if (-not $ragdollTo -or $ragdollTo.Length -ne $ragdollFrom.Length -or $ragdollTo.LastWriteTimeUtc -lt $ragdollFrom.LastWriteTimeUtc) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ragdollStaged) | Out-Null
        Copy-Item -LiteralPath $ragdollSource -Destination $ragdollStaged -Force
        Write-Host "Staged the ragdoll animation library from $ragdollSource"
    }
} else {
    Write-Host 'No ragdoll animation library in this checkout or the main one: Death Animations has no clips.' -ForegroundColor Yellow
}

Initialize-StagedFfmpeg -StageBin $bin -RepositoryRoot $root

# A pipeline that matches nothing yields AutomationNull, not $null. It compares -eq $null as
# True, but contributes ZERO elements to an array concatenation (a real $null contributes one).
# So @($launcher) + ... silently added nothing here and the -not $f check below never saw the
# missing launcher: the script ran on and died 150 lines later inside Process.Start with
# "Cannot start process because a file name has not been provided". Cast to [string] first so
# an unresolved launcher is a plain '' that both checks can actually see.
$nativeHost = [string]$nativeHost
$launcher   = [string]$launcher
if (-not $nativeHost -and -not $launcher) {
    Write-Error ("No SOURCE:MVM launch host was found. Build one with" +
        " SourceMovieMakerLauncher\build-app.bat -Dir (or -Package). Looked for:`r`n  " +
        (($hostCandidates + $launcherCandidates) -join "`r`n  "))
    exit 1
}
$requiredLaunchTools = @($dll, $injector, $cs2exe)
if ($nativeHost) {
    $requiredLaunchTools = @($nativeHost) + $requiredLaunchTools
} else {
    $requiredLaunchTools = @($launcher) + $requiredLaunchTools
}
foreach ($f in $requiredLaunchTools) {
    if ([string]::IsNullOrWhiteSpace($f) -or -not (Test-Path -LiteralPath $f)) { Write-Error "Missing: $f"; exit 1 }
}

# End the old session through the engine so Steam can release its SDK connection.
$runningCs2 = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue)
if ($runningCs2.Count -gt 0) {
    Write-Host 'Closing the running CS2 (console quit)...'
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'netcon/cs2-netcon.ps1') -Port $Port -CommandBatch 'mvm_debug stop;;quit' -ReadSeconds 0.3
    $quitSent = ($LASTEXITCODE -eq 0)
    foreach ($cs2Process in $runningCs2) {
        # A hung or netcon-less CS2 never sees `quit`. The window's own close (WM_CLOSE) takes the
        # engine's normal shutdown too, so Steam still releases its SDK connection. Never
        # force-kill: that leaves the Steam pipe broken and the next launch black.
        if (-not $quitSent -or -not $cs2Process.WaitForExit(30000)) {
            Write-Host 'CS2 did not quit from the console; asking its window to close...'
            $cs2Process.Refresh()
            [void]$cs2Process.CloseMainWindow()
            if (-not $cs2Process.WaitForExit(45000)) {
                throw "CS2 (pid $($cs2Process.Id)) did not close from the console or its window. It is probably hung: close it from Task Manager yourself, then launch again."
            }
        }
    }
    Write-Host 'CS2 closed.'
}

# Hook-only development builds cannot overwrite the staged DLL while CS2 has it loaded.
# Once Live has closed CS2, promote a newer completed build before injection.
Copy-NewerCs2HookBuildDll -SourceDll $builtDll -StagedDll $dll

$steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }

# Mirror SourceMovieMaker / LaunchCs2 cmdline, plus the net console flags.
# NOTE: -novid is deliberately NOT passed. Verified 2026-07-29 on build 2000879 that CS2
# ignores it (the intro played with the flag present in the process command line), and if it
# ever started working it would suppress the SOURCE:MVM intro too -- that intro is now served
# by a head-priority search-path override, so the stock player must run normally.
$cmdLine = "-steam -insecure -windowed -sw -w $Width -h $Height -console +sv_lan 1 -netconport $Port -afxFixNetCon"
$expectedProcessIdPath = Join-Path $OutDir 'process-id.txt'
# -afxFreeMouse is a NO-OP as of the click-to-capture default (AfxHookSource2/Filmmaker/
# MouseCapture.h): the hook never captures the desktop cursor until you click into the game,
# whichever way it was launched. Still appended when the switch is used so older builds keep
# behaving the same; -afxGrabMouse is the opt-out if CS2's normal grab-on-focus is wanted.
if ($FreeMouse) { $cmdLine += " -afxFreeMouse" }
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'launch-cs2' -Additional @{
    port = $Port
    width = $Width
    height = $Height
    monitor = $Monitor
    windowed = $true
    freeMouse = $FreeMouse.IsPresent
    commandLine = $cmdLine
    launcher = if ($nativeHost) { $nativeHost } else { $launcher }
    fxAssetsGameDir = if ($fxAssetsAvailable) { (Resolve-Path -LiteralPath $FxAssetsGameDir).Path } else { '' }
} | Out-Null

$launchEnv = @{
    SteamPath = $steam
    SteamClientLaunch = '1'
    SteamGameId = '730'
    SteamAppId = '730'
    SteamOverlayGameId = '730'
    MVM_AGENT_DEBUG_LOG = $agentDebugLog
}
if ($fxAssetsAvailable) {
    $launchEnv['USRLOCALCSGO'] = (Resolve-Path -LiteralPath $FxAssetsGameDir).Path
    Write-Host "Mounting converted FX assets via USRLOCALCSGO: $FxAssetsGameDir" -ForegroundColor Green
} else {
    Write-Host "Converted FX asset dir not found; Povarehok swaps will fail open: $FxAssetsGameDir" -ForegroundColor Yellow
}

if ($nativeHost) {
    $launcherName = [System.IO.Path]::GetFileNameWithoutExtension($nativeHost)
    Write-Host "Launching CS2 via $launcherName (hook + netconport $Port, ${Width}x${Height} windowed)..."
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = (Split-Path -Parent $cs2exe)
    # Preserve host diagnostics on the caller's streams. The native host creates CS2
    # without inherited handles, so a captured caller can finish while the game stays open.
    foreach ($key in $launchEnv.Keys) {
        # Windows PowerShell 5.1 can expose ProcessStartInfo.EnvironmentVariables as null
        # for this native-host path. Set the launcher's process environment instead; the
        # native host and the CS2 process it creates inherit these values unchanged.
        [System.Environment]::SetEnvironmentVariable($key, [string]$launchEnv[$key], 'Process')
    }
    $hostArgs = @(
        '--cs2', $cs2exe,
        '--hook', $dll,
        '--injector', $injector,
        '--steam-path', $steam,
        '--pid-file', $expectedProcessIdPath,
        '--cmd-line', $cmdLine
    )
    $launchExe = $nativeHost
    $launchArgs = $hostArgs
    if (-not [string]::IsNullOrWhiteSpace($debugHelper) -and (Test-Path -LiteralPath $debugHelper)) {
        $hostArgs += @('--debug-helper', $debugHelper)
        $launchArgs = $hostArgs
        Write-Host "Debug helper inject: $debugHelper" -ForegroundColor Cyan
    }
    $psi.FileName = $launchExe
    $argumentListProperty = [System.Diagnostics.ProcessStartInfo].GetProperty('ArgumentList')
    if ($null -ne $argumentListProperty) {
        foreach ($a in $launchArgs) { $psi.ArgumentList.Add([string]$a) }
    } else {
        $quote = { param($s) if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }
        $psi.Arguments = (($launchArgs | ForEach-Object { & $quote ([string]$_) }) -join ' ')
    }
    # On a UTF-8 (65001) console the .NET Framework host writes a BOM into the injector's stdin and
    # injection fails ("not a valid binary format"). The .bat wrappers switch the codepage, but this
    # script is also called directly, so it guards itself: 1252 for the host, restored afterwards.
    if (-not ('MvmLaunch.ConsoleCp' -as [type])) {
        Add-Type -Namespace MvmLaunch -Name ConsoleCp -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint GetConsoleCP();
[DllImport("kernel32.dll")] public static extern uint GetConsoleOutputCP();
[DllImport("kernel32.dll")] public static extern bool SetConsoleCP(uint id);
[DllImport("kernel32.dll")] public static extern bool SetConsoleOutputCP(uint id);
'@
    }
    $previousInputCp = [MvmLaunch.ConsoleCp]::GetConsoleCP()
    $previousOutputCp = [MvmLaunch.ConsoleCp]::GetConsoleOutputCP()
    try {
        if ($previousInputCp -ne 0) { [void][MvmLaunch.ConsoleCp]::SetConsoleCP(1252); [void][MvmLaunch.ConsoleCp]::SetConsoleOutputCP(1252) }
        $hostProcess = [System.Diagnostics.Process]::Start($psi)
        $hostFinished = $hostProcess.WaitForExit(30000)
    } finally {
        if ($previousInputCp -ne 0) { [void][MvmLaunch.ConsoleCp]::SetConsoleCP($previousInputCp); [void][MvmLaunch.ConsoleCp]::SetConsoleOutputCP($previousOutputCp) }
    }
    if (-not $hostFinished) { throw 'Native injection host did not finish within 30 seconds.' }
    if ($hostProcess.ExitCode -ne 0) { throw "Native injection host failed with exit code $($hostProcess.ExitCode)." }
} else {
    $useLegacyCustomLoader = ([System.IO.Path]::GetFileName($launcher) -ieq 'HLAE.exe')
    if ($useLegacyCustomLoader) {
        $argList = @(
            '-customLoader','-autoStart','-noGui',
            '-hookDllPath', $dll,
            '-programPath', $cs2exe,
            '-cmdLine', $cmdLine
        )
        foreach ($key in $launchEnv.Keys) {
            $argList += @('-addEnv', ("$key=" + $launchEnv[$key]))
        }
    } else {
        $argList = @(
            '--headless',
            '--hook', $dll,
            '--injector', $injector,
            '--cs2', $cs2exe,
            '--cmd-line', $cmdLine
        )
        foreach ($key in $launchEnv.Keys) {
            $argList += @('--env', ("$key=" + $launchEnv[$key]))
        }
    }

    $launcherName = [System.IO.Path]::GetFileNameWithoutExtension($launcher)
    Write-Host "Launching CS2 via $launcherName (hook + netconport $Port, ${Width}x${Height} windowed)..."
    # Use ProcessStartInfo.ArgumentList so the multi-word -cmdLine stays a single argument.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $launcher
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $bin
    # PowerShell 7 (.NET Core) exposes ProcessStartInfo.ArgumentList for robust quoting.
    # Windows PowerShell 5.1 (.NET Framework) does not. Do not read $psi.ArgumentList
    # directly under StrictMode: on 5.1 that throws before the fallback can run.
    $argumentListProperty = [System.Diagnostics.ProcessStartInfo].GetProperty('ArgumentList')
    if ($null -ne $argumentListProperty) {
        foreach ($a in $argList) { $psi.ArgumentList.Add([string]$a) }
    } else {
        $quote = { param($s) if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }
        $psi.Arguments = (($argList | ForEach-Object { & $quote ([string]$_) }) -join ' ')
    }
    [void][System.Diagnostics.Process]::Start($psi)
}
if ($NoWait) {
    Write-Host "Launch started without readiness wait. Artifacts: $OutDir"
    exit 0
}

Write-Host "Waiting for CS2 window and netcon readiness..."
$pidDeadline = (Get-Date).AddSeconds(5)
while (-not (Test-Path -LiteralPath $expectedProcessIdPath) -and (Get-Date) -lt $pidDeadline) {
    Start-Sleep -Milliseconds 50
}
if (-not (Test-Path -LiteralPath $expectedProcessIdPath)) { throw 'The native host did not record the CS2 PID.' }
$expectedProcessId = [int](Get-Content -LiteralPath $expectedProcessIdPath)
$readinessTimer = [System.Diagnostics.Stopwatch]::StartNew()
$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Id $expectedProcessId -ErrorAction SilentlyContinue)) { throw "CS2 exited before readiness. Check the game/Steam error; no automatic relaunch was attempted." }
    $startupFailure = Get-Cs2StartupFailure -ProcessId $expectedProcessId
    if ($startupFailure) { throw $startupFailure }
    $cs2 = Get-Process -Id $expectedProcessId -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and $_.Responding } |
        Select-Object -First 1
    if ($cs2) {
        try {
            $probe = [System.Net.Sockets.TcpClient]::new()
            $connect = $probe.BeginConnect('127.0.0.1', $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(500)) {
                $probe.EndConnect($connect)
                $ready = $true
            }
            $probe.Dispose()
        } catch {
            $ready = $false
        }
    }
    if ($ready) { break }
    # 250ms, not 1s: this polls a local process and a loopback port, so the interval is pure
    # latency added AFTER the game is already up -- a second of it on every launch, for nothing.
    Start-Sleep -Milliseconds 250
}
$readinessTimer.Stop()
$observedProcesses = @((Get-Process -Id $expectedProcessId -ErrorAction SilentlyContinue | ForEach-Object {
    @{ pid = $_.Id; responding = $_.Responding; windowHandle = $_.MainWindowHandle.ToInt64(); cpuSeconds = $_.CPU }
}))
$readinessPath = Join-Path $OutDir 'readiness.json'
@{ ready = $ready; elapsedSeconds = $readinessTimer.Elapsed.TotalSeconds; port = $Port;
   processes = $observedProcesses; hook = $dll } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $readinessPath -Encoding UTF8
if (-not $ready) {
    throw "CS2 did not expose a responsive window and netcon port within $ReadyTimeoutSeconds seconds. Diagnostics: $readinessPath"
}
Write-Host ("CS2 window and netcon ready in {0:N1} seconds." -f $readinessTimer.Elapsed.TotalSeconds)

# Place the window after it has a real HWND. Monitor 0 follows the primary display even
# when Windows renumbers DISPLAY devices. Explicit positive values select DISPLAY<n>.
$windowMover = Join-Path $automationRoot 'tools\move-process-window-to-monitor.ps1'
if (Test-Path -LiteralPath $windowMover) {
    try {
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $windowMover `
            -ProcessName 'cs2' -Monitor $Monitor -TimeoutSeconds 12
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not move CS2 to monitor $Monitor (exit $LASTEXITCODE)." }
    } catch {
        Write-Warning "Could not move CS2 to monitor ${Monitor}: $($_.Exception.Message)"
    }
}

if ($SettleSeconds -gt 0) {
    # WAIT FOR THE HOOK TO ANSWER, not for a fixed duration.
    #
    # This used to be a flat `Start-Sleep -Seconds $SettleSeconds`, which paid the full ten
    # seconds on EVERY launch even when the hook was already answering after two -- and, being a
    # blind sleep, it still could not promise readiness on a slow one. The open netcon PORT that
    # the readiness loop above waits for is not the same thing as a working hook: the port is
    # opened by the engine, while `mirv_filmmaker` only answers once the DLL has installed its
    # hooks. So ask the hook directly, and stop as soon as it replies.
    #
    # $SettleSeconds is now a CAP rather than a cost: same worst case as before, and a launch
    # that is ready early returns early. Not answering is not fatal -- a caller that only wants
    # the game running should not be failed by a triage command -- so this falls through with a
    # warning and lets the caller's own probes decide.
    Write-Host "CS2 is responsive; waiting up to $SettleSeconds seconds for the hook to answer..."
    $settleTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $settleDeadline = (Get-Date).AddSeconds($SettleSeconds)
    $hookAnswered = $false
    while ((Get-Date) -lt $settleDeadline) {
        $startupFailure = Get-Cs2StartupFailure -ProcessId $expectedProcessId
        if ($startupFailure) { throw $startupFailure }
        try {
            $probe = New-Object System.Net.Sockets.TcpClient
            $probeConnect = $probe.BeginConnect('127.0.0.1', $Port, $null, $null)
            if ($probeConnect.AsyncWaitHandle.WaitOne(400)) {
                $probe.EndConnect($probeConnect)
                $probe.NoDelay = $true
                $probeStream = $probe.GetStream()
                $probeStream.ReadTimeout = 700
                $payload = [System.Text.Encoding]::ASCII.GetBytes("mirv_filmmaker health`n")
                $probeStream.Write($payload, 0, $payload.Length)
                $probeStream.Flush()
                $buffer = New-Object byte[] 8192
                $reply = ''
                $readDeadline = (Get-Date).AddMilliseconds(900)
                while ((Get-Date) -lt $readDeadline) {
                    try {
                        $count = $probeStream.Read($buffer, 0, $buffer.Length)
                        if ($count -gt 0) { $reply += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $count) }
                    } catch { break }
                    if ($reply -match '(?m)^mirv_filmmaker health:' -and $reply -match 'render pump') { break }
                }
                # The banner is enough: it only prints when the command is REGISTERED, which is
                # the thing being waited on. Matching on "healthy" instead would stall the launch
                # whenever a version-coupled subsystem is genuinely broken -- exactly the session
                # where getting in-game to diagnose it matters most.
                if ($reply -match '(?m)^mirv_filmmaker health:' -and $reply -match 'render pump') { $hookAnswered = $true }
            }
            $probe.Dispose()
        } catch {
            $hookAnswered = $false
        }
        if ($hookAnswered) { break }
        Start-Sleep -Milliseconds 250
    }
    $settleTimer.Stop()
    if ($hookAnswered) {
        Write-Host ("Hook answered in {0:N1} seconds." -f $settleTimer.Elapsed.TotalSeconds)
    } else {
        Write-Warning "Hook did not answer within $SettleSeconds seconds; continuing. The caller's own probes decide readiness."
    }
}
Write-Host "CS2 automation-ready at ${Width}x${Height} windowed on netcon $Port. Artifacts: $OutDir"
