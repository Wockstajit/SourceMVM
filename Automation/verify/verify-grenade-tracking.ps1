#requires -Version 5
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [int]$DiscoveryTimeoutSeconds = 45,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-grenade-tracking'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-grenade-tracking' -Additional @{
    port = $Port
    discoveryTimeoutSeconds = $DiscoveryTimeoutSeconds
} | Out-Null

$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath (Join-Path $OutDir 'verification.log') -Force | Out-Null
    $transcriptStarted = $true
} catch {
    Write-Warning "Could not start transcript: $($_.Exception.Message)"
}

$client = [System.Net.Sockets.TcpClient]::new()
try {
    $client.Connect('127.0.0.1', $Port)
} catch {
    Write-Host "[FAIL] No CS2 netcon on 127.0.0.1:$Port." -ForegroundColor Red
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    exit 1
}
$client.NoDelay = $true
$stream = $client.GetStream()
$encoding = [Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $buffer = New-Object byte[] 16384
    $builder = [Text.StringBuilder]::new()
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) { [void]$builder.Append($encoding.GetString($buffer, 0, $read)) }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    return $builder.ToString()
}

function Send([string]$command, [double]$seconds = 0.7) {
    Drain 0.06 | Out-Null
    $bytes = $encoding.GetBytes($command + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    return Drain $seconds
}

function Read-FollowState {
    $text = Send 'mirv_filmmaker follow state64' 1.2
    $matches = [regex]::Matches(
        $text,
        [regex]::Escape('[followcam][state64]') + '\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)'
    )
    $parts = @{}
    $expected = 0
    $complete = New-Object System.Collections.Generic.List[string]
    foreach ($match in $matches) {
        $index = [int]$match.Groups[1].Value
        $total = [int]$match.Groups[2].Value
        if ($index -eq 1) {
            $parts = @{}
            $expected = $total
        }
        $parts[$index] = $match.Groups[3].Value
        if ($expected -gt 0 -and $index -eq $expected -and $parts.Count -eq $expected) {
            $complete.Add(((1..$expected | ForEach-Object { $parts[$_] }) -join ''))
        }
    }
    if ($complete.Count -eq 0) { return $null }
    try {
        $json = [Text.Encoding]::UTF8.GetString(
            [Convert]::FromBase64String($complete[$complete.Count - 1])
        )
        return ($json | ConvertFrom-Json)
    } catch {
        return $null
    }
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$name, [bool]$condition) {
    if ($condition) {
        Write-Host "[ PASS ] $name" -ForegroundColor Green
        $results.Add("PASS $name")
    } else {
        Write-Host "[ FAIL ] $name" -ForegroundColor Red
        $results.Add("FAIL $name")
    }
}

Write-Host '=== Grenade tracking verifier ===' -ForegroundColor Cyan
Send 'mirv_filmmaker editor on' | Out-Null
Send 'mirv_filmmaker follow place' | Out-Null
Send 'mirv_filmmaker follow type grenade' | Out-Null
Send "mirv_filmmaker editor eval `"`$.CamEditor && `$.CamEditor.setInspectorMode('follow');`"" | Out-Null
Send 'demo_timescale 4' | Out-Null
Send 'demo_resume' | Out-Null

$candidate = $null
$deadline = (Get-Date).AddSeconds($DiscoveryTimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $state = Read-FollowState
    if ($state -and @($state.candidates).Count -gt 0) {
        $candidate = @($state.candidates)[0]
        break
    }
}
Send 'demo_pause' | Out-Null
Send 'demo_timescale 1' | Out-Null

Check 'Nearby grenade was discovered within ±500 ticks' ($null -ne $candidate)
if ($candidate) {
    Check 'Grenade entry includes type' (-not [string]::IsNullOrWhiteSpace([string]$candidate.grenadeType))
    Check 'Grenade entry includes thrower' (-not [string]::IsNullOrWhiteSpace([string]$candidate.ownerName))
    Check 'Grenade entry includes throw tick' ([int]$candidate.throwTick -ge 0)
    Check 'Grenade entry includes status' (-not [string]::IsNullOrWhiteSpace([string]$candidate.status))

    Send ("mirv_filmmaker follow selecthandle {0}" -f $candidate.handle) | Out-Null
    $capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
    if (Test-Path $capture) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out (Join-Path $OutDir 'grenade-list.png') | Out-Null
    }

    $trackOutput = Send 'mirv_filmmaker follow trackgrenade' 1.2
    $armed = Read-FollowState
    Check 'Tracking seek starts 40 ticks before throw' (
        $armed -and [int]$armed.grenadeSeekTick -eq ([int]$armed.grenadeThrowTick - 40)
    )
    Check 'Grenade tracking emitted armed instrumentation' ($trackOutput -match '\[followcam\] grenade track armed:')

    $tracked = if ($armed -and $armed.enabled -and -not $armed.grenadePending -and $armed.targetValid) { $armed } else { $null }
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        Start-Sleep -Milliseconds 250
        $state = Read-FollowState
        if ($state -and $state.enabled -and -not $state.grenadePending -and $state.targetValid) {
            $tracked = $state
            break
        }
        if ($state -and -not $state.enabled -and -not $state.grenadePending) { break }
    }
    Check 'Selected grenade was reacquired after seek' ($null -ne $tracked)
    if ($tracked) {
        Check 'Follow Camera owns a valid grenade target' ($tracked.targetValid -and [int]$tracked.targetIndex -ge 0)
        if (Test-Path $capture) {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out (Join-Path $OutDir 'grenade-tracking.png') | Out-Null
        }
    } elseif ($state) {
        Write-Host ("      Runtime message: {0}" -f $state.message) -ForegroundColor DarkYellow
    }
}

Send 'mirv_filmmaker follow stop' | Out-Null
$client.Close()

$failures = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
Write-Host "Artifacts: $OutDir"
if ($transcriptStarted) { Stop-Transcript | Out-Null }
if ($failures -eq 0) {
    Write-Host "`nALL GRENADE TRACKING CHECKS PASSED" -ForegroundColor Green
    exit 0
}
Write-Host "`n$failures CHECK(S) FAILED" -ForegroundColor Red
exit 1




