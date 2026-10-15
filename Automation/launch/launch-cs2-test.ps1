# Launches CS2 with the hook + FX pack, then starts mvm_test offline bot practice.
# Press Insert in-game to open the FX test menu.
#
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-test.ps1
#   powershell.exe -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-test.ps1 -Map de_mirage
param(
    [int]$Port = 29010,
    [string]$Map = 'de_dust2',
    [string]$Cs2Dir = '',
    [int]$Width = 854,
    [int]$Height = 480,
    [ValidateRange(1, 16)]
    [int]$Monitor = 3,
    [int]$ReadyTimeoutSeconds = 90,
    [int]$SettleSeconds = 12,
    [switch]$FreeMouse,
    [string]$FxAssetsGameDir = '',
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')

# Counter-Strike 2 location. Empty -Cs2Dir means 'work it out', which is the normal path:
# this used to default to the author's own Steam library, so a fresh clone could not launch
# the game without editing the script. Override with -Cs2Dir or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = Get-Cs2InstallDir }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass -Cs2Dir <install root> or set SOURCEMVM_CS2_DIR.'
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'launch-cs2-test'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

$netconArgs = @(
    '-Port', $Port, '-Cs2Dir', $Cs2Dir, '-Width', $Width, '-Height', $Height, '-Monitor', $Monitor,
    '-ReadyTimeoutSeconds', $ReadyTimeoutSeconds, '-SettleSeconds', $SettleSeconds
)
if ($FreeMouse) { $netconArgs += '-FreeMouse' }
if (-not [string]::IsNullOrWhiteSpace($FxAssetsGameDir)) {
    $netconArgs += @('-FxAssetsGameDir', $FxAssetsGameDir)
}
$netconArgs += @('-OutDir', $OutDir)
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'launch-cs2-netcon.ps1') @netconArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Send-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect('127.0.0.1', $Port)
    } catch {
        throw "Could not connect to netcon 127.0.0.1:$Port"
    }
    $client.NoDelay = $true
    $stream = $client.GetStream()
    $encoding = [Text.Encoding]::ASCII
    foreach ($cmd in $Commands) {
        $bytes = $encoding.GetBytes($cmd + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 120
    }
    $deadline = (Get-Date).AddSeconds($ReadSeconds)
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
    $client.Close()
    return $builder.ToString()
}

if ($Map -match '^\d+$') {
    # Workshop addon id. `mvm_test start <map>` issues a plain `map <name>` command, which does
    # not resolve a workshop addon, so load it with host_workshop_map first and then arm
    # mvm_test on the level that is already loaded (its no-map form).
    Write-Host "Loading workshop map $Map, then arming mvm_test..." -ForegroundColor Cyan
    $out = Send-Netcon @('mirv_cvar_unlock_sv_cheats', 'sv_cheats 1', "host_workshop_map $Map") 6.0
    if ($out) { Write-Host $out }
    # Wait for the level to finish loading before arming, otherwise mvm_test sees no level.
    $armed = $false
    for ($try = 0; $try -lt 20; $try++) {
        Start-Sleep -Seconds 2
        $st = Send-Netcon @('mvm_test start') 2.0
        if ($st -match 'armed on current map|menu opening') { Write-Host $st; $armed = $true; break }
    }
    if (-not $armed) {
        Write-Host "WARNING: workshop map $Map did not report ready; run 'mvm_test start' in the console once it has loaded." -ForegroundColor Yellow
    }
} else {
    Write-Host "Starting mvm_test on map '$Map'..." -ForegroundColor Cyan
    $out = Send-Netcon @("mvm_test start $Map") 3.5
    if ($out) { Write-Host $out }
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'launch-cs2-test' -Additional @{
    port = $Port
    map = $Map
    width = $Width
    height = $Height
    monitor = $Monitor
    freeMouse = $FreeMouse.IsPresent
} | Out-Null

Write-Host ''
Write-Host 'mvm_test ready. In-game: press INSERT to open the FX test menu.' -ForegroundColor Green
Write-Host "Console: mvm_test menu toggle | mvm_test status | mvm_test stop" -ForegroundColor DarkGray
Write-Host "Artifacts: $OutDir"
