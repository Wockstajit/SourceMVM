[CmdletBinding()]
param(
    [int]$Port = 29010,
    [switch]$LibraryOnly,
    [string]$InteriorPoses = 'Automation/fixtures/environment-interior-poses.json',
    [string]$OutputDir = 'Automation/output/environment-competitive-matrix',
    [string[]]$Maps = @(
        'de_ancient', 'de_anubis', 'de_cache', 'de_dust2', 'de_inferno',
        'de_mirage', 'de_nuke', 'de_overpass', 'de_train', 'de_vertigo'
    )
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
$cs2 = & (Join-Path $repo 'Automation/lib/Resolve-Cs2Dir.ps1')
if (-not $cs2) { throw 'CS2 is not installed.' }
# mirv_streams resolves a relative record name against cs2.exe's folder, so
# always pass an absolute path under the ignored output directory.
$recordRoot = Join-Path $output 'takes'
$runName = 'mvm_environment_matrix_' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')
New-Item -ItemType Directory -Force -Path $output | Out-Null
Add-Type -AssemblyName System.Drawing

function Send-Netcon {
    param([string[]]$Commands, [double]$ReadSeconds = 0.12, [string]$ExpectedReply = '', [int]$ReplyTimeoutSeconds = 120)
    # A level transition can briefly recreate the listener. Retry only before
    # sending any commands, so mutations and capture requests cannot be replayed.
    for ($attempt = 0; $attempt -lt 4; ++$attempt) {
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
            if (-not $connect.AsyncWaitHandle.WaitOne(3000)) { throw "Netcon $Port did not answer." }
            $client.EndConnect($connect)
            break
        } catch {
            $client.Dispose()
            if ($attempt -eq 3) { throw }
            Start-Sleep -Milliseconds 400
        }
    }
    $client.NoDelay = $true
    $stream = $client.GetStream()
    $encoding = [Text.Encoding]::ASCII
    $buffer = New-Object byte[] 16384
    $text = [Text.StringBuilder]::new()
    function Read-Reply([double]$seconds) {
        $until = (Get-Date).AddSeconds($seconds)
        while ((Get-Date) -lt $until) {
            if ($stream.DataAvailable) {
                $count = $stream.Read($buffer, 0, $buffer.Length)
                if ($count -gt 0) { [void]$text.Append($encoding.GetString($buffer, 0, $count)) }
            } else { Start-Sleep -Milliseconds 15 }
        }
    }
    Read-Reply 0.08
    # The listener may replay pending console output from an earlier connection.
    # It cannot acknowledge this request or supply its camera/lighting state.
    [void]$text.Clear()
    foreach ($command in $Commands) {
        $bytes = $encoding.GetBytes($command + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Read-Reply $ReadSeconds
    }
    if ($ExpectedReply) {
        $deadline = (Get-Date).AddSeconds($ReplyTimeoutSeconds)
        while ($text.ToString() -notmatch $ExpectedReply -and (Get-Date) -lt $deadline) { Read-Reply .1 }
        if ($text.ToString() -notmatch $ExpectedReply) {
            $client.Dispose()
            throw "Netcon did not acknowledge '$ExpectedReply': $($text.ToString())"
        }
    }
    $client.Dispose()
    return $text.ToString()
}

function Wait-Map([string]$Map, [int]$TimeoutSeconds = 120) {
    $until = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $until) {
        try {
            $reply = Send-Netcon @('mirv_filmmaker environment status', 'status') 0.18
            # The hook publishes the map before the client finishes shader compilation.
            # Restarting the round during levelload can crash server.dll.
            if ($reply -match "current='$([regex]::Escape($Map))'" -and
                $reply -match '@ Current\s+:\s+game(?:\r?\n|$)') { return $reply }
        } catch { }
        Start-Sleep -Milliseconds 350
    }
    throw "$Map did not become ready in $TimeoutSeconds seconds."
}

function Wait-MapLighting([string]$Map, [int]$TimeoutSeconds = 180) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $reply = Send-Netcon @('mirv_filmmaker environment status','mirv_relight status') .4 '  reflections='
        if ($reply -match 'Map data unavailable: ([^
]+)') {
            throw "Runtime map data for $Map failed: $($Matches[1]); native lighting must remain active."
        }
        if ($reply -match "relight: enabled=\d generation=\d+ lights=([1-9]\d*) map='$([regex]::Escape($Map))'") { return }
        Start-Sleep -Milliseconds 500
    }
    throw "Runtime authored lighting for $Map did not become ready; disk cache alone is insufficient."
}

