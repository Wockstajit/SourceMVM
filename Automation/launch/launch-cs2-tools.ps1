# Launches CS2 in WORKSHOP TOOLS mode with the AfxHookSource2 hook injected and a TCP
# console (-netconport) open.
#
# This is deliberately a SEPARATE script from launch-cs2-netcon.ps1. Tools mode has its own
# preflight (the Workshop Tools DLC content must be installed), its own command line, and its
# own failure modes. It must not silently become a switch on the normal launch path.
#
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-tools.ps1
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-tools.ps1 -Addon my_addon
#
# Why these flags (all verified against the shipped binaries on 2026-08-07):
#   -tools   recognized by engine2.dll and cs2.exe. This is what Steam's "Counter-Strike 2
#            Workshop Tools" entry ultimately reaches; Steam runs game\bin\win64\csgocfg.exe,
#            which is only a picker dialog that then starts cs2.exe. We skip the dialog.
#   -dx11    recognized by engine2.dll and rendersystemdx11.dll. engine2 chooses between
#            rendersystemdx11 / rendersystemvulkan / rendersystemempty, and Steam's tools entry
#            passes -vulkan. Vulkan would bypass EVERY D3D11 hook this project owns, so we pin
#            D3D11 explicitly rather than relying on the default.
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [int]$Width = 854,
    [int]$Height = 480,
    [ValidateRange(1, 16)]
    [int]$Monitor = 3,
    [int]$ReadyTimeoutSeconds = 120,
    [int]$SettleSeconds = 10,
    # Optional workshop addon to mount (-addon <name>). Tools mode runs without one.
    [string]$Addon = '',
    [switch]$NoWait,
    [switch]$FreeMouse,
    # By default this launcher disables CS2's unfocused-window throttle (engine_no_focus_sleep),
    # so the game keeps running while you work in another window. Pass this to keep stock behavior.
    [switch]$KeepFocusSleep,
    [string]$FxAssetsGameDir = '',
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$agentDebugLog = Join-Path $root 'debug-af2ef9.log'
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')

# Counter-Strike 2 location. Empty -Cs2Dir means 'work it out', which is the normal path:
# this used to default to the author's own Steam library, so a fresh clone could not launch
# the game without editing the script. Override with -Cs2Dir or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = Get-Cs2InstallDir }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass -Cs2Dir <install root> or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'launch-cs2-tools'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $OutDir = (Resolve-Path -LiteralPath $OutDir).Path
}

function Resolve-Source2BinDir {
    $candidates = @(
        (Join-Path $root 'build\source2-release\bin'),
        (Join-Path $root 'build\staging-release\bin')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'x64\AfxHookSource2.dll')) { return $candidate }
    }
    return $candidates[0]
}

# ---- Workshop Tools preflight ---------------------------------------------------------------
# The tools are a separate Steam DLC (appid 2279721, "Counter-Strike 2 Workshop Tools"). Its
# content lands in the SAME install directory as the base game, so there is no separate folder to
# look for -- the only reliable local check is whether the tool binaries are actually on disk.
$WorkshopToolsAppId = 2279721
$toolsProbe = @(
    (Join-Path $Cs2Dir 'game\bin\win64\tools\hammer.dll'),
    (Join-Path $Cs2Dir 'game\bin\win64\csgocfg.exe')
)

