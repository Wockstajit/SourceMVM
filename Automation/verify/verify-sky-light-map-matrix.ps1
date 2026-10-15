# In-game A/B verification for the skybox, scene-light direction and the two
# independent shadow switches. This deliberately talks to a live private
# practice server over -netconport; it does not use demos or replay state.
#
#   powershell -File Automation/verify/verify-sky-light-map-matrix.ps1
#
# The output is a machine-readable JSONL/CSV record under Automation/output.
param(
    [int]$Port = 29010,
    [int]$MapWaitSeconds = 5,
    [string]$OutputDir = "Automation/output/sky-light-matrix",
    [string]$OnlyMap = ''
)
$ErrorActionPreference = 'Stop'

$maps = @(
    'de_ancient', 'de_anubis', 'de_cache', 'de_dust2', 'de_inferno',
    'de_mirage', 'de_nuke', 'de_overpass', 'de_train', 'de_vertigo',
    'cs_office', 'cs_italy', 'de_boulder', 'de_fachwerk', 'cs_shelter',
    'ar_baggage', 'ar_pool_day', 'ar_shoots'
)
if ($OnlyMap) { $maps = @($OnlyMap) }

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$jsonlPath = Join-Path $OutputDir 'results.jsonl'
$csvPath = Join-Path $OutputDir 'results.csv'
Remove-Item -Force -ErrorAction SilentlyContinue $jsonlPath, $csvPath

function Send-ConsoleBatch {
    param([string[]]$Commands, [double]$ReadSeconds = 0.35)
    $client = New-Object System.Net.Sockets.TcpClient
    $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
    if (-not $connect.AsyncWaitHandle.WaitOne(3000)) {
        $client.Close()
        throw "Could not connect to CS2 netcon on port $Port."
    }
    $client.EndConnect($connect)
    $client.NoDelay = $true
    $stream = $client.GetStream()
    $encoding = [System.Text.Encoding]::ASCII
    $builder = New-Object System.Text.StringBuilder
    $buffer = New-Object byte[] 8192

    function Read-For([double]$Seconds) {
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            if ($stream.DataAvailable) {
                $count = $stream.Read($buffer, 0, $buffer.Length)
                if ($count -gt 0) { [void]$builder.Append($encoding.GetString($buffer, 0, $count)) }
            } else { Start-Sleep -Milliseconds 25 }
        }
    }

    Read-For 0.15
    foreach ($command in $Commands) {
        $bytes = $encoding.GetBytes($command + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Read-For $ReadSeconds
    }
    $client.Close()
    # The console highlights field names with ANSI SGR sequences when a
    # command is echoed. Remove those before scalar extraction (otherwise
    # `skyOrientOk` can arrive as `ESC[7mskyOrientESC[0mOk`).
    $ansi = [string][char]27 + '\[[0-9;]*m'
    return (($builder.ToString() -replace $ansi, '') -replace '[\r\n]', '')
}

function Read-Field([string]$Text, [string]$Name) {
    # Config state is line-wrapped by the engine console. Restrict parsing to
    # scalar fields so a wrap inside skyList cannot make the result unusable.
    $pattern = '"' + [regex]::Escape($Name) + '":\s*(?:"([^"]*)"|([^,}\s]+))'
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) { return $null }
    if ($match.Groups[1].Success) { return $match.Groups[1].Value }
    return $match.Groups[2].Value
}

