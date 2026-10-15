<#
.SYNOPSIS
  Launches Deadlock with AfxHookDeadlock injected, and waits until the hook answers.

.DESCRIPTION
  The Deadlock counterpart of Automation/launch/launch-cs2-netcon.ps1. The injection chain is
  reused verbatim: SourceMovieMakerHost.exe creates the process SUSPENDED, drives injector.exe over
  stdin, then resumes. Neither the host nor the injector knows anything about which game it is
  launching -- the injector takes only { ProcessId, DllPath } -- so the only Deadlock-specific
  parts here are the executable path, the Steam app id and the command line.

  Readiness is proven by the hook ANSWERING, not by the port being open: the script writes
  `mirv_deadlock health` and waits for that banner to come back. An open port means the listener
  started; the banner means the DLL registered its commands.

.NOTES
  Run this as a background task using the tool's completion/session mechanism. Read the exit
  code and output, diagnose launch failures, then continue through the requested verification.
  Use bounded waits when needed; avoid busy polling. Launch completion alone is not task completion.

  Close Deadlock with a console `quit`, never taskkill /F. A force-kill breaks the Steam pipe and
  the next launch comes up as a black window that loads no map while still reporting healthy.
#>
[CmdletBinding()]
param(
    [string]$DeadlockDir = '',
    [int]$Port = 29020,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$CustomLaunchOptions = '',
    [int]$ReadyTimeoutSeconds = 90,
    [int]$SettleSeconds = 2,
    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not $DeadlockDir) {
    $DeadlockDir = & (Join-Path $repoRoot 'Automation\lib\Resolve-DeadlockDir.ps1')
}
if (-not $DeadlockDir) {
    throw "Deadlock not found. Set SOURCEMVM_DEADLOCK_DIR or pass -DeadlockDir."
}

$deadlockExe = Join-Path $DeadlockDir 'game\bin\win64\deadlock.exe'
if (-not (Test-Path -LiteralPath $deadlockExe)) {
    throw "deadlock.exe not found at $deadlockExe"
}

# --- staged payload -------------------------------------------------------------------------

$hookDll = Join-Path $repoRoot 'build\x64-deadlock-release\AfxHookSource2\Deadlock\Release\AfxHookDeadlock.dll'
if (-not (Test-Path -LiteralPath $hookDll)) {
    throw @"
AfxHookDeadlock.dll not built. Run:
  cmake --preset x64-deadlock-release
  cmake --build build/x64-deadlock-release --config Release --target AfxHookDeadlock --parallel
"@
}

# The injector and the host come from the CS2 staging prefix: both are game-agnostic, so there is
# no Deadlock-specific copy of either and there should never be one.
$injector = $null
foreach ($candidate in @(
    (Join-Path $repoRoot 'build\source2-release\bin\x64\injector.exe'),
    (Join-Path $repoRoot 'build\staging-release\bin\x64\injector.exe'),
    (Join-Path $repoRoot 'build\portable-runtime\bin\x64\injector.exe')
)) {
    if (Test-Path -LiteralPath $candidate) { $injector = $candidate; break }
}
if (-not $injector) {
    throw "injector.exe not staged. Build the CS2 stage once (build.bat -NoLaunch) to get it."
}

$host_exe = $null
foreach ($candidate in @(
    (Join-Path $repoRoot 'SourceMovieMakerLauncher\host\build\SourceMovieMakerHost.exe'),
    (Join-Path $repoRoot 'SourceMovieMakerLauncher\resources\host\SourceMovieMakerHost.exe')
)) {
    if (Test-Path -LiteralPath $candidate) { $host_exe = $candidate; break }
}
if (-not $host_exe) {
    throw "SourceMovieMakerHost.exe not built. Run SourceMovieMakerLauncher\scripts\build-host.ps1."
}

# --- Steam contract -------------------------------------------------------------------------

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

# --- command line ---------------------------------------------------------------------------

