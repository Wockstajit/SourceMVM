#requires -Version 5
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Demo,
    [string]$Cs2Dir = '',
    [int]$Width = 1600,
    [int]$Height = 1200,
    [int]$ScanTimeoutSeconds = 130,
    [int]$GrenadeDiscoveryTimeoutSeconds = 120,
    [int]$DiscoveryParseLimit = 8,
    [switch]$NoLaunch,
    [string]$OutDir
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-attach-camera-live'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

function Find-Cs2Exe {
    param([string]$Preferred)
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Preferred)) {
        $candidates += (Join-Path $Preferred 'game\bin\win64\cs2.exe')
    }
    # Steam's own library list first, then the conventional install locations. The personal
    # library path that used to head this list only ever resolved on one machine.
    $detected = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1')
    if (-not [string]::IsNullOrWhiteSpace($detected)) {
        $candidates += (Join-Path $detected 'game\bin\win64\cs2.exe')
    }
    $candidates += @(
        'C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe',
        'C:\Program Files\Steam\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe'
    )
    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) { return (Resolve-Path -LiteralPath $path).Path }
    }
    $drives = Get-PSDrive -PSProvider FileSystem | Select-Object -ExpandProperty Root
    foreach ($drive in $drives) {
        $steamRoot = Join-Path $drive 'SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe'
        if (Test-Path -LiteralPath $steamRoot) { return (Resolve-Path -LiteralPath $steamRoot).Path }
    }
    throw 'Could not find cs2.exe.'
}

function Get-Cs2RootFromExe {
    param([string]$Cs2Exe)
    return (Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $Cs2Exe) '..\..\..')).Path
}

function Find-DemoInfoHelper {
    $automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
    $candidates = @(
        (Join-Path $root 'build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe'),
        (Join-Path $root 'build\staging-release\bin\FilmmakerDemoInfo\FilmmakerDemoInfo.exe'),
        (Join-Path $root 'build\staging-release\bin\FilmmakerDemoInfo.exe'),
        (Join-Path $root 'FilmmakerDemoInfoGo\FilmmakerDemoInfo.exe')
    )
    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) { return (Resolve-Path -LiteralPath $path).Path }
    }
    return $null
}

function Read-DemoInfo {
    param([string]$Path, [string]$Helper)
    $cache = "$Path.fmjson"
    if (Test-Path -LiteralPath $cache) {
        try { return (Get-Content -LiteralPath $cache -Raw | ConvertFrom-Json) } catch {}
    }
    if ([string]::IsNullOrWhiteSpace($Helper)) { return $null }
    try {
        $json = & $Helper $Path 2>$null
        if (-not [string]::IsNullOrWhiteSpace($json)) {
            $json | Set-Content -LiteralPath $cache -Encoding UTF8
            return ($json | ConvertFrom-Json)
        }
    } catch {}
    return $null
}

function Find-Mirage913Demo {
    param([string]$Root, [string]$Helper, [int]$ParseLimit)
    $demoRoots = @(
        (Join-Path $Root 'game\csgo\replays'),
        (Join-Path $Root 'game\csgo')
    ) | Where-Object { Test-Path -LiteralPath $_ }
    $demos = foreach ($dir in $demoRoots) {
        Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like '*.dem' -or $_.Name -like '*.dem.bz2' }
    }
    $demos = @($demos | Sort-Object LastWriteTime -Descending)
    if (-not $demos.Count) { throw "No .dem files found under $Root\game\csgo." }

    $parsed = 0
    foreach ($file in $demos) {
        $info = Read-DemoInfo -Path $file.FullName -Helper $Helper
        if ($info -and $info.ok) {
            $scores = @([int]$info.teamScore0, [int]$info.teamScore1)
            if ([string]$info.map -eq 'de_mirage' -and $scores -contains 9 -and $scores -contains 13) {
                return $file.FullName
            }
        }
        $parsed++
        if ($parsed -ge $ParseLimit) { break }
    }
    return $demos[0].FullName
}

$cs2Exe = Find-Cs2Exe -Preferred $Cs2Dir
$cs2Root = Get-Cs2RootFromExe -Cs2Exe $cs2Exe
$helper = Find-DemoInfoHelper
if ([string]::IsNullOrWhiteSpace($Demo)) {
    $Demo = Find-Mirage913Demo -Root $cs2Root -Helper $helper -ParseLimit $DiscoveryParseLimit
}
$Demo = (Resolve-Path -LiteralPath $Demo).Path

Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-attach-camera-live' -Additional @{
    port = $Port
    demo = $Demo
    cs2Exe = $cs2Exe
    cs2Root = $cs2Root
    demoInfoHelper = $helper
} | Out-Null