function Test-WorkshopToolsInstalled {
    return @($toolsProbe | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -eq 0
}

if (-not (Test-WorkshopToolsInstalled)) {
    $missingTools = @($toolsProbe | Where-Object { -not (Test-Path -LiteralPath $_) })
    Write-Host ''
    Write-Host 'Counter-Strike 2 Workshop Tools are NOT installed.' -ForegroundColor Yellow
    Write-Host "  Missing: $($missingTools -join '; ')"
    Write-Host "  They are a free Steam DLC (appid $WorkshopToolsAppId) and are required for -tools mode."
    Write-Host ''

    # Interactive runs get an offer to kick off the Steam install directly. A non-interactive run
    # (agent, CI, double-clicked .bat with no console owner) must never block on a prompt, so it
    # falls through to the actionable error below instead.
    $canPrompt = -not [System.Console]::IsInputRedirected
    $startInstall = $false
    if ($canPrompt) {
        try {
            $answer = Read-Host 'Open Steam now to install the Workshop Tools DLC? [Y/n]'
            $startInstall = [string]::IsNullOrWhiteSpace($answer) -or $answer -match '^(y|yes)$'
        } catch {
            $startInstall = $false
        }
    }

    if ($startInstall) {
        # steam://install/<appid> makes the Steam client show its own install dialog for that app.
        Write-Host "Asking Steam to install appid $WorkshopToolsAppId ..." -ForegroundColor Cyan
        try {
            Start-Process "steam://install/$WorkshopToolsAppId"
        } catch {
            Write-Warning "Could not hand off to Steam: $($_.Exception.Message)"
        }

        Write-Host 'Waiting for the Workshop Tools content to appear (Ctrl+C to stop)...'
        Write-Host 'Confirm the install in the Steam window; this checks every 5s for up to 30 minutes.'
        $installDeadline = (Get-Date).AddMinutes(30)
        while ((Get-Date) -lt $installDeadline) {
            Start-Sleep -Seconds 5
            if (Test-WorkshopToolsInstalled) { break }
        }
    }

    if (-not (Test-WorkshopToolsInstalled)) {
        Write-Error @"
Counter-Strike 2 Workshop Tools content is still not installed. Missing:
  $($missingTools -join "`n  ")

Install it in Steam, then re-run this launcher:
  * steam://install/$WorkshopToolsAppId
  * or: Steam Library -> Counter-Strike 2 -> Manage/Properties -> DLC ->
    tick "Counter-Strike 2 Workshop Tools" and let it download.
"@
        exit 1
    }
    Write-Host 'Workshop Tools content detected. Continuing.' -ForegroundColor Green
}

$bin = Resolve-Source2BinDir
$hostCandidates = @(
    (Join-Path $root 'SourceMovieMakerLauncher\release\win-unpacked\resources\host\SourceMovieMakerHost.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe')
)
$nativeHost = $hostCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$launcherCandidates = @(
    (Join-Path $root 'SourceMovieMakerLauncher\release\win-unpacked\SourceMovieMaker.exe'),
    (Join-Path $root 'SourceMovieMakerLauncher\release\SourceMovieMaker.exe'),
    (Join-Path $bin 'SourceMovieMaker.exe')
)
$launcher = $launcherCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

$dll      = Join-Path $bin 'x64\AfxHookSource2.dll'
$injector = Join-Path $bin 'x64\injector.exe'
$cs2exe   = Join-Path $Cs2Dir 'game\bin\win64\cs2.exe'
# Both hook-only build trees are valid producers; stage whichever DLL is newest so a build
# in either tree is never silently skipped in favor of a stale one in the other.
$builtDll = Get-NewestCs2HookBuildDll -RepositoryRoot $root

if ([string]::IsNullOrWhiteSpace($FxAssetsGameDir)) {
    $fxCandidates = @(
        (Join-Path $root 'build\source2-release\fx\source_mvm_fx'),
        (Join-Path $root 'build\staging-release\fx\source_mvm_fx')
    )
    $FxAssetsGameDir = $fxCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $FxAssetsGameDir) { $FxAssetsGameDir = $fxCandidates[0] }
}
$fxAssetsAvailable = Test-Path -LiteralPath $FxAssetsGameDir

$required = @($dll, $injector, $cs2exe)
if ($nativeHost) { $required = @($nativeHost) + $required }
elseif ($launcher) { $required = @($launcher) + $required }
else { Write-Error "No SourceMovieMaker host or launcher found. Build it first."; exit 1 }
foreach ($f in $required) {
    if (-not $f -or -not (Test-Path -LiteralPath $f)) { Write-Error "Missing: $f"; exit 1 }
}

# Only one Steam instance is allowed, and the staged DLL cannot be replaced while CS2 holds it.
$runningCs2 = @(Get-Process -Name cs2 -ErrorAction SilentlyContinue)
if ($runningCs2.Count -gt 0) {
    $runningCs2 | Stop-Process -Force
    foreach ($p in $runningCs2) { try { [void]$p.WaitForExit(10000) } catch { } }
}