function Read-Number([string]$Text, [string]$Name) {
    $value = Read-Field $Text $Name
    $number = 0.0
    if ($null -ne $value -and [double]::TryParse($value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { return $number }
    return $null
}

function Read-Bool([string]$Text, [string]$Name) {
    $value = Read-Field $Text $Name
    if ($null -eq $value) { return $null }
    return $value -eq 'true'
}

function Get-State([string]$Map, [string]$Phase) {
    $text = Send-ConsoleBatch @('mirv_filmmaker config state') 0.65
    $health = Send-ConsoleBatch @('mirv_filmmaker world health') 0.35
    $row = [ordered]@{
        map = $Map; phase = $Phase
        skyYaw = Read-Number $text 'skyYaw'; skyPitch = Read-Number $text 'skyPitch'
        skyEnts = Read-Number $text 'skyEnts'; skyFogEnts = Read-Number $text 'skyFogEnts'
        skyOrientOk = Read-Bool $text 'skyOrientOk'; sunYaw = Read-Number $text 'sunYaw'
        sunPitch = Read-Number $text 'sunPitch'; lightRotationOk = Read-Bool $text 'lightRotationOk'
        bakedShadows = Read-Bool $text 'bakedShadows'; bakedShadowsApplied = Read-Bool $text 'bakedShadowsApplied'
        bakedShadowsOk = Read-Bool $text 'bakedShadowsOk'; dynamicShadows = Read-Bool $text 'dynamicShadows'
        dynamicShadowsApplied = Read-Bool $text 'dynamicShadowsApplied'; dynamicShadowsOk = Read-Bool $text 'dynamicShadowsOk'
        dynamicShadowsCvar = Read-Field $text 'dynamicShadowsCvar'
        health = (($health -replace "`r", '') -replace "`n", ' ').Trim()
    }
    return [pscustomobject]$row
}

$rows = @()
foreach ($map in $maps) {
    Write-Host "[$map] loading..." -ForegroundColor Cyan
    [void](Send-ConsoleBatch @("map $map") 0.25)
    Start-Sleep -Seconds $MapWaitSeconds
    [void](Send-ConsoleBatch @(
        'sv_cheats 1', 'bot_kick', 'mp_warmuptime 999999',
        'mp_roundtime 9999', 'mp_roundtime_defuse 9999', 'mp_freezetime 0',
        'noclip 1', 'fov_cs_debug 120', 'r_drawviewmodel 0',
        'mirv_filmmaker world bakedshadows on',
        'mirv_filmmaker world dynamicshadows on',
        'mirv_filmmaker world skyorient reset',
        'mirv_filmmaker world light reset'
    ) 0.2)
    Start-Sleep -Milliseconds 900

    $base = Get-State $map 'base'
    [void](Send-ConsoleBatch @('mirv_filmmaker world skyorient yaw 90') 0.25)
    Start-Sleep -Milliseconds 600
    $skyB = Get-State $map 'sky-yaw-90'
    [void](Send-ConsoleBatch @('mirv_filmmaker world skyorient yaw 0') 0.25)

    [void](Send-ConsoleBatch @('mirv_filmmaker world bakedshadows off') 0.25)
    Start-Sleep -Milliseconds 600
    $bakedOff = Get-State $map 'baked-off'
    [void](Send-ConsoleBatch @('mirv_filmmaker world bakedshadows on', 'mirv_filmmaker world dynamicshadows off') 0.25)
    Start-Sleep -Milliseconds 600
    $dynamicOff = Get-State $map 'dynamic-off'
    [void](Send-ConsoleBatch @('mirv_filmmaker world dynamicshadows on') 0.25)

    $result = [pscustomobject][ordered]@{
        map = $map
        baseSkyEnts = $base.skyEnts; baseFogEnts = $base.skyFogEnts; baseSkyOk = $base.skyOrientOk
        skyYawBefore = $base.skyYaw; skyYawAfter = $skyB.skyYaw
        skyABChanged = ($null -ne $base.skyYaw -and $null -ne $skyB.skyYaw -and $base.skyYaw -ne $skyB.skyYaw)
        lightRotationOk = $base.lightRotationOk
        bakedOn = $base.bakedShadows; bakedOff = $bakedOff.bakedShadows
        bakedToggleApplied = ($bakedOff.bakedShadows -eq $false -and $base.bakedShadows -eq $true -and $bakedOff.bakedShadowsOk -eq $true)
        dynamicOn = $base.dynamicShadows; dynamicOff = $dynamicOff.dynamicShadows
        dynamicToggleApplied = ($dynamicOff.dynamicShadows -eq $false -and $base.dynamicShadows -eq $true -and $dynamicOff.dynamicShadowsOk -eq $true)
        dynamicCvar = $base.dynamicShadowsCvar
        health = $base.health
    }
    $rows += $result
    ($result | ConvertTo-Json -Compress) | Add-Content -Encoding UTF8 $jsonlPath
    Write-Host ("[{0}] sky={1}/{2} fog={3} skyAB={4} bakedAB={5} dynamicAB={6}" -f `
        $map, $result.baseSkyEnts, $result.baseSkyOk, $result.baseFogEnts,
        $result.skyABChanged, $result.bakedToggleApplied, $result.dynamicToggleApplied)
}

$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $csvPath
Write-Host "Wrote $jsonlPath and $csvPath" -ForegroundColor Green
