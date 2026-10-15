# Requires an offline repaired demo open through live.bat / NetCon. Replays one known death.
param(
    [Parameter(Mandatory=$true)][int]$DeathTick,
    [Parameter(Mandatory=$true)][int]$UserId,
    [int]$Port = 29010,
    [string]$OutDir = 'build/verification/follow-death'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$netcon = Join-Path $repo 'Automation/netcon/cs2-netcon.ps1'
$out = [IO.Path]::GetFullPath((Join-Path $repo $OutDir))
New-Item -ItemType Directory -Path $out -Force | Out-Null
$script:step = 0
function Send([string]$command, [double]$read=0.15) {
    $script:step++
    $log = Join-Path $out ('netcon-{0:d3}.txt' -f $script:step)
    $null = & $netcon -Port $Port -ReadSeconds $read -CommandBatch $command -LogPath $log 6>&1
    return [IO.File]::ReadAllText($log)
}
function State {
    $text = Send 'mirv_filmmaker follow state64' 0.4
    $chunks = [regex]::Matches($text, '\[followcam\]\[state64\]\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)')
    $encoded = ''; $parts = @{}; $expected = 0
    foreach ($c in $chunks) {
        $index = [int]$c.Groups[1].Value
        if ($index -eq 1) { $parts = @{}; $expected = [int]$c.Groups[2].Value }
        $parts[$index] = $c.Groups[3].Value
        if ($index -eq $expected -and $parts.Count -eq $expected) {
            $encoded = (1..$expected | ForEach-Object { $parts[$_] }) -join ''
        }
    }
    if (!$encoded) { throw 'No Follow state received over NetCon.' }
    return ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded)) | ConvertFrom-Json)
}
function Check([bool]$ok, [string]$message) {
    if (!$ok) { throw $message }
    Write-Output "PASS: $message"
}
function Prepare([string]$mode, [double]$speed, [bool]$timed) {
    $null = Send ('mirv_filmmaker follow stop;;demo_timescale 1;;demo_gototick {0};;demo_pause' -f ($DeathTick-80)) 0.2
    $s = State
    $player = @($s.players | Where-Object { $_.userId -eq $UserId -and $_.alive })
    if ($player.Count -ne 1) { throw 'The selected player is not alive before the supplied death tick.' }
    $null = Send (('mirv_filmmaker follow mode {0};;mirv_filmmaker follow type player;;mirv_filmmaker follow select {1};;mirv_filmmaker follow reset framing;;mirv_filmmaker follow reset tracking;;mirv_filmmaker follow autodead {2};;mirv_filmmaker follow deathhold 1;;mirv_filmmaker follow place;;demo_timescale {3};;mirv_filmmaker follow preview' -f $mode,$player[0].index,[int]$timed,$speed))
}
$results = @()
try {
    $null = Send 'mirv_filmmaker editor on;;mvm_debug start' 0.4
    # Different values, mode changes, and resets must not contaminate the other mode.
    $null = Send 'mirv_filmmaker follow mode lockon;;mirv_filmmaker follow offset 11 12 13;;mirv_filmmaker follow fov 77;;mirv_filmmaker follow autodead 0;;mirv_filmmaker follow deathhold 4.2;;mirv_filmmaker follow look 0.23;;mirv_filmmaker follow mode attach;;mirv_filmmaker follow offset -24 4 8;;mirv_filmmaker follow fov 81;;mirv_filmmaker follow autodead 1;;mirv_filmmaker follow deathhold 1.5;;mirv_filmmaker follow mode lockon'
    $s = State
    Check ($s.offset[0] -eq 11 -and $s.fov -eq 77 -and !$s.autoDead -and $s.deathHold -eq 4.2 -and $s.look -eq 0.23) 'Lock-on restores its own framing and tracking'
    $null = Send 'mirv_filmmaker follow mode attach'
    $s = State
    Check ($s.offset[0] -eq -24 -and $s.fov -eq 81 -and $s.autoDead -and $s.deathHold -eq 1.5 -and $s.look -eq 0) 'Attached restores its own framing and tracking'
    $null = Send 'mirv_filmmaker follow reset framing;;mirv_filmmaker follow reset tracking;;mirv_filmmaker follow mode lockon'
    $s = State
    Check ($s.offset[0] -eq 11 -and $s.deathHold -eq 4.2 -and $s.look -eq 0.23) 'Resetting Attached preserves Lock-on settings'
    foreach ($mode in @('lockon','attach')) {
        Prepare $mode 1 $false
        $null = Send 'demo_resume' 2.8
        $null = Send 'demo_pause' 0.2
        $before = State
        Check ($before.enabled -and $before.targetDead -and $before.targetValid -and $before.deathElapsed -gt 1) "$mode continues following the corpse beyond Death hold"
        $null = Send 'mirv_filmmaker follow campose' 2
        $after = State
        Check ($after.enabled -and [Math]::Abs($after.deathElapsed-$before.deathElapsed) -lt 0.02) "$mode pause preserves elapsed hold time"
        $results += @{mode=$mode;case='keep-and-pause';before=$before;after=$after}
        foreach ($speed in @(1,0.25)) {
            Prepare $mode $speed $true
            $null = Send 'demo_resume' (2.8/$speed)
            $null = Send 'demo_pause' 0.2
            $s = State
            Check (!$s.enabled -and $s.message -eq 'Tracking stopped: death hold finished.') "$mode timed hold finishes at playback speed $speed"
            $results += @{mode=$mode;case='timed';speed=$speed;state=$s}
        }
    }
} finally {
    $results | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $out 'results.json')
    $null = Send 'demo_pause;;demo_timescale 1;;mirv_filmmaker follow stop'
}