Copy-NewerCs2HookBuildDll -SourceDll $builtDll -StagedDll $dll

$steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }

# ---- "game freezes when unfocused" fix ------------------------------------------------------
# In tools mode the toolframework only renders the FOCUSED window: click into any other window
# and the game viewport stops. engine_no_focus_sleep 0 does NOT fix this — the real switch is
# Tools menu -> "Render All Windows", persisted as `always_render_all_windows` in
# game\_toolsettings\toolframework\settings.txt (kv3). Found by the user on 2026-08-07.
# Ensure it is true before every launch so a fresh machine/profile gets it by default.
$tfSettings = Join-Path $Cs2Dir 'game\_toolsettings\toolframework\settings.txt'
try {
    if (Test-Path -LiteralPath $tfSettings) {
        $tfText = Get-Content -LiteralPath $tfSettings -Raw
        if ($tfText -match 'always_render_all_windows\s*=\s*false') {
            (
                $tfText -replace 'always_render_all_windows\s*=\s*false', 'always_render_all_windows = true'
            ) | Set-Content -LiteralPath $tfSettings -Encoding UTF8 -NoNewline
            Write-Host 'Enabled "Render All Windows" in toolframework settings (was false).' -ForegroundColor Green
        } elseif ($tfText -notmatch 'always_render_all_windows') {
            (
                $tfText -replace '(?s)^(.*?\{)', ("`$1`r`n`talways_render_all_windows = true")
            ) | Set-Content -LiteralPath $tfSettings -Encoding UTF8 -NoNewline
            Write-Host 'Added "Render All Windows" to toolframework settings.' -ForegroundColor Green
        }
    } else {
        New-Item -ItemType Directory -Path (Split-Path -Parent $tfSettings) -Force | Out-Null
        @"
<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
	always_render_all_windows = true
}
"@ | Set-Content -LiteralPath $tfSettings -Encoding UTF8
        Write-Host 'Created toolframework settings with "Render All Windows" enabled.' -ForegroundColor Green
    }
} catch {
    Write-Warning "Could not ensure always_render_all_windows in ${tfSettings}: $($_.Exception.Message)"
}

$cmdLine = "-steam -insecure -tools -dx11 -windowed -sw -w $Width -h $Height -console +sv_lan 1 -netconport $Port -afxFixNetCon"
# CS2 sleeps 20ms per frame whenever the window is not focused (engine2's
# `engine_no_focus_sleep`, default 20), which reads as "the game pauses when you click away".
# That is unusable for filmmaking: the whole workflow is driving the game from another window --
# netcon, the SOURCE:MVM launcher, an editor -- while watching playback. Verified live over
# netcon on 2026-08-07 that the default really is 20 and that setting 0 is accepted.
if (-not $KeepFocusSleep) { $cmdLine += " +engine_no_focus_sleep 0" }
if (-not [string]::IsNullOrWhiteSpace($Addon)) { $cmdLine += " -addon $Addon" }
if ($FreeMouse) { $cmdLine += " -afxFreeMouse" }

Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'launch-cs2-tools' -Additional @{
    port = $Port
    width = $Width
    height = $Height
    monitor = $Monitor
    toolsMode = $true
    renderBackend = 'dx11'
    addon = $Addon
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
}

Write-Host "Launching CS2 in WORKSHOP TOOLS mode (-tools -dx11), netconport $Port, ${Width}x${Height} windowed..." -ForegroundColor Cyan
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.UseShellExecute = $false
foreach ($key in $launchEnv.Keys) { $psi.EnvironmentVariables[$key] = [string]$launchEnv[$key] }

if ($nativeHost) {
    $psi.FileName = $nativeHost
    $psi.WorkingDirectory = (Split-Path -Parent $cs2exe)
    $argList = @('--cs2', $cs2exe, '--hook', $dll, '--injector', $injector, '--cmd-line', $cmdLine)
} else {
    $psi.FileName = $launcher
    $psi.WorkingDirectory = $bin
    $argList = @('--headless', '--hook', $dll, '--injector', $injector, '--cs2', $cs2exe, '--cmd-line', $cmdLine)
    foreach ($key in $launchEnv.Keys) { $argList += @('--env', ("$key=" + $launchEnv[$key])) }
}