function Get-RecordCommand([string]$RecordName) {
    return 'mirv_streams record name "' + (Join-Path $recordRoot $RecordName) + '"'
}

function Open-Frame([string]$Path) {
    # Recording completion queues disk writes; a BMP may still be incomplete.
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        try { return [Drawing.Bitmap]::FromFile($Path) }
        catch { if ($attempt -eq 99) { throw }; Start-Sleep -Milliseconds 100 }
    }
}

function Remove-Take([string]$Path) {
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop; return }
        catch { if ($attempt -eq 49) { throw }; Start-Sleep -Milliseconds 200 }
    }
}

# Raw BMP takes are 5-15 GB per run. Re-encode every frame as lossless PNG
# and drop the BMP, keeping all frames for motion evidence at a fraction of the size.
function Compress-Take([string]$TakeDir) {
    foreach ($frame in @(Get-ChildItem -LiteralPath $TakeDir -Filter '*.bmp' -Recurse)) {
        $bitmap = Open-Frame $frame.FullName
        try { $bitmap.Save([IO.Path]::ChangeExtension($frame.FullName, '.png'), [Drawing.Imaging.ImageFormat]::Png) }
        finally { $bitmap.Dispose() }
        Remove-Item -LiteralPath $frame.FullName -Force
    }
}

