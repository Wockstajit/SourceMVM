# Launches CS2 with SOURCE:MVM, opens a demo, verifies that the hook can see it,
# and writes session.json for Codex/Claude and other automation clients.
#
# Examples:
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-demo.ps1
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-demo.ps1 -Demo "replays\match.dem"
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-demo.ps1 -Demo "F:\demos\match.dem"
#
# With no -Demo, the newest .dem in game\csgo\replays is opened.
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Demo = '',
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [int]$Width = 854,
    [int]$Height = 480,
    [ValidateRange(0, 16)]
    [int]$Monitor = 3,
    [int]$ReadyTimeoutSeconds = 75,
    [int]$SettleSeconds = 6,
    [int]$DemoReadyTimeoutSeconds = 150,   # a 340 MB match demo takes well over 45 s to load
    [ValidateRange(10, 1000)]
    [int]$ReadPollMilliseconds = 50,
    [string]$FxAssetsGameDir = '',
    [string]$OutDir,
    [switch]$NoLaunch,
    [switch]$FreeMouse,
    # live.bat: launch from whichever checkout of this repository has the newest hook build
    # (Invoke-NewestHookCheckout). Automation leaves it off and launches its own checkout.
    [switch]$NewestHook
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ($NewestHook) {
    $handedOff = Invoke-NewestHookCheckout -RepositoryRoot $repositoryRoot -ScriptName 'launch-demo.ps1' -BoundParameters $PSBoundParameters
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
    $OutDir = New-AutomationRunFolder -Name 'launch-demo'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $OutDir = (Resolve-Path -LiteralPath $OutDir).Path
}

$sessionPath = Join-Path $OutDir 'session.json'
$session = [ordered]@{
    schemaVersion = 1
    automation = 'launch-demo'
    state = if ($NoLaunch) { 'connecting' } else { 'launching' }
    startedAt = (Get-Date).ToString('o')
    updatedAt = (Get-Date).ToString('o')
    readyAt = $null
    demoInput = $Demo
    demoPath = ''
    playdemoArgument = ''
    monitor = $Monitor
    hookHealthy = $false
    hookHealthOutput = ''
    demoProbeOutput = ''
    netcon = [ordered]@{
        host = '127.0.0.1'
        port = $Port
    }
    runDirectory = $OutDir
    error = $null
}

function Save-Session {
    $session.updatedAt = (Get-Date).ToString('o')
    $json = $session | ConvertTo-Json -Depth 8
    $utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($sessionPath, $json, $utf8WithoutBom)
}

function Resolve-DemoFile {
    param([string]$InputPath)

    $csgoDirectory = Join-Path $Cs2Dir 'game\csgo'
    $replayDirectory = Join-Path $csgoDirectory 'replays'

    if ([string]::IsNullOrWhiteSpace($InputPath)) {
        $newest = Get-ChildItem -LiteralPath $replayDirectory -Filter '*.dem' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        if (-not $newest) {
            throw "No demos found under '$replayDirectory'. Pass -Demo with a .dem path."
        }
        return $newest.FullName
    }

    $candidates = New-Object System.Collections.Generic.List[string]
    [void]$candidates.Add($InputPath)
    if (-not [System.IO.Path]::IsPathRooted($InputPath)) {
        [void]$candidates.Add((Join-Path $repositoryRoot $InputPath))
        [void]$candidates.Add((Join-Path $csgoDirectory $InputPath))
        $replayRelative = $InputPath -replace '^[Rr]eplays[\\/]', ''
        [void]$candidates.Add((Join-Path $replayDirectory $replayRelative))
    }

    foreach ($candidate in $candidates) {
        $withExtension = if ([System.IO.Path]::GetExtension($candidate) -ieq '.dem') {
            $candidate
        } else {
            "$candidate.dem"
        }
        if (Test-Path -LiteralPath $withExtension -PathType Leaf) {
            return (Resolve-Path -LiteralPath $withExtension).Path
        }
    }

    throw "Demo '$InputPath' was not found. Checked the supplied path, the repository, game\csgo, and game\csgo\replays."
}

