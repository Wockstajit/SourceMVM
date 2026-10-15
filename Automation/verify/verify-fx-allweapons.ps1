#requires -Version 5
# Verifies the Config-panel EFFECTS particle swaps against a purpose-made "shoot every
# weapon" demo (Docs/features/filmmaker_effects_modifiers.md). For each requested mode profile it
# replays the demo with `fx log on`, then asserts from `fx names` (the hook's own ground
# truth) that every expected effect group was BOTH exercised by the demo (seen > 0) and
# acted on by the hook (acted > 0): muzzle flashes, tracers, sustained-fire muzzle smoke,
# HE grenade, bomb, molotov/incendiary. Weapon-path systems the demo creates that no rule
# acts on (grenade trails, thrown-projectile fx, ...) are written to
# unmapped-<profile>.txt -- that list is exactly where new variant-table entries should
# come from (repo rule: map from live `fx names`, never from pak listings).
#
# Periodic full-screen captures land in the run folder for visual review (molotov white
# square, trails following grenades, muzzle smoke alignment).
#
# Requires CS2 already running with netcon (Automation/launch/launch-cs2-netcon.ps1) and
# the converted FX pack mounted. The demo must live under game/csgo (playdemo path rules).
[CmdletBinding()]
param(
    [int]$Port = 29010,
    # playdemo argument, relative to game/csgo. The default is the user's all-weapons
    # test demo (note the space before .dem -- it is part of the real filename).
    [string]$DemoName = 'all weapon test .dem',
    # Mode profiles to verify, in order. Each is a full category->mode assignment.
    [ValidateSet('modern', 'classic', 'less')]
    [string[]]$Profiles = @('modern', 'less'),
    [double]$Timescale = 2.0,
    # Demo end detection: stop when the hook's seen-counter has not moved for this long.
    [int]$StallSeconds = 25,
    [int]$MaxMinutes = 20,
    [int]$ScreenshotEverySeconds = 12,
    [switch]$SkipDemoLoad,   # demo already playing; just apply profile + observe
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-fx-allweapons'
} else {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-fx-allweapons' -Additional @{
    port = $Port
    demoName = $DemoName
    profiles = @($Profiles)
    timescale = $Timescale
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

# `mirv_filmmaker fx state` prints "[fx][state] {json}" on one line. The json itself
# contains a nested "modes":{...} object, so a lazy \{.*?\} stops at that inner '}' and
# yields invalid JSON -- must match the OUTER braces (first '{' .. matching last '}'),
# found here by counting depth rather than regex backtracking tricks.
function Read-FxState {
    $text = Send 'mirv_filmmaker fx state' 1.0
    $markerIdx = $text.IndexOf('[fx][state]')
    if ($markerIdx -lt 0) { return $null }
    $braceStart = $text.IndexOf('{', $markerIdx)
    if ($braceStart -lt 0) { return $null }
    $depth = 0
    for ($i = $braceStart; $i -lt $text.Length; $i++) {
        if ($text[$i] -eq '{') { $depth++ }
        elseif ($text[$i] -eq '}') {
            $depth--
            if ($depth -eq 0) {
                try { return ($text.Substring($braceStart, $i - $braceStart + 1) | ConvertFrom-Json) }
                catch { return $null }
            }
        }
    }
    return $null
}

# `fx names <filter>` rows: "  <seen> <acted>  <name>" (top 60 per call, so callers pass
# narrow filters and results are merged by name).
function Read-FxNames([string]$filter) {
    $text = Send ("mirv_filmmaker fx names {0}" -f $filter) 1.2
    $rows = @{}
    foreach ($match in [regex]::Matches($text, '(?m)^\s+(\d+)\s+(\d+)\s+(particles/\S+)\s*$')) {
        $rows[$match.Groups[3].Value] = [pscustomobject]@{
            name  = $match.Groups[3].Value
            seen  = [long]$match.Groups[1].Value
            acted = [long]$match.Groups[2].Value
        }
    }
    return $rows
}

# The whole-monitor capturer grabs whatever is on top of the screen, which is NOT
# reliably the CS2 window (other windows, other monitors). Use the game-window-specific
# capturer so screenshots are always the actual game viewport.
$capture = Join-Path $automationRoot 'capture\capture-game-window.ps1'
function Snap([string]$label) {
    if (Test-Path $capture) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Process cs2 -Mode client `
            -Out (Join-Path $OutDir ("{0}.png" -f $label)) | Out-Null
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

function Get-FxCategoryMode([object]$state, [string]$category) {
    if (-not $state -or -not $state.modes) { return 'off' }
    $prop = $state.modes.PSObject.Properties[$category]
    if ($prop) { return [string]$prop.Value }
    return 'off'
}

# Category modes per profile. 'modern' = the MW2019 pack everywhere it exists, 'classic'
# = Povarehok On everywhere, 'less' = the mod's reduced variants where they exist.
$profileModes = @{
    modern  = @{ impacts = 'less'; tracers = 'modern'; weaponfx = 'modern';
                 explosions = 'modern'; bombfx = 'on'; molotov = 'on' }
    classic = @{ impacts = 'on'; tracers = 'on'; weaponfx = 'on';
                 explosions = 'on'; bombfx = 'on'; molotov = 'on' }
    less    = @{ impacts = 'less'; tracers = 'on'; weaponfx = 'on';
                 explosions = 'less'; bombfx = 'less'; molotov = 'on' }
}

# Expected effect groups: (label, fx-names filter, name substrings that count, requireActed).
# seen>0 proves the demo exercised the effect; acted>0 proves the hook swapped it.
# Substrings chosen from the variant tables in AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp.
$expectedGroups = @(
    @{ label = 'Muzzle flash (any weapon class)'; filter = 'muz';        match = @('uweapon_muzflsh', 'uweapon_muzzleflash', 'weapon_muzzleflash'); requireActed = $true },
    @{ label = 'Silenced muzzle flash';           filter = 'muzsilenced'; match = @('uweapon_muzsilenced'); requireActed = $true },
    @{ label = 'Bullet tracers';                  filter = 'tracer';     match = @('weapon_tracers'); requireActed = $true },
    @{ label = 'Sustained-fire muzzle smoke (On/Less)'; filter = 'muzzle_smoke'; match = @('weapon_muzzle_smoke'); requireActed = $true; profiles = @('classic', 'less') },
    # Modern barrel smoke is a PCF child of the swapped class flash (never a top-level
    # fx-names row). Prove the per-shot path by the same muzzle-flash swap signal.
    @{ label = 'Barrel smoke (Modern per-shot child)'; filter = 'muz'; match = @('uweapon_muzflsh', 'uweapon_muzzleflash', 'weapon_muzzleflash'); requireActed = $true; profiles = @('modern') },
    @{ label = 'Bullet impacts';                  filter = 'impact_fx';  match = @('particles/impact_fx/impact_'); requireActed = $true },
    @{ label = 'HE grenade explosion';            filter = 'explosion';  match = @('explosion_hegrenade', 'explosion_basic'); requireActed = $true },
    @{ label = 'Bomb (C4) blast';                 filter = 'explosion_c4'; match = @('explosion_c4'); requireActed = $true },
    @{ label = 'Molotov / incendiary fire';       filter = 'inferno';    match = @('molotov_groundfire', 'incendiary_groundfire', 'molotov_fire01', 'molotov_explosion', 'incendiary_explosion'); requireActed = $true },
    # Coverage-only groups (pass-through by design today; listed so a demo that skips
    # them fails loudly and so trail candidates always land in the unmapped report).
    @{ label = 'Shell casings (coverage)';        filter = 'shell';      match = @('weapon_shell_casing'); requireActed = $false },
    @{ label = 'Molotov/incendiary thrown trail (coverage)'; filter = 'thrown'; match = @('molotov_thrown', 'incend_thrown', 'incgrenade_thrown'); requireActed = $false }
)

# fx-names filters that cover every path family the report cares about (60-row cap per
# call makes one unfiltered dump unreliable on a demo this dense). Narrow filters for
# lower-frequency systems (sustained muzzle smoke, ground dust) get their OWN sweep so a
# high-frequency generic filter (e.g. 'muz' matching every flash on every shot) can't
# crowd them out of the shared 60-row cap.
$sweepFilters = @('muz', 'tracer', 'smoke', 'muzzle_smoke', 'ground_smoke', 'shocksmoke',
    'shell', 'impact', 'explosion', 'inferno', 'molotov', 'incend', 'thrown', 'trail',
    'blood', 'weapons/cs_weapon_fx', 'unified_weapon_fx')

Write-Host '=== All-weapons FX verifier ===' -ForegroundColor Cyan

$state = Read-FxState
if (-not $state) {
    Write-Host '[FAIL] mirv_filmmaker fx state returned nothing (hook DLL not loaded?).' -ForegroundColor Red
    $client.Close()
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    exit 1
}
# Restore the user's persisted modes when done (SetMode auto-saves).
$savedEnabled = [bool]$state.enabled
$savedMoneyshot = [bool]$state.moneyHeadshot
$savedModes = @{}
foreach ($p in $state.modes.PSObject.Properties) { $savedModes[$p.Name] = $p.Value }

# Lazy-precache state checks. Start from a known all-Off configuration, then prove that
# selecting one category or auxiliary feature is sufficient to opt in, while master Off
# always cancels the pending queue. Exact target names stay an internal implementation
# detail; the all-other-categories-Off assertions prove the queue planner has one source.
$categoryKeys = @('impacts', 'tracers', 'weaponfx', 'explosions', 'bombfx', 'molotov')
foreach ($category in $categoryKeys) {
    Send ("mirv_filmmaker fx set {0} off quiet" -f $category) 0.05 | Out-Null
}
Send 'mirv_filmmaker fx moneyshot off' 0.05 | Out-Null
Send 'mirv_filmmaker fx off quiet' 0.05 | Out-Null
$offState = Read-FxState
Check '[lazy] master Off reports an empty resolve queue' ($offState -and -not $offState.enabled -and [int]$offState.resolveQueue -eq 0)
$allModesOff = $offState -ne $null
if ($offState) {
    foreach ($category in $categoryKeys) {
        if ((Get-FxCategoryMode $offState $category) -ne 'off') { $allModesOff = $false }
    }
}
Check '[lazy] all eight categories can remain Off' $allModesOff

Send 'mirv_filmmaker fx set impacts on quiet' 0.05 | Out-Null
$oneCategory = Read-FxState
$onlyImpacts = $oneCategory -and $oneCategory.enabled -and (Get-FxCategoryMode $oneCategory 'impacts') -eq 'on'
if ($oneCategory) {
    foreach ($category in ($categoryKeys | Where-Object { $_ -ne 'impacts' })) {
        if ((Get-FxCategoryMode $oneCategory $category) -ne 'off') { $onlyImpacts = $false }
    }
}
Check '[lazy] selecting one non-Off category auto-enables master without changing other modes' $onlyImpacts
$queueBeforeRepeat = if ($oneCategory) { [int]$oneCategory.resolveQueue } else { 0 }
Send 'mirv_filmmaker fx set impacts on quiet' 0.05 | Out-Null
$repeatState = Read-FxState
Check '[lazy] repeating a selection does not grow the pending queue' `
    ($repeatState -and [int]$repeatState.resolveQueue -le $queueBeforeRepeat)

Send 'mirv_filmmaker fx set impacts less quiet' 0.05 | Out-Null
$switchState = Read-FxState
Check '[lazy] switching packs replaces the selected mode' `
    ($switchState -and $switchState.enabled -and (Get-FxCategoryMode $switchState 'impacts') -eq 'less')
Send 'mirv_filmmaker fx off quiet' 0.05 | Out-Null
$cancelState = Read-FxState
Check '[lazy] disabling master cancels remaining precache work' `
    ($cancelState -and -not $cancelState.enabled -and [int]$cancelState.resolveQueue -eq 0)

Send 'mirv_filmmaker fx moneyshot on' 0.05 | Out-Null
$moneyState = Read-FxState
Check '[lazy] enabling Money on Headshot auto-enables master' `
    ($moneyState -and $moneyState.enabled -and $moneyState.moneyHeadshot)
Send 'mirv_filmmaker fx moneyshot off' 0.05 | Out-Null
Send 'mirv_filmmaker fx off quiet' 0.05 | Out-Null

$ruleToken = '__mvm_lazy_verify_' + (Get-Date -Format 'yyyyMMddHHmmssfff')
$beforeRule = Read-FxState
Send ("mirv_filmmaker fx block {0}" -f $ruleToken) 0.05 | Out-Null
$ruleState = Read-FxState
Check '[lazy] adding a custom rule auto-enables master' `
    ($beforeRule -and $ruleState -and $ruleState.enabled -and `
     [int]$ruleState.customRules -eq ([int]$beforeRule.customRules + 1))
Send ("mirv_filmmaker fx unblock {0}" -f $ruleToken) 0.05 | Out-Null
Send 'mirv_filmmaker fx off quiet' 0.05 | Out-Null

Send 'mirv_filmmaker fx on quiet' | Out-Null
Send 'mirv_filmmaker fx debughud on' | Out-Null
Send 'mvm_debug start' 0.4 | Out-Null

$report = [ordered]@{ profiles = [ordered]@{} }

foreach ($profile in $Profiles) {
    Write-Host ("--- Profile '{0}' ---" -f $profile) -ForegroundColor Cyan
    foreach ($entry in $profileModes[$profile].GetEnumerator()) {
        Send ("mirv_filmmaker fx set {0} {1} quiet" -f $entry.Key, $entry.Value) 0.3 | Out-Null
    }
    Send 'mirv_filmmaker fx log on' 0.4 | Out-Null

    if (-not $SkipDemoLoad) {
        Write-Host ("Loading demo: {0}" -f $DemoName)
        Send ('playdemo "{0}"' -f $DemoName) 2.0 | Out-Null
        # The hook arms once a demo/map is loaded, but `installed` stays true from any
        # previous demo -- so demand the seen-counter actually MOVES (particles being
        # created) before trusting that the demo is really playing.
        $preLoad = Read-FxState
        $preSeen = if ($preLoad) { [long]$preLoad.seen } else { 0 }
        $loadDeadline = (Get-Date).AddSeconds(120)
        $armed = $false
        while ((Get-Date) -lt $loadDeadline) {
            Start-Sleep -Seconds 3
            $state = Read-FxState
            if ($state -and $state.installed -and [long]$state.seen -gt $preSeen) { $armed = $true; break }
        }
        Check ("[{0}] Demo loaded, hook armed, particles flowing" -f $profile) $armed
        if (-not $armed) { continue }
    } else {
        # Demo is already open (playdemo over netcon is unreliable) -- rewind to the start
        # so every profile gets a full, identical pass instead of riding wherever the demo
        # happened to be paused/scrubbed to.
        Send 'demo_gototick 0' 1.0 | Out-Null
    }
    # ALWAYS set playback speed + resume, even in -SkipDemoLoad mode -- otherwise the
    # observe loop just rides whatever speed/pause state the demo was left in (seen
    # 2026-07-03: a run crawled through 26s of demo content over 606s of wall time because
    # SkipDemoLoad had skipped these two lines).
    Send ("demo_timescale {0}" -f $Timescale) 0.3 | Out-Null
    Send 'demo_resume' 0.3 | Out-Null

    # Observe until the demo stops producing particle creations.
    $baseline = Read-FxState
    $lastSeen = if ($baseline) { [long]$baseline.seen } else { 0 }
    $lastGrowth = Get-Date
    $started = Get-Date
    $lastSnap = Get-Date
    $snapIndex = 0
    while ($true) {
        Start-Sleep -Seconds 5
        $now = Get-Date
        $state = Read-FxState
        if ($state -and [long]$state.seen -gt $lastSeen) {
            $lastSeen = [long]$state.seen
            $lastGrowth = $now
        }
        if (($now - $lastSnap).TotalSeconds -ge $ScreenshotEverySeconds) {
            Snap ("{0}-t{1:000}" -f $profile, [int]($now - $started).TotalSeconds)
            $lastSnap = $now
            $snapIndex++
        }
        if (($now - $lastGrowth).TotalSeconds -ge $StallSeconds) { break }
        if (($now - $started).TotalMinutes -ge $MaxMinutes) {
            Write-Warning "MaxMinutes reached; collecting results anyway."
            break
        }
    }
    Send 'demo_pause' 0.3 | Out-Null
    Send 'demo_timescale 1' 0.3 | Out-Null
    Write-Host ("Playback observed for {0:n0}s, {1} creations, {2} screenshots." -f `
        ((Get-Date) - $started).TotalSeconds, $lastSeen, $snapIndex)

    # Collect the aggregated name table.
    $names = @{}
    foreach ($filter in $sweepFilters) {
        foreach ($row in (Read-FxNames $filter).GetEnumerator()) { $names[$row.Key] = $row.Value }
    }
    $names.Values | Sort-Object -Property seen -Descending |
        ForEach-Object { '{0,8} {1,6}  {2}' -f $_.seen, $_.acted, $_.name } |
        Set-Content -LiteralPath (Join-Path $OutDir ("fx-names-{0}.txt" -f $profile)) -Encoding UTF8
    (Send 'mirv_filmmaker fx recent 120' 1.5) |
        Set-Content -LiteralPath (Join-Path $OutDir ("fx-recent-{0}.txt" -f $profile)) -Encoding UTF8

    # Assertions per expected group (optional profile filter).
    foreach ($group in $expectedGroups) {
        $groupProfiles = if ($group.ContainsKey('profiles')) { $group['profiles'] } else { $null }
        if ($groupProfiles -and ($groupProfiles -notcontains $profile)) { continue }
        $hits = @($names.Values | Where-Object {
            $row = $_
            @($group.match | Where-Object { $row.name -like ('*' + $_ + '*') }).Count -gt 0
        })
        # Measure-Object on an empty collection returns $null, not a zeroed object --
        # guard the .Sum access or PowerShell throws PropertyNotFoundException.
        $seen = 0; $acted = 0
        if ($hits.Count -gt 0) {
            $seen = ($hits | Measure-Object -Property seen -Sum).Sum
            $acted = ($hits | Measure-Object -Property acted -Sum).Sum
        }
        Check ("[{0}] {1}: demo exercised it (seen)" -f $profile, $group.label) ($seen -gt 0)
        if ($group.requireActed) {
            Check ("[{0}] {1}: hook swapped it (acted)" -f $profile, $group.label) ($acted -gt 0)
        }
    }

    # Unmapped report: weapon/explosion/inferno systems the demo created that nothing
    # acted on. These are the candidates for new variant-table entries (grenade trails,
    # thrown fx, muzzle variants a CS2 update may have added).
    $unmapped = @($names.Values | Where-Object {
        $_.acted -eq 0 -and (
            $_.name -like 'particles/weapons/cs_weapon_fx/*' -or
            $_.name -like 'particles/unified_weapon_fx/*' -or
            $_.name -like 'particles/explosions_fx/*' -or
            $_.name -like 'particles/entity/env_explosion/*' -or
            $_.name -like 'particles/inferno_fx/*' -or
            $_.name -like 'particles/burning_fx/*'
        )
    } | Sort-Object -Property seen -Descending)
    $unmapped | ForEach-Object { '{0,8}  {1}' -f $_.seen, $_.name } |
        Set-Content -LiteralPath (Join-Path $OutDir ("unmapped-{0}.txt" -f $profile)) -Encoding UTF8
    Write-Host ("Unmapped weapon-path systems this profile: {0} (see unmapped-{1}.txt)" -f `
        $unmapped.Count, $profile) -ForegroundColor DarkYellow

    $endState = Read-FxState
    $report.profiles[$profile] = [ordered]@{
        state    = $endState
        names    = @($names.Values | Sort-Object -Property seen -Descending)
        unmapped = @($unmapped | ForEach-Object { $_.name })
    }
    Send 'mirv_filmmaker fx log off' 0.3 | Out-Null

    if (-not $SkipDemoLoad) {
        Send 'mirv_filmmaker fx off quiet' 0.2 | Out-Null
        Send 'disconnect' 1.5 | Out-Null
        Start-Sleep -Seconds 4
        $offTransition = Read-FxState
        Check ("[{0}] level exit while FX Off leaves the queue empty" -f $profile) `
            ($offTransition -and -not $offTransition.enabled -and [int]$offTransition.resolveQueue -eq 0)
    }
}

# Restore the user's persisted category modes and auxiliary/master state. Restore master
# last because non-Off modes and Money on Headshot intentionally auto-enable it.
foreach ($entry in $savedModes.GetEnumerator()) {
    Send ("mirv_filmmaker fx set {0} {1} quiet" -f $entry.Key, $entry.Value) 0.25 | Out-Null
}
Send ("mirv_filmmaker fx moneyshot {0}" -f $(if ($savedMoneyshot) { 'on' } else { 'off' })) 0.25 | Out-Null
Send ("mirv_filmmaker fx {0} quiet" -f $(if ($savedEnabled) { 'on' } else { 'off' })) 0.25 | Out-Null
$client.Close()

$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'report.json') -Encoding UTF8

$failures = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
Write-Host "Artifacts: $OutDir"
if ($transcriptStarted) { Stop-Transcript | Out-Null }
if ($failures -eq 0) {
    Write-Host "`nALL FX COVERAGE CHECKS PASSED (review screenshots + unmapped-*.txt for visual issues)" -ForegroundColor Green
    exit 0
}
Write-Host "`n$failures CHECK(S) FAILED" -ForegroundColor Red
exit 1
