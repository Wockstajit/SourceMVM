# With a repaired offline demo open through live.bat, replay movement and weapon swaps.
param(
    [Parameter(Mandatory=$true)][int]$UserId,
    [Parameter(Mandatory=$true)][int]$MovementTick,
    [Parameter(Mandatory=$true)][int]$DeathTick,
    [Parameter(Mandatory=$true)][int]$WeaponSwitchTick,
    [int]$Port = 29010,
    [string]$OutDir = 'build/verification/follow-mount'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = [IO.Path]::GetFullPath((Join-Path $repo $OutDir))
New-Item -ItemType Directory -Path $out -Force | Out-Null
$script:step = 0
function Send([string]$command, [double]$read=0.15) {
    $script:step++
    $log = Join-Path $out ('netcon-{0:d3}.txt' -f $script:step)
    $null = & (Join-Path $repo 'Automation/netcon/cs2-netcon.ps1') -Port $Port -ReadSeconds $read -CommandBatch $command -LogPath $log 6>&1
    return [IO.File]::ReadAllText($log)
}
function State {
    $text = Send 'mirv_filmmaker follow state64' 0.4
    $parts = @{}; $expected = 0; $encoded = ''
    foreach ($c in [regex]::Matches($text, '\[followcam\]\[state64\]\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)')) {
        $index = [int]$c.Groups[1].Value
        if ($index -eq 1) { $parts = @{}; $expected = [int]$c.Groups[2].Value }
        $parts[$index] = $c.Groups[3].Value
        if ($index -eq $expected -and $parts.Count -eq $expected) {
            $encoded = (1..$expected | ForEach-Object { $parts[$_] }) -join ''
        }
    }
    if (!$encoded) { throw 'No complete Follow state received.' }
    return ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded)) | ConvertFrom-Json)
}
$results = @()
try {
    $null = Send 'mirv_filmmaker editor on;;mirv_filmmaker health;;mvm_debug start'
    foreach ($case in @(
        @{mount='head';tick=$MovementTick;seconds=8;type='player'},
        @{mount='weapon_hand_R';tick=($DeathTick-300);seconds=6;type='player'},
        @{mount='weapon_hand_L';tick=$MovementTick;seconds=8;type='player'},
        @{mount='muzzle';tick=$WeaponSwitchTick;seconds=13;type='weapon'}
    )) {
        $null = Send ('mirv_filmmaker follow stop;;demo_timescale 1;;demo_gototick {0};;demo_pause' -f $case.tick) 0.3
        $s = State
        $player = @($s.players | Where-Object { $_.userId -eq $UserId -and $_.alive })
        if ($player.Count -ne 1) { throw 'Player not alive at the requested tick.' }
        $selection = if ($case.type -eq 'weapon') { 'weaponsource held;;mirv_filmmaker follow weaponplayer' } else { 'select' }
        $null = Send ('mirv_filmmaker follow mode attach;;mirv_filmmaker follow type {0};;mirv_filmmaker follow {1} {2};;mirv_filmmaker follow attachment {3};;mirv_filmmaker follow reset tracking;;mirv_filmmaker follow autodead 0;;mirv_filmmaker follow offset -24 12 8;;mirv_filmmaker follow rotation 15 35 12;;mirv_filmmaker follow preview' -f $case.type,$selection,$player[0].index,$case.mount)
        $before = State
        if (!$before.enabled -or !$before.targetValid) { throw "Invalid initial mount: $($case.mount)" }
        if ($case.type -eq 'weapon' -and $before.attachDebug.source -notmatch '^(model-attachment|alias-)') { throw 'Gun mount fell back instead of using its native attachment.' }
        if ($case.mount -eq 'head' -and $before.attachDebug.source -ne 'bone-clip_limit') { throw 'Head mount is not following the skeleton.' }
        $null = Send 'demo_resume' $case.seconds
        $null = Send 'demo_pause' 0.3
        $after = State
        if (!$after.enabled -or !$after.targetValid -or $after.attachment -ne $case.mount) { throw "Mount lost: $($case.mount)" }
        if (!$after.attachDebug.oriented) { throw "Mount lacks orientation: $($case.mount)" }
        if ($case.type -eq 'weapon' -and $after.attachDebug.source -notmatch '^(model-attachment|alias-)') { throw 'Returning gun did not recover its native attachment.' }
        $results += @{case=$case;before=$before;after=$after}
        Write-Output "PASS: $($case.mount) remained selected and oriented through playback"
    }
} finally {
    $null = Send 'demo_pause;;mirv_filmmaker follow stop;;mvm_debug stop'
    $results | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $out 'results.json')
}