# PowerShell 5.1 (.NET Framework) has no ProcessStartInfo.ArgumentList; reading it under
# StrictMode throws before a fallback could run, so probe the property by reflection first.
$argumentListProperty = [System.Diagnostics.ProcessStartInfo].GetProperty('ArgumentList')
if ($null -ne $argumentListProperty) {
    foreach ($a in $argList) { $psi.ArgumentList.Add([string]$a) }
} else {
    $quote = { param($s) if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }
    $psi.Arguments = (($argList | ForEach-Object { & $quote ([string]$_) }) -join ' ')
}
[void][System.Diagnostics.Process]::Start($psi)

if ($NoWait) {
    Write-Host "Launch started without readiness wait. Artifacts: $OutDir"
    exit 0
}

# Tools mode builds tool UI on startup, so allow a longer default than the normal client path.
Write-Host "Waiting for CS2 window and netcon readiness (tools startup is slower)..."
$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
while ((Get-Date) -lt $deadline) {
    $cs2 = Get-Process -Name cs2 -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 -and $_.Responding } |
        Select-Object -First 1
    if ($cs2) {
        try {
            $probe = [System.Net.Sockets.TcpClient]::new()
            $connect = $probe.BeginConnect('127.0.0.1', $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(500)) { $probe.EndConnect($connect); $ready = $true }
            $probe.Dispose()
        } catch { $ready = $false }
    }
    if ($ready) { break }
    Start-Sleep -Seconds 1
}
if (-not $ready) {
    throw "CS2 did not expose a responsive window and netcon port within $ReadyTimeoutSeconds seconds. Artifacts: $OutDir"
}

$windowMover = Join-Path $automationRoot 'tools\move-process-window-to-monitor.ps1'
if (Test-Path -LiteralPath $windowMover) {
    try {
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $windowMover `
            -ProcessName 'cs2' -Monitor $Monitor -TimeoutSeconds 12
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not move CS2 to monitor $Monitor (exit $LASTEXITCODE)." }
    } catch { Write-Warning "Could not move CS2 to monitor ${Monitor}: $($_.Exception.Message)" }
}

if ($SettleSeconds -gt 0) {
    Write-Host "CS2 is responsive; allowing $SettleSeconds seconds to settle..."
    Start-Sleep -Seconds $SettleSeconds
}

# ---- Evidence that tools mode actually engaged ------------------------------------------------
# A window plus an open netcon port only proves CS2 started. Record what the engine reports so a
# failed tools launch is visible in the run folder instead of being discovered three steps later.
$probeLog = Join-Path $OutDir 'tools-probe.txt'
try {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    $client.NoDelay = $true
    $stream = $client.GetStream()
    $buffer = New-Object byte[] 32768
    $sb = New-Object System.Text.StringBuilder
    function Invoke-Probe([string]$cmd, [double]$seconds) {
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($cmd + "`n")
        $stream.Write($bytes, 0, $bytes.Length); $stream.Flush()
        [void]$sb.AppendLine("===== $cmd")
        $stop = (Get-Date).AddSeconds($seconds)
        while ((Get-Date) -lt $stop) {
            if ($stream.DataAvailable) {
                $read = $stream.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) { break }
                [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
            } else { Start-Sleep -Milliseconds 50 }
        }
        [void]$sb.AppendLine()
    }
    Start-Sleep -Milliseconds 300
    Invoke-Probe 'mirv_filmmaker health' 1.5
    # mat_print_modes dumps the material system's render-mode table. In tools mode this is where
    # ToolsVis / ToolsWireframe / ToolsUtil should appear; it is the whole reason for tools mode.
    Invoke-Probe 'mat_print_modes' 2.5
    $sb.ToString() | Set-Content -LiteralPath $probeLog -Encoding UTF8
    $client.Dispose()
    Write-Host "Engine probe written to: $probeLog" -ForegroundColor Green
} catch {
    Write-Warning "Tools probe failed (CS2 is up, but the probe did not complete): $($_.Exception.Message)"
}

Write-Host "CS2 WORKSHOP TOOLS mode ready at ${Width}x${Height} windowed on netcon $Port." -ForegroundColor Green
Write-Host "Artifacts: $OutDir"
