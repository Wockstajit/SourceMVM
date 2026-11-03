<#
.SYNOPSIS
  Launches Counter-Strike: Source (64-bit) with AfxHookSource injected.

.DESCRIPTION
  The CS:S counterpart of launch-deadlock.ps1, and the same injection chain:
  SourceMovieMakerHost.exe creates cstrike_win64.exe suspended, drives injector.exe, then resumes.
  Both are game-agnostic, so they come from the CS2 stage.

  The game runs offline (-insecure), windowed at 854x480 and is moved to Windows DISPLAY3, the
  routine test setup. D3D9 only: never -vulkan, because every render hook in the DLL is a D3D9
  hook. -condebug mirrors the console into cstrike\console.log for diagnosis.

  The engine has no -netconport, so the hook serves the command port itself (-mvmnetconport, port
  29030). Readiness is the hook ANSWERING `mirv_filmmaker health` over that port, not the port
  being open: an open port proves the listener started, the reply proves the commands work.

  A startup error from the hook is a message box titled "Error - AfxHookSource". The script reports
  any that appear, because the game sits blocked behind one.

.NOTES
  Run this as a background task. Close CS:S with the console `quit` command, never by killing the
  process: a force-kill can break the Steam pipe for the next launch.
#>
[CmdletBinding()]
param(
    [string]$CssDir = '',
    [int]$Port = 29030,
    [int]$Width = 854,
    [int]$Height = 480,
    [ValidateRange(0, 16)]
    [int]$Monitor = 3,
    [string]$CustomLaunchOptions = '',
    [int]$ReadyTimeoutSeconds = 90,
    [int]$SettleSeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$automationRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $automationRoot

if (-not $CssDir) { $CssDir = & (Join-Path $automationRoot 'lib\Resolve-CssDir.ps1') }
if (-not $CssDir) { throw 'Counter-Strike: Source not found. Set SOURCEMVM_CSS_DIR or pass -CssDir.' }

$cssExe = Join-Path $CssDir 'cstrike_win64.exe'
if (-not (Test-Path -LiteralPath $cssExe)) { throw "cstrike_win64.exe not found at $cssExe" }

# --- staged payload -------------------------------------------------------------------------

$hookDll = Join-Path $repoRoot 'build\source1-release\bin\x64\AfxHookSource.dll'
if (-not (Test-Path -LiteralPath $hookDll)) {
    throw @"
AfxHookSource.dll is not staged. Run:
  cmake --preset x64-release
  cmake --build build/x64-release --config Release --target AfxHookSource --parallel
  cmake --install build/x64-release --config Release --prefix build/source1-release --component x64-runtime
  cmake --install build/x64-release --config Release --prefix build/source1-release --component x64-source1
"@
}

# The Render workspace encodes through the stage's ffmpeg (AutomationCommon.ps1).
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
Initialize-StagedFfmpeg -StageBin (Join-Path $repoRoot 'build\source1-release\bin') -RepositoryRoot $repoRoot

$injector = $null
foreach ($candidate in @(
    (Join-Path $repoRoot 'build\source2-release\bin\x64\injector.exe'),
    (Join-Path $repoRoot 'build\staging-release\bin\x64\injector.exe'),
    (Join-Path $repoRoot 'build\portable-runtime\bin\x64\injector.exe')
)) {
    if (Test-Path -LiteralPath $candidate) { $injector = $candidate; break }
}
if (-not $injector) { throw 'injector.exe not staged. Build the CS2 stage once (build.bat -NoLaunch) to get it.' }

$hostExe = $null
foreach ($candidate in @(
    (Join-Path $repoRoot 'SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe'),
    (Join-Path $repoRoot 'SourceMovieMakerLauncher\resources\host\SourceMovieMakerHost.exe')
)) {
    if (Test-Path -LiteralPath $candidate) { $hostExe = $candidate; break }
}
if (-not $hostExe) { throw 'SourceMovieMakerHost.exe not built. Run SourceMovieMakerLauncher\scripts\build-host.ps1.' }

$steamPath = ''
foreach ($entry in @(
    @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Value = 'InstallPath' },
    @{ Path = 'HKCU:\SOFTWARE\Valve\Steam'; Value = 'SteamPath' }
)) {
    try {
        $found = (Get-ItemProperty -Path $entry.Path -Name $entry.Value -ErrorAction Stop).($entry.Value)
        if ($found) { $steamPath = ($found -replace '/', '\'); break }
    } catch { }
}
if (-not $steamPath) { $steamPath = 'C:\Program Files (x86)\Steam' }

# --- launch ---------------------------------------------------------------------------------

$commandLine = "-steam -game cstrike -insecure -console -condebug -novid -windowed -w $Width -h $Height -mvmnetconport $Port"
if ($CustomLaunchOptions) { $commandLine += " $CustomLaunchOptions" }

Write-Host "CS:S:      $cssExe"
Write-Host "hook:      $hookDll"
Write-Host "cmdline:   $commandLine"

$existing = Get-Process -Name cstrike_win64 -ErrorAction SilentlyContinue
if ($existing) {
    throw "CS:S is already running (PID $($existing.Id -join ', ')). Close it with console quit before launching another instance."
}

# A closed game can hold its port for a moment, and the new hook then fails to bind while the old
# process still answers. Wait for the port to be free rather than talk to the wrong game.
$portDeadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $portDeadline) {
    $owner = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
             Select-Object -First 1 -ExpandProperty OwningProcess
    if (-not $owner) { break }
    Write-Host "Port $Port is still held by pid $owner; waiting..." -ForegroundColor Yellow
    Start-Sleep -Milliseconds 750
}
$stillHeld = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
             Select-Object -First 1 -ExpandProperty OwningProcess
if ($stillHeld) { throw "Port $Port is still held by pid $stillHeld. Close it before launching, or pass a different -Port." }

$env:SteamPath = $steamPath
$env:SteamClientLaunch = '1'
$env:SteamGameId = '240'
$env:SteamAppId = '240'
$env:SteamOverlayGameId = '240'

# Start-Process joins -ArgumentList with spaces and quotes nothing, so every value is quoted.
function Quote([string]$value) { return '"' + ($value -replace '"', '\"') + '"' }

$hostArgs = @(
    '--cs2', (Quote $cssExe),
    '--hook', (Quote $hookDll),
    '--injector', (Quote $injector),
    '--steam-path', (Quote $steamPath),
    '--steam-appid', '240',
    '--cmd-line', (Quote $commandLine),
    '--settle-ms', ([string]($SettleSeconds * 1000))
)
$logDir = Join-Path $repoRoot 'Automation\runs\css-launch'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logStem = Join-Path $logDir ([Guid]::NewGuid().ToString('N'))
$proc = Start-Process -FilePath $hostExe -ArgumentList $hostArgs -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput ($logStem + '.stdout.log') -RedirectStandardError ($logStem + '.stderr.log')
$null = $proc.Handle
if (-not $proc.WaitForExit(30000)) { throw "Injection host did not finish within 30 seconds. See $logStem.*.log." }
Get-Content -LiteralPath ($logStem + '.stdout.log')
Get-Content -LiteralPath ($logStem + '.stderr.log')
if ($proc.ExitCode -ne 0) { throw "SourceMovieMakerHost failed with exit code $($proc.ExitCode)." }

# --- readiness ------------------------------------------------------------------------------

if (-not ('MvmCssLaunch.Native' -as [type])) {
    Add-Type -Namespace MvmCssLaunch -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetDlgItemTextW(IntPtr dlg, int id, System.Text.StringBuilder text, int max);
'@
}

function Get-HookErrorText {
    $box = [MvmCssLaunch.Native]::FindWindowW('#32770', 'Error - AfxHookSource')
    if ($box -eq [IntPtr]::Zero) { return $null }
    $text = New-Object System.Text.StringBuilder 1024
    # 0xFFFF is the static text control of a MessageBox.
    [void][MvmCssLaunch.Native]::GetDlgItemTextW($box, 0xFFFF, $text, $text.Capacity)
    return $text.ToString()
}

$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$game = $null
while ((Get-Date) -lt $deadline) {
    $errorText = Get-HookErrorText
    if ($errorText) {
        Write-Host "[FAIL] AfxHookSource reported: $errorText" -ForegroundColor Red
        exit 2
    }
    $game = Get-Process -Name cstrike_win64 -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($game -and $game.Responding) { break }
    $game = $null
    Start-Sleep -Milliseconds 500
}
if (-not $game) {
    if (-not (Get-Process -Name cstrike_win64 -ErrorAction SilentlyContinue)) {
        Write-Host '[FAIL] cstrike_win64.exe exited during startup.' -ForegroundColor Red
    } else {
        Write-Host "[FAIL] CS:S showed no responsive window within $ReadyTimeoutSeconds seconds." -ForegroundColor Red
    }
    exit 1
}

$windowMover = Join-Path $automationRoot 'tools\move-process-window-to-monitor.ps1'
if (Test-Path -LiteralPath $windowMover) {
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $windowMover `
        -ProcessName 'cstrike_win64' -Monitor $Monitor -TimeoutSeconds 12
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not move CS:S to monitor $Monitor (exit $LASTEXITCODE)." }
}

# One connection, held open, re-asking now and then. The command only runs when the main thread
# next drains the queue, so a probe that hangs up after a second can miss its own reply.
$client = $null
while ((Get-Date) -lt $deadline -and -not $client) {
    $attempt = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $attempt.BeginConnect('127.0.0.1', $Port, $null, $null)
        if ($async.AsyncWaitHandle.WaitOne(500)) {
            $attempt.EndConnect($async)
            $attempt.NoDelay = $true
            $client = $attempt
        } else {
            $attempt.Close()
        }
    } catch {
        try { $attempt.Close() } catch { }
    }
    if (-not $client) { Start-Sleep -Milliseconds 500 }
}
if (-not $client) {
    Write-Host "[FAIL] Nothing is listening on 127.0.0.1:$Port. Check the hook banner in cstrike\console.log." -ForegroundColor Red
    exit 1
}

$ready = $false
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$buffer = New-Object byte[] 65536
$text = ''
$lastSend = (Get-Date).AddSeconds(-10)
while ((Get-Date) -lt $deadline -and -not $ready) {
    if (((Get-Date) - $lastSend).TotalMilliseconds -gt 1500) {
        try { $writer.Write("mirv_filmmaker health`n") } catch { break }
        $lastSend = Get-Date
    }
    if ($stream.DataAvailable) {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -gt 0) { $text += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read) }
    }
    if ($text -match 'AfxHookSource health[\s\S]*state:') { $ready = $true; break }
    Start-Sleep -Milliseconds 100
}
try { $writer.Dispose(); $stream.Dispose() } catch { }
$client.Close()

if (-not $ready) {
    Write-Host "[FAIL] The hook is listening on $Port but never answered mirv_filmmaker health." -ForegroundColor Red
    Write-Host "       Commands run from the hook's window procedure; check 'pumps' once it answers." -ForegroundColor Red
    exit 1
}

$healthStart = $text.IndexOf('AfxHookSource health')
Write-Host $text.Substring($healthStart).TrimEnd()
Write-Host "[OK] CS:S is up (PID $($game.Id)) and the hook answers on port $Port." -ForegroundColor Green
exit 0