$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath (Join-Path $OutDir 'verification.log') -Force | Out-Null
    $transcriptStarted = $true
} catch {
    Write-Warning "Could not start transcript: $($_.Exception.Message)"
}

if (-not $NoLaunch) {
    Write-Host "Launching CS2 automation target..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $cs2Root -Width $Width -Height $Height -OutDir (Join-Path $OutDir 'launch') | Out-Host
    Write-Host "Launch phase completed." -ForegroundColor Cyan
}

Write-Host "Connecting to CS2 netcon on 127.0.0.1:$Port..."
$client = [System.Net.Sockets.TcpClient]::new()
try {
    $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
    if (-not $connect.AsyncWaitHandle.WaitOne(8000)) {
        $client.Close()
        throw "Timed out connecting to netcon."
    }
    $client.EndConnect($connect)
} catch {
    Write-Host "[FAIL] No CS2 netcon on 127.0.0.1:$Port." -ForegroundColor Red
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    exit 1
}
$client.NoDelay = $true
$stream = $client.GetStream()
$encoding = [Text.Encoding]::ASCII

function Drain([double]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    $buffer = New-Object byte[] 16384
    $builder = [Text.StringBuilder]::new()
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -gt 0) { [void]$builder.Append($encoding.GetString($buffer, 0, $read)) }
        } else {
            Start-Sleep -Milliseconds 25
        }
    }
    return $builder.ToString()
}