function Get-PlaydemoArgument {
    param([string]$DemoFile)

    $csgoDirectory = [System.IO.Path]::GetFullPath((Join-Path $Cs2Dir 'game\csgo')).TrimEnd('\', '/')
    $fullDemo = [System.IO.Path]::GetFullPath($DemoFile)
    $prefix = $csgoDirectory + [System.IO.Path]::DirectorySeparatorChar
    if ($fullDemo.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        $argument = $fullDemo.Substring($prefix.Length)
    } else {
        $argument = $fullDemo
    }
    if ($argument.EndsWith('.dem', [System.StringComparison]::OrdinalIgnoreCase)) {
        $argument = $argument.Substring(0, $argument.Length - 4)
    }
    return ($argument -replace '\\', '/')
}

function Quote-ConsoleArgument {
    param([string]$Value)
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Connect-Netcon {
    param([datetime]$Deadline)

    while ((Get-Date) -lt $Deadline) {
        $candidate = New-Object System.Net.Sockets.TcpClient
        try {
            $connect = $candidate.BeginConnect('127.0.0.1', $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(500)) {
                $candidate.EndConnect($connect)
                $candidate.NoDelay = $true
                return $candidate
            }
        } catch {
            # CS2 can expose the process/window before netcon starts listening.
        }
        $candidate.Dispose()
        Start-Sleep -Milliseconds 200
    }
    throw "Could not connect to CS2 netcon at 127.0.0.1:$Port."
}

function Read-Netcon {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [double]$Seconds
    )

    $deadline = (Get-Date).AddSeconds($Seconds)
    $buffer = New-Object byte[] 16384
    $builder = New-Object System.Text.StringBuilder
    while ((Get-Date) -lt $deadline) {
        if ($Stream.DataAvailable) {
            $read = $Stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            [void]$builder.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
        } else {
            Start-Sleep -Milliseconds $ReadPollMilliseconds
        }
    }
    return $builder.ToString()
}

function Send-Netcon {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Command,
        [double]$ReadSeconds = 0.5
    )

    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`n")
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
    return (Read-Netcon -Stream $Stream -Seconds $ReadSeconds)
}

Save-Session
$client = $null
try {
    $demoFile = Resolve-DemoFile -InputPath $Demo
    $playdemoArgument = Get-PlaydemoArgument -DemoFile $demoFile
    $session.demoPath = $demoFile
    $session.playdemoArgument = $playdemoArgument
    Save-Session

    if (-not $NoLaunch) {
        $launchOutput = Join-Path $OutDir 'launch'
        $launchArguments = @(
            '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', (Join-Path $PSScriptRoot 'launch-cs2-netcon.ps1'),
            '-Port', $Port,
            '-Cs2Dir', $Cs2Dir,
            '-Width', $Width,
            '-Height', $Height,
            '-Monitor', $Monitor,
            '-ReadyTimeoutSeconds', $ReadyTimeoutSeconds,
            '-SettleSeconds', $SettleSeconds,
            '-OutDir', $launchOutput
        )
        if ($FreeMouse) {
            $launchArguments += '-FreeMouse'
        }
        if (-not [string]::IsNullOrWhiteSpace($FxAssetsGameDir)) {
            $launchArguments += @('-FxAssetsGameDir', $FxAssetsGameDir)
        }

        & powershell.exe @launchArguments
        if ($LASTEXITCODE -ne 0) {
            throw "launch-cs2-netcon.ps1 failed with exit code $LASTEXITCODE."
        }
    }

    $session.state = 'loading-demo'
    Save-Session

    $client = Connect-Netcon -Deadline (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    $stream = $client.GetStream()
    [void](Read-Netcon -Stream $stream -Seconds 0.2)
    if ($FreeMouse) {
        # Click-to-capture is the hook's DEFAULT now, so every session already leaves the desktop
        # cursor alone until you click into the game. This switch asks for something stronger:
        # NEVER capture, not even on a game click -- which is what an unattended automation run
        # wants, since a stray click must not steal the operator's mouse.
        [void](Send-Netcon -Stream $stream -Command 'mirv_filmmaker mouse free' -ReadSeconds 0.5)
    }

    $playCommand = 'playdemo ' + (Quote-ConsoleArgument -Value $playdemoArgument)
    [void](Send-Netcon -Stream $stream -Command $playCommand -ReadSeconds 0.5)

    $expectedLeaf = [System.IO.Path]::GetFileNameWithoutExtension($demoFile)
    $demoDeadline = (Get-Date).AddSeconds($DemoReadyTimeoutSeconds)
    $lastProbe = ''
    # The first playdemo is sent the moment netcon answers, which can be before the main menu
    # accepts console commands; it is then dropped silently and this loop used to wait out the
    # whole timeout probing for a demo it never asked for again. Re-send while nothing has started
    # loading (IsPlayingDemo still 0); once loading begins, only wait.
    $nextResend = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $demoDeadline) {
        $lastProbe = Send-Netcon -Stream $stream -Command 'mirv_filmmaker demoprobe' -ReadSeconds 0.75
        if (($lastProbe -notmatch 'IsPlayingDemo=1') -and ((Get-Date) -ge $nextResend)) {
            Write-Host "Re-sending: $playCommand"
            [void](Send-Netcon -Stream $stream -Command $playCommand -ReadSeconds 0.5)
            $nextResend = (Get-Date).AddSeconds(10)
        }
        $session.demoProbeOutput = $lastProbe.Trim()
        Save-Session

        $isPlaying = $lastProbe -match 'IsPlayingDemo=1'
        $resolvedMatch = [regex]::Match($lastProbe, "(?m)^\[demoprobe\] RESOLVED = '(.+)'")
        $resolvedLeaf = if ($resolvedMatch.Success) {
            [System.IO.Path]::GetFileNameWithoutExtension($resolvedMatch.Groups[1].Value)
        } else {
            ''
        }
        # IsPlayingDemo + a resolved path become true before the map's entity/class tables
        # finish loading. Seeking or issuing demo controls at that point can make CS2 process
        # a network entity with an empty class list (CopyNewEntity: invalid class index).
        $levelMatch = [regex]::Match($lastProbe, "(?m)^\[demoprobe\] level = '([^']*)'")
        $levelReady = $levelMatch.Success -and -not [string]::IsNullOrWhiteSpace($levelMatch.Groups[1].Value)
        if ($isPlaying -and $resolvedLeaf -ieq $expectedLeaf -and $levelReady) {
            $healthOutput = Send-Netcon -Stream $stream -Command 'mirv_filmmaker health' -ReadSeconds 0.75
            $session.hookHealthOutput = $healthOutput.Trim()
            $session.hookHealthy = $healthOutput -match '=>\s+healthy:'
            $session.state = 'ready'
            $session.readyAt = (Get-Date).ToString('o')
            Save-Session
            Write-Host "Demo ready: $demoFile" -ForegroundColor Green
            Write-Host "Netcon: 127.0.0.1:$Port"
            Write-Host "Session: $sessionPath"
            exit 0
        }
        Start-Sleep -Milliseconds 250
    }

    throw "CS2 did not report '$expectedLeaf' as the playing demo within $DemoReadyTimeoutSeconds seconds. Last probe: $($lastProbe.Trim())"
} catch {
    $session.state = 'failed'
    $session.error = $_.Exception.Message
    Save-Session
    Write-Error "$($_.Exception.Message) Session: $sessionPath"
    exit 1
} finally {
    if ($client) { $client.Dispose() }
}
