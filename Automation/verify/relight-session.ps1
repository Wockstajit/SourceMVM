[CmdletBinding()]
param(
    [string]$Map = 'de_inferno',
    [int]$Port = 29010,
    [switch]$NoBuild,
    # Skip the quit/install/launch cycle and only (re)load the map into the running game.
    [switch]$KeepGame,
    [string]$Extra = ''
)
# One command from "edited the hook" to "offline map loaded with a free camera": build, quit the
# running game the clean way (a force-kill poisons Steam's pipe), stage the DLL, launch, load the map.
# Everything after that is netcon (Automation/netcon/cs2-netcon.ps1).
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir 'Automation/output/lighting-live'

if (-not $KeepGame) {
    if (-not $NoBuild) {
        Write-Host '[build] AfxHookSource2'
        cmake --build --preset x64-source2-release --parallel --target AfxHookSource2 2>&1 |
            Where-Object { $_ -match 'error|Error' } | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw 'Hook build failed.' }
    }
    if (Get-Process cs2 -ErrorAction SilentlyContinue) {
        Write-Host '[quit] asking CS2 to close over netcon'
        try { [void](Send-Netcon @('mvm_debug stop', 'quit') 0.3) } catch { }
        for ($t = 0; $t -lt 60 -and (Get-Process cs2 -ErrorAction SilentlyContinue); $t += 2) { Start-Sleep 2 }
        if (Get-Process cs2 -ErrorAction SilentlyContinue) { throw 'CS2 did not quit; close it by hand (never taskkill /F).' }
        Start-Sleep 8
    }
    Write-Host '[install] hook + runtime'
    cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-runtime | Out-Null
    cmake --install build/x64-source2-release --config Release --prefix build/source2-release --component x64-source2 | Out-Null
    $staged = Get-Item build\source2-release\bin\x64\AfxHookSource2.dll
    Write-Host ("[install] staged DLL {0}" -f $staged.LastWriteTime)
    Write-Host '[launch] CS2'
    # Redirect to files, never a pipe: cs2.exe inherits the launcher's stdout, so a piped launcher
    # (| Out-Null, a captured task) only "finishes" when the game exits.
    $launchLog = Join-Path $repo 'Automation/output/lighting-live/launch.log'
    $launcher = Start-Process cmd.exe -ArgumentList '/c', "chcp 1252 >nul && powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File Automation\launch\launch-cs2-netcon.ps1 -Port $Port -Monitor 3" `
        -RedirectStandardOutput $launchLog -RedirectStandardError "$launchLog.err" -PassThru -WindowStyle Hidden
    # Cache the process handle before exit; Windows PowerShell otherwise loses ExitCode.
    $null = $launcher.Handle
    if (-not $launcher.WaitForExit(240000)) { throw 'The launcher did not report ready within 240 s.' }
    if ($launcher.ExitCode -ne 0) { throw "CS2 launcher failed (exit $($launcher.ExitCode)); see $launchLog.err." }
}

Write-Host "[map] $Map"
# A map command sent before the main menu is up is dropped without a word, so give it a few tries.
$loaded = $false
for ($attempt = 0; $attempt -lt 4 -and -not $loaded; ++$attempt) {
    Start-Sleep -Seconds ($(if ($attempt -eq 0 -and -not $KeepGame) { 12 } else { 2 }))
    [void](Send-Netcon @("map $Map") 0.1)
    try { [void](Wait-Map $Map 60); $loaded = $true } catch { Write-Host "[map] attempt $($attempt + 1) timed out" }
}
if (-not $loaded) { throw "$Map did not load." }
Start-Sleep 2
[void](Send-Netcon @('exec dev', 'sv_cheats 1') 0.12)
Start-Sleep 3
[void](Send-Netcon @('jointeam 3', 'mp_team_intro_time 0') 0.15)
Start-Sleep 12
# The first join can be dropped while the team-selection view is still loading.
# getpos also answers there, with the zero player pose; do not call that ready.
$joined = $false
for ($attempt = 0; $attempt -lt 12 -and -not $joined; ++$attempt) {
    $poseReply = Send-Netcon @('getpos') 0.15
    $poseMatch = [regex]::Match($poseReply, 'setpos\s+(-?[0-9.]+)\s+(-?[0-9.]+)\s+(-?[0-9.]+);setang')
    if ($poseMatch.Success) {
        $culture = [Globalization.CultureInfo]::InvariantCulture
        $px = [double]::Parse($poseMatch.Groups[1].Value, $culture)
        $py = [double]::Parse($poseMatch.Groups[2].Value, $culture)
        $pz = [double]::Parse($poseMatch.Groups[3].Value, $culture)
        $joined = [math]::Abs($px) + [math]::Abs($py) + [math]::Abs($pz) -gt 1
    }
    if (-not $joined) {
        [void](Send-Netcon @('jointeam 3 1', 'mp_team_intro_time 0') 0.15)
        Start-Sleep -Seconds 1
    }
}
if (-not $joined) { throw 'No valid player pose after joining; the team-selection view is not a lighting test.' }
$reply = Send-Netcon @('sv_cheats 1', 'bot_kick', 'bot_quota 0', 'mp_roundtime 60', 'mp_roundtime_defuse 60',
    'mp_ignore_round_win_conditions 1', 'noclip 1', 'r_drawviewmodel 0', 'fov_cs_debug 0', 'mirv_fov 100',
    'cl_drawhud 0', 'mirv_filmmaker hud off', 'getpos') 0.12
if ($Extra) { [void](Send-Netcon @($Extra -split ';;') 0.15) }
Write-Host ([regex]::Match($reply, 'setpos[^\r\n]*').Value)
Write-Host '[ready]'