# -insecure is mandatory: the hook refuses to load without it. Never -vulkan: every render hook in
# the DLL is a D3D11 hook, so Vulkan is not a degraded mode, it is no mode at all.
#
# -dev is here as well as -insecure. Deadlock's gameinfo.gi sets Engine2 { DefensiveConCommands 1 },
# which is why some tools ship a gameinfo unlocker; -dev is the flag that relaxes it
# without touching the install, and the tool drives the console for everything.
$commandLine = "-steam -insecure -dev -console -windowed -sw -w $Width -h $Height -mvmnetconport $Port"
if ($CustomLaunchOptions) { $commandLine += " $CustomLaunchOptions" }

Write-Host "Deadlock:  $deadlockExe"
Write-Host "hook:      $hookDll"
Write-Host "cmdline:   $commandLine"

if (-not $NoLaunch) {
    $existingGame = Get-Process -Name deadlock -ErrorAction SilentlyContinue
    if ($existingGame) {
        throw "Deadlock is already running (PID $($existingGame.Id -join ', ')). Close it with console quit before launching another instance."
    }

    # WAIT FOR THE PORT, not just for the process.
    #
    # Closing the process is not enough: the old listener can still own 29020 for a moment after
    # exit, the new instance then fails to bind, and every command sent afterwards is answered by
    # a game that is on its way out. That is not a hypothetical -- it cost an evening. The
    # symptom is maximally misleading, because the port IS open and the hook DOES reply, just
    # from the wrong process.
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
    if ($stillHeld) {
        throw "Port $Port is still held by pid $stillHeld. Close it before launching, or pass a different -Port."
    }

    $env:SteamPath = $steamPath
    $env:SteamClientLaunch = '1'
    $env:SteamGameId = '1422450'
    $env:SteamAppId = '1422450'
    $env:SteamOverlayGameId = '1422450'

    # EVERY value is quoted, not just the obviously long ones. Start-Process -ArgumentList joins an
    # array with spaces and quotes nothing, so an unquoted 'C:\Program Files (x86)\Steam' arrives at
    # the host as two arguments and it reports "Steam install directory was not found: C:\Program".
    # The repository path and the Deadlock path both contain spaces here too, so this is not a
    # theoretical concern -- it is the first thing that broke.
    function Quote([string]$value) { return '"' + ($value -replace '"', '\"') + '"' }

    $hostArgs = @(
        '--cs2', (Quote $deadlockExe),
        '--hook', (Quote $hookDll),
        '--injector', (Quote $injector),
        '--steam-path', (Quote $steamPath),
        # Without this the host tells Steam the child is app 730, and Deadlock exits
        # immediately and silently -- which looks exactly like a crashing hook.
        '--steam-appid', '1422450',
        '--cmd-line', (Quote $commandLine),
        '--settle-ms', ([string]($SettleSeconds * 1000))
    )
    # The host's --cs2 switch is a path parameter, not a game selector: it names the executable to
    # create suspended. Nothing about it is CS2-specific beyond the switch name.
    Write-Host "Starting via $host_exe ..."
    # Start-Process -Wait waits for descendants too, including the game. Wait on the
    # host process itself so the readiness probe can run while Deadlock is alive.
    $hostLogDir = Join-Path $repoRoot 'Automation\runs\deadlock-launch'
    New-Item -ItemType Directory -Force -Path $hostLogDir | Out-Null
    $hostLogStem = Join-Path $hostLogDir ([Guid]::NewGuid().ToString('N'))
    $proc = Start-Process -FilePath $host_exe -ArgumentList $hostArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($hostLogStem + '.stdout.log') -RedirectStandardError ($hostLogStem + '.stderr.log')
    # Retain the handle before the short-lived host exits so PowerShell can read ExitCode.
    $hostProcessHandle = $proc.Handle
    if (-not $proc.WaitForExit(30000)) { throw "Injection host did not finish within 30 seconds. See $hostLogStem.*.log." }
    Get-Content -LiteralPath ($hostLogStem + '.stdout.log')
    Get-Content -LiteralPath ($hostLogStem + '.stderr.log')
    if ($proc.ExitCode -ne 0) {
        throw "SourceMovieMakerHost failed with exit code $($proc.ExitCode)."
    }
}