function Convert-LastFrame([string]$RecordName, [string]$Destination) {
    $recordDir = Join-Path $recordRoot $RecordName
    $take = Get-ChildItem -LiteralPath $recordDir -Directory |
        Sort-Object LastWriteTimeUtc | Select-Object -Last 1
    if (-not $take) { throw "No take was created for $RecordName." }
    $frame = Get-ChildItem -LiteralPath $take.FullName -Filter '*.bmp' |
        Sort-Object Name | Select-Object -Last 1
    if (-not $frame) { throw "No frame was recorded for $RecordName." }
    $bitmap = Open-Frame $frame.FullName
    try { $bitmap.Save($Destination, [Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
    $count = @(Get-ChildItem -LiteralPath $take.FullName -Filter '*.bmp').Count
    # Only the last frame is kept; the raw take is disposable.
    Remove-Take $recordDir
    return $count
}

function Assert-CameraPose([double[]]$Eye, [double[]]$Angles) {
    $match = $null
    for ($attempt=0;$attempt -lt 4;$attempt++) {
        $reply = Send-Netcon @('getpos') .4 'setpos\s+.*;setang'
        $match = @([regex]::Matches($reply,'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+)'))[-1]
        if ($match.Success) { break }
    }
    if (-not $match.Success) { throw 'No camera pose; refusing capture.' }
    for ($axis=0;$axis -lt 3;$axis++) {
        $actual = [double]::Parse($match.Groups[$axis+1].Value,[Globalization.CultureInfo]::InvariantCulture)
        if ([math]::Abs($actual-$Eye[$axis]) -gt 1) { throw 'Camera moved from the checked interior pose.' }
    }
    for ($axis=0;$axis -lt $Angles.Count;$axis++) {
        $actual = [double]::Parse($match.Groups[$axis+4].Value,[Globalization.CultureInfo]::InvariantCulture)
        $difference = (($actual-$Angles[$axis]+540)%360)-180
        if ([math]::Abs($difference) -gt 1) { throw 'Camera turned from the checked view; capture rejected.' }
    }
}

function Assert-RelightState([string]$Map, [int]$Enabled, [int]$Gi = -1) {
    $reply = Send-Netcon @('mirv_filmmaker environment status','mirv_relight status') .2 '  reflections='
    $scene = [regex]::Match($reply, "relight: enabled=(\d) generation=\d+ lights=(\d+) map='([^']+)'")
    $giState = [regex]::Match($reply, '\bgi=(\d)\b')
    $environment = [regex]::Match($reply, "environment: enabled=(\d) map='([^']*)' current='([^']*)'")
    if (-not $environment.Success -or $environment.Groups[2].Value -ne $Map -or $environment.Groups[3].Value -ne $Map -or [int]$environment.Groups[1].Value -ne $Enabled -or
        -not $scene.Success -or $scene.Groups[3].Value -ne $Map -or [int]$scene.Groups[1].Value -ne $Enabled -or
        ($Enabled -eq 1 -and [int]$scene.Groups[2].Value -eq 0) -or
        ($Gi -ge 0 -and (-not $giState.Success -or [int]$giState.Groups[1].Value -ne $Gi))) {
        throw "Unexpected relighting state for $Map enabled=$Enabled gi=${Gi}: $reply"
    }
    return [pscustomobject]@{map=$scene.Groups[3].Value;enabled=[int]$scene.Groups[1].Value;lights=[int]$scene.Groups[2].Value;gi=[int]$giState.Groups[1].Value;environmentMap=$environment.Groups[2].Value;environmentEnabled=[int]$environment.Groups[1].Value;raw=$reply}
}

function Capture-Frame([string]$Map, [string]$Phase, [double[]]$Eye=@(), [double[]]$Angles=@(), [int]$RelightEnabled=-1, [int]$Gi=-1) {
    if ($Eye.Count) { Assert-CameraPose $Eye $Angles }
    $relightState = $null
    if ($RelightEnabled -ge 0) { $relightState = Assert-RelightState $Map $RelightEnabled $Gi }
    $recordName = "$runName/$Map-$Phase"
    [void](Send-Netcon @(
        'hideconsole', 'fov_cs_debug 0', 'mirv_fov 110',
        (Get-RecordCommand $recordName),
        'mirv_streams record format bmp',
        'mirv_streams record screen enabled 1',
        'mirv_streams record start'
    ) .2 'Recording to "')
    # Wait for the start acknowledgement before ending the take. This also keeps
    # all setup commands connected until they have reached the engine.
    # Wide map views can take more than 80 ms for one render frame.
    Start-Sleep -Milliseconds 600
    [void](Send-Netcon @('mirv_streams record end') .2 'done\.')
    if ($Eye.Count) { Assert-CameraPose $Eye $Angles }
    $path = Join-Path $output "$Map-$Phase.png"
    $frames = Convert-LastFrame $recordName $path
    if ($RelightEnabled -ge 0) { [void](Assert-RelightState $Map $RelightEnabled $Gi) }
    return @{ path = $path; frames = $frames; relight = $relightState }
}

if ($LibraryOnly) { return }
$routePath = Join-Path $repo $InteriorPoses
if (-not (Test-Path -LiteralPath $routePath)) { throw 'An interior camera route is required; refusing guessed map positions.' }
$routes = Get-Content -LiteralPath $routePath -Raw | ConvertFrom-Json
$results = @()
foreach ($map in $Maps) {
    $mapPackage = Join-Path $cs2 "game/csgo/maps/$map.vpk"
    if (-not (Test-Path -LiteralPath $mapPackage)) {
        $results += [pscustomobject]@{ map = $map; result = 'not-installed' }
        continue
    }
    Write-Host "[$map] load -> exec dev" -ForegroundColor Cyan
    $timer = [Diagnostics.Stopwatch]::StartNew()
    [void](Send-Netcon @('mvm_debug stop', 'tv_enable 1', "map $map") 0.08)
    [void](Wait-Map $map)
    $timer.Stop()

    # Join before dev.cfg's round restart. Cache readiness alone is not player readiness.
    $joined = $false
    for ($attempt=0; $attempt -lt 20 -and -not $joined; $attempt++) {
        $reply = Send-Netcon @('jointeam 3 1', 'mp_team_intro_time 0', 'getpos') 0.15
        $p = [regex]::Match($reply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
        if ($p.Success) {
            $joined = [math]::Abs([double]$p.Groups[1].Value) + [math]::Abs([double]$p.Groups[2].Value) + [math]::Abs([double]$p.Groups[3].Value) -gt 1
        }
        if (-not $joined) { Start-Sleep -Seconds 1 }
    }
    if (-not $joined) { throw "No spawned player on $map; refusing dev.cfg round restart." }
    [void](Send-Netcon @('exec dev') 0.15)
    Start-Sleep -Seconds 4
    [void](Send-Netcon @('sv_cheats 1', 'bot_kick', 'noclip 1', 'cl_drawhud 0', 'r_drawviewmodel 0', 'mvm_debug start') 0.11)
    $route = $routes | Where-Object { $_.map -eq $map } | Select-Object -First 1
    if (-not $route) { throw "No reviewed interior camera for $map." }
    $manifestPath = Join-Path $env:APPDATA "HLAE/source_mvm/environment/occlusion/$map.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($route.visualReview -ne 'accepted' -or $route.physicsDigest -ne $manifest.physicsDigest) {
        throw "Interior route for $map needs review against the current collision revision."
    }
    $x = [double]$route.eye[0]; $y = [double]$route.eye[1]; $z = [double]$route.eye[2]
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $pose = 'setpos {0} {1} {2}' -f $x.ToString($culture), $y.ToString($culture), ($z - 64).ToString($culture)
    $view = 'setang {0} {1} 0' -f $route.angles[0], $route.angles[1]
    [void](Send-Netcon @($pose, $view) 0.1)
    $positionReply = Send-Netcon @('getpos') 0.2
    $position = [regex]::Match($positionReply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
    if (-not $position.Success) { throw "No actual camera pose for $map." }
    $actualEye = @(); foreach ($axis in 1..3) { $actualEye += [double]::Parse($position.Groups[$axis].Value, $culture) }
    for ($axis=0; $axis -lt 3; $axis++) {
        if ([math]::Abs($actualEye[$axis] - $route.eye[$axis]) -gt 1) { throw "Camera did not reach the interior route on $map." }
    }
    $validationText = & python (Join-Path $PSScriptRoot 'interior-camera.py') $map --eye $actualEye[0] $actualEye[1] $actualEye[2]
    if ($LASTEXITCODE -ne 0) { throw "Interior camera rejected on ${map}: $validationText" }
    $validation = $validationText | ConvertFrom-Json

    [void](Send-Netcon @(
        $pose, $view, 'mirv_filmmaker environment reset all', 'mirv_filmmaker environment bind', 'mirv_filmmaker environment on',
        'mirv_filmmaker environment climate sky 0', 'mirv_filmmaker environment climate relighting 0',
        'mirv_filmmaker environment climate wet 0', 'mirv_filmmaker environment climate puddles 0'
    ) 0.09)
    $native = Capture-Frame $map 'native' $actualEye $route.angles

    [void](Send-Netcon @('mirv_filmmaker environment climate enabled 1', 'mirv_filmmaker environment climate sky 1') 0.15)
    $sky = Capture-Frame $map 'sky' $actualEye $route.angles

    [void](Send-Netcon @(
        'mirv_filmmaker environment climate relighting 1',
        'mirv_filmmaker environment climate relight 0.62',
        'mirv_filmmaker environment climate sunRelight 0.8',
        'mirv_filmmaker environment climate ambient 0.3',
        'mirv_filmmaker environment climate contactShadows 1'
    ) 0.09)
    $lighting = Capture-Frame $map 'lighting' $actualEye $route.angles
    [void](Send-Netcon @('mirv_filmmaker environment climate sunAzimuth 0') 0.15)
    $sunEast = Capture-Frame $map 'sun-0' $actualEye $route.angles
    [void](Send-Netcon @('mirv_filmmaker environment climate sunAzimuth 180') 0.15)
    $sunWest = Capture-Frame $map 'sun-180' $actualEye $route.angles
    $state = Send-Netcon @('mirv_filmmaker config state', 'mirv_filmmaker environment status') 0.2

    $row = [pscustomobject][ordered]@{
        map = $map
        result = 'captured'
        loadSeconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
        camera = $actualEye
        angles = $route.angles
        validation = $validation
        nativeFrames = $native.frames
        skyFrames = $sky.frames
        lightingFrames = $lighting.frames
        shaderWater = $state -match "water='Shader water'"
        environmentRendering = $state -match "render='Rendering'"
        bakedShadowsOff = $state -match '"bakedShadows":false'
        dynamicShadowsOff = $state -match '"dynamicShadows":false'
    }
    $results += $row
    # Preserve completed map evidence if a later capture or map transition fails.
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
    Write-Host ("[{0}] {1:N2}s, sky/water/lighting captured" -f $map, $timer.Elapsed.TotalSeconds) -ForegroundColor Green
}

$json = Join-Path $output 'results.json'
$csv = Join-Path $output 'results.csv'
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $json -Encoding UTF8
$results | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
[void](Send-Netcon @('mvm_debug stop') 0.15)
Write-Host "Environment map matrix complete: $output" -ForegroundColor Green