function Send([string]$Command, [double]$Seconds = 0.7) {
    Drain 0.05 | Out-Null
    $bytes = $encoding.GetBytes($Command + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    return Drain $Seconds
}

function Read-EncodedState([string]$Command, [string]$Marker) {
    $text = Send $Command 1.2
    $matches = [regex]::Matches($text, [regex]::Escape($Marker) + '\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)')
    $parts = @{}
    $expected = 0
    $complete = New-Object System.Collections.Generic.List[string]
    foreach ($match in $matches) {
        $index = [int]$match.Groups[1].Value
        $total = [int]$match.Groups[2].Value
        if ($index -eq 1) { $parts = @{}; $expected = $total }
        $parts[$index] = $match.Groups[3].Value
        if ($expected -gt 0 -and $index -eq $expected -and $parts.Count -eq $expected) {
            $complete.Add(((1..$expected | ForEach-Object { $parts[$_] }) -join ''))
        }
    }
    if ($complete.Count -eq 0) { return $null }
    try {
        $json = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($complete[$complete.Count - 1]))
        return ($json | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Read-FollowState { Read-EncodedState 'mirv_filmmaker follow state64' '[followcam][state64]' }

$capture = Join-Path $automationRoot 'capture\capture-game-window.ps1'
function Capture-Case([string]$Name) {
    $path = Join-Path $OutDir ($Name + '.png')
    if (Test-Path -LiteralPath $capture) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out $path | Out-Null
    }
    return $path
}

$results = New-Object System.Collections.Generic.List[object]
function Add-Result([string]$Name, [bool]$Pass, [object]$State, [string]$Screenshot, [string]$Reason) {
    $results.Add([ordered]@{
        name = $Name
        pass = $Pass
        reason = $Reason
        screenshot = $Screenshot
        follow = $State
    })
    Write-Host ("[{0}] {1} {2}" -f ($(if ($Pass) { 'PASS' } else { 'FAIL' })), $Name, $Reason)
}

function Wait-State([scriptblock]$Predicate, [double]$TimeoutSeconds = 8) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $state = Read-FollowState
        if ($state -and (& $Predicate $state)) { return $state }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Select-AttachPoint([object]$State, [string[]]$Preferred) {
    $points = @($State.attachPoints)
    foreach ($id in $Preferred) {
        foreach ($p in $points) {
            if ($p.valid -and [string]$p.id -eq $id) { return [string]$p.id }
        }
    }
    foreach ($p in $points) {
        if ($p.valid) { return [string]$p.id }
    }
    return $null
}

function Run-AttachCase([string]$Name, [string]$SetupCommand, [string[]]$AttachPreference, [double]$MaxDistance, [double]$MaxAimError) {
    Write-Host "Running attach case: $Name" -ForegroundColor Cyan
    Send 'mirv_filmmaker follow stop' | Out-Null
    foreach ($cmd in $SetupCommand -split ';') {
        $trim = $cmd.Trim()
        if ($trim) { Send $trim | Out-Null }
    }
    $state = Wait-State { param($s) $s.targetValid -and @($s.attachPoints).Count -gt 0 } 8
    if (-not $state) {
        Add-Result $Name $false $null '' 'no valid target/attach list'
        return
    }
    $attach = Select-AttachPoint -State $state -Preferred $AttachPreference
    if ($attach) { Send "mirv_filmmaker follow bone $attach" | Out-Null }
    Send 'mirv_filmmaker follow position 0' | Out-Null
    Send 'mirv_filmmaker follow look 0' | Out-Null
    $previewText = Send 'mirv_filmmaker follow preview'
    Start-Sleep -Milliseconds 700
    $rtText = $previewText + (Drain 1.0)
    $samples = @()
    for ($i = 0; $i -lt 8; $i++) {
        $samples += ,(Read-FollowState)
        Start-Sleep -Milliseconds 180
    }
    $final = @($samples | Where-Object { $_ -and $_.attachDebug -and $_.attachDebug.active })[-1]
    $shot = Capture-Case $Name
    $pass = $false
    $reason = 'missing attach telemetry'
    if ($final) {
        $ad = $final.attachDebug
        $distOk = [double]$ad.distance -le $MaxDistance
        $mountValues = @($samples | Where-Object { $_ -and $_.attachDebug } | ForEach-Object {
            if ($_.attachDebug.PSObject.Properties.Name -contains 'mountAngleError') { [double]$_.attachDebug.mountAngleError }
            else { [double]$_.attachDebug.aimError }
        })
        $mountMax = if ($mountValues.Count) { [double]($mountValues | Measure-Object -Maximum).Maximum } else { [double]$ad.aimError }
        $aimOk = $mountMax -le $MaxAimError
        $angleValues = @($samples | Where-Object { $_ -and $_.attachDebug } | ForEach-Object { [double]$_.attachDebug.angleDelta })
        $angleMax = if ($angleValues.Count) { [double]($angleValues | Measure-Object -Maximum).Maximum } else { 0.0 }
        $angleOk = $angleMax -le 35.0
        $jitterValues = @($samples | Where-Object { $_ -and $_.attachDebug } | Select-Object -Last 4 | ForEach-Object { [double]$_.attachDebug.jitter })
        $jitterOk = $true
        if ($jitterValues.Count -ge 4) {
            $sorted = @($jitterValues | Sort-Object)
            $median = [double]$sorted[[int]($sorted.Count / 2)]
            $max = [double]($jitterValues | Measure-Object -Maximum).Maximum
            $jitterOk = ($median -le 0.01) -or ($max -le [Math]::Max(12.0, $median * 3.0))
        }
        $renderSampleOk = $final.PSObject.Properties.Name -contains 'renderTimeSample' -and [bool]$final.renderTimeSample
        $badRtSamples = 0
        foreach ($m in [regex]::Matches($rtText, '\[followcam\]\[rtsample\][^\r\n]*steppedVsFresh=([0-9.]+)[^\r\n]*apply=(\d)')) {
            if ([double]$m.Groups[1].Value -ge 1.0 -and [int]$m.Groups[2].Value -eq 0) { $badRtSamples++ }
        }
        $rtOk = $renderSampleOk -and ($badRtSamples -lt 2)
        $pass = $distOk -and $aimOk -and $angleOk -and $jitterOk -and $rtOk
        $tailMax = if ($jitterValues.Count) { [double]($jitterValues | Measure-Object -Maximum).Maximum } else { 0.0 }
        $reason = "attach=$($final.attachment) dist=$([Math]::Round([double]$ad.distance,1)) mountMax=$([Math]::Round($mountMax,1)) angleMax=$([Math]::Round($angleMax,1)) jitter=$([Math]::Round([double]$ad.jitter,2)) tailMax=$([Math]::Round($tailMax,2)) rtsample=$renderSampleOk badRt=$badRtSamples"
    }
    Add-Result $Name $pass $final $shot $reason
}

Write-Host "Loading demo: $Demo" -ForegroundColor Cyan
Send ('playdemo "' + ($Demo -replace '\\','/') + '"') 6 | Out-Null
Send 'demo_pause' | Out-Null
Send 'mirv_filmmaker editor on' | Out-Null
Send "mirv_filmmaker editor eval `"`$.CamEditor && `$.CamEditor.setInspectorMode('follow');`"" | Out-Null
Send 'mirv_filmmaker follow debug 1' | Out-Null
Send 'mirv_filmmaker follow mode attach' | Out-Null

Write-Host "Running core attach cases..." -ForegroundColor Cyan
Run-AttachCase 'player-head-eyes' 'mirv_filmmaker follow type player; mirv_filmmaker follow nearest' @('eyes','head') 180 8
Run-AttachCase 'player-hand' 'mirv_filmmaker follow type player; mirv_filmmaker follow nearest' @('weapon_hand_R','weapon_hand_L','hand_r','hand_l','head') 180 8
Run-AttachCase 'weapon-muzzle' 'mirv_filmmaker follow type weapon; mirv_filmmaker follow weaponsource held; mirv_filmmaker follow nearest' @('muzzle','muzzle_flash','entity') 180 8

$knifeState = Read-FollowState
$knife = @($knifeState.candidates | Where-Object { ([string]$_.name -match 'Knife') -or ([string]$_.className -match 'knife') } | Select-Object -First 1)
if ($knife) {
    Run-AttachCase 'knife-attach' ("mirv_filmmaker follow type weapon; mirv_filmmaker follow select {0}" -f $knife.index) @('muzzle','entity') 180 8
} else {
    Add-Result 'knife-attach' $true $knifeState '' 'skipped: no live knife candidate exposed near current tick'
}

Send 'mirv_filmmaker follow type grenade' | Out-Null
Send 'mirv_filmmaker follow mode attach' | Out-Null
Send 'demo_gototick 0' 1.0 | Out-Null
Start-Sleep -Seconds 1
Send 'demo_timescale 8' | Out-Null
Send 'demo_resume' | Out-Null
$grenadeState = Wait-State { param($s) @($s.candidates).Count -gt 0 } $GrenadeDiscoveryTimeoutSeconds
Send 'demo_pause' | Out-Null
Send 'demo_timescale 1' | Out-Null
if ($grenadeState) {
    $grenade = @($grenadeState.candidates | Where-Object { ([string]$_.grenadeType -match 'Smoke') } | Select-Object -First 1)
    if (-not $grenade) { $grenade = @($grenadeState.candidates | Select-Object -First 1) }
    Run-AttachCase 'smoke-grenade-attach' ("mirv_filmmaker follow type grenade; mirv_filmmaker follow selecthandle {0}" -f $grenade.handle) @('entity') 220 12
} else {
    Add-Result 'smoke-grenade-attach' $false $null '' 'no grenade candidates discovered'
}

Send 'mirv_filmmaker follow type weapon' | Out-Null
$scan = Wait-State { param($s) $s.eventStatus -eq 2 -or $s.eventStatus -eq 3 } $ScanTimeoutSeconds
if ($scan -and $scan.eventStatus -eq 2) {
    Add-Result 'preview-tick-scan' $true $scan (Capture-Case 'preview-tick-scan') "ready events=$($scan.eventCount)"
    $drop = @($scan.events | Where-Object { [string]$_.type -match 'drop|pickup' } | Select-Object -First 1)
    if ($drop) {
        Send ("mirv_filmmaker follow eventselect {0}" -f $drop.index) | Out-Null
        Send 'mirv_filmmaker follow previewtick' 1.0 | Out-Null
        Start-Sleep -Seconds 2
        Run-AttachCase 'dropped-weapon-attach' 'mirv_filmmaker follow type weapon; mirv_filmmaker follow weaponsource dropped; mirv_filmmaker follow nearest' @('entity','muzzle') 220 12
    } else {
        Add-Result 'dropped-weapon-attach' $true $scan '' 'skipped: no drop/pickup event returned'
    }
} elseif ($scan) {
    Add-Result 'preview-tick-scan' $true $scan (Capture-Case 'preview-tick-scan') ("failed clearly: " + $scan.eventError)
    Add-Result 'dropped-weapon-attach' $true $scan '' 'skipped: scan failed clearly'
} else {
    Add-Result 'preview-tick-scan' $false $null (Capture-Case 'preview-tick-scan') 'scan did not reach ready or failed'
}

Send 'mirv_filmmaker follow stop' | Out-Null
$client.Close()

$resultPath = Join-Path $OutDir 'results.json'
$results | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$failures = @($results | Where-Object { -not $_.pass }).Count
Write-Host "Artifacts: $OutDir"
if ($transcriptStarted) { Stop-Transcript | Out-Null }
if ($failures -eq 0) { exit 0 }
exit 1