# --- readiness ------------------------------------------------------------------------------

$deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
$ready = $false
$sawBanner = $false

# ONE CONNECTION, HELD OPEN, and a read window long enough for a round trip.
#
# The first version of this loop opened a fresh socket per attempt, sent `mirv_deadlock health`,
# read for a second and closed. That cannot work, and the way it fails is instructive: a command
# arriving over netcon is queued and executed by the MAIN-THREAD PUMP, so the reply cannot appear
# until the game reaches its next frame. During startup that is well over a second away -- so every
# attempt had already hung up by the time its own command ran, and the output went to zero clients.
# The boot trace showed exactly that: "client accepted" four times, then "exec ... -> ok" once the
# probe was gone.
#
# So: connect once, keep the socket, re-send periodically, and keep reading the whole time.
while ((Get-Date) -lt $deadline -and -not (Get-Process -Name deadlock -ErrorAction SilentlyContinue)) {
    Start-Sleep -Milliseconds 500
}
if (-not (Get-Process -Name deadlock -ErrorAction SilentlyContinue)) {
    Write-Host "[FAIL] deadlock.exe never appeared." -ForegroundColor Red
    Write-Host "       Read %TEMP%\afxhookdeadlock_boot.log -- a process that dies during startup" -ForegroundColor Red
    Write-Host "       leaves no dump and no event-log entry." -ForegroundColor Red
    exit 1
}

$client = $null
while ((Get-Date) -lt $deadline -and -not $client) {
    $attempt = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $attempt.BeginConnect('127.0.0.1', $Port, $null, $null)
        if ($async.AsyncWaitHandle.WaitOne(500)) {
            $attempt.EndConnect($async)
            $attempt.NoDelay = $true
            $client = $attempt
            $sawBanner = $true
        } else {
            $attempt.Close()
        }
    } catch {
        try { $attempt.Close() } catch { }
    }
    if (-not $client) { Start-Sleep -Milliseconds 500 }
}

if ($client) {
    $stream = $client.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    $buffer = New-Object byte[] 65536
    $text = ''
    $lastSend = (Get-Date).AddSeconds(-10)

    while ((Get-Date) -lt $deadline -and -not $ready) {
        # Re-ask every second and a half. Harmless: health is read-only.
        if (((Get-Date) - $lastSend).TotalMilliseconds -gt 1500) {
            try { $writer.Write("mirv_deadlock health`n") } catch { break }
            $lastSend = Get-Date
        }
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) {
                $text += [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            }
        }
        if ($text -match 'AfxHookDeadlock health') { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }

    if ($ready) { Write-Host $text.TrimEnd() }
    try { $writer.Dispose(); $stream.Dispose() } catch { }
    $client.Close()
}

if (-not $ready) {
    if ($sawBanner) {
        Write-Host "[FAIL] the hook is loaded and listening on $Port but never answered." -ForegroundColor Red
        Write-Host "       The main-thread pump executes received commands, and it is driven from" -ForegroundColor Red
        Write-Host "       the view hook -- so check 'mirv_deadlock view' and the boot trace at" -ForegroundColor Red
        Write-Host "       %TEMP%\afxhookdeadlock_boot.log." -ForegroundColor Red
    } else {
        Write-Host "[FAIL] nothing is listening on 127.0.0.1:$Port." -ForegroundColor Red
        Write-Host "       Read %TEMP%\afxhookdeadlock_boot.log -- it records how far startup got." -ForegroundColor Red
    }
    exit 1
}

Write-Host "[OK] Deadlock is up and the hook is answering on port $Port." -ForegroundColor Green
exit 0
