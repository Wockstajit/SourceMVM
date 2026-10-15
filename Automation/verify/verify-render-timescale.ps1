# Verifies the IWXMVM-style capture timing model end to end against a live CS2 demo session:
# the timeline speed dial (mirv_filmmaker timescale) is the game-speed source for render jobs,
# and each output frame advances demo time by exactly timescale/fps (sampling at fps/timescale).
#
# Runs one job per dial value (default 1.0, 0.01, 0.001), then checks per job:
#   * manifest carries the snapshotted timescale + captureFps and completed with every frame;
#   * frames.csv row count == framesExpected;
#   * the demo_tick column advances ~captureFps/64 frames per tick (the slow-mo staircase);
#   * total tick span matches the requested range.
#
# Standalone on purpose (no-coupling rule): drives only the shipped console surface.
#
#   powershell -ExecutionPolicy Bypass -File Automation\verify\verify-render-timescale.ps1 `
#       -OutRoot C:\path\to\out
param(
    [int]$Port = 29010,
    [Parameter(Mandatory = $true)]
    [string]$OutRoot,
    [int]$JobTimeoutSeconds = 1200
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$netcon = Join-Path $automationRoot 'netcon\cs2-netcon.ps1'
if (-not (Test-Path -LiteralPath $netcon)) { throw "netcon helper not found: $netcon" }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$renderRoot = Join-Path $OutRoot 'takes'
New-Item -ItemType Directory -Force -Path $renderRoot | Out-Null

$netconLog = Join-Path $OutRoot 'netcon.log'
function Send-Cmds([string]$batch, [double]$read = 0.8) {
    if (Test-Path -LiteralPath $netconLog) { Remove-Item -LiteralPath $netconLog -Force }
    & $netcon -Port $Port -CommandBatch $batch -ReadSeconds $read -LogPath $netconLog *> $null
    if (-not (Test-Path -LiteralPath $netconLog)) { return '' }
    return (Get-Content -Raw -LiteralPath $netconLog)
}

function Wait-Manifest([string]$takeDir) {
    $deadline = (Get-Date).AddSeconds($JobTimeoutSeconds)
    $manifest = Join-Path $takeDir 'manifest.json'
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $manifest) {
            $j = $null
            try { $j = Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json } catch { }
            if ($j -and @('completed','failed','canceled') -contains $j.status) { return $j }
        }
        Start-Sleep -Seconds 3
    }
    return $null
}

$health = Send-Cmds 'mirv_filmmaker health' 1.0
if ([string]::IsNullOrWhiteSpace($health)) { throw 'no netcon reply to mirv_filmmaker health' }
"OK health replied"

# WAIT for the demo to actually be playing. Starting while it still loads means every netcon
# reply is buried in map-load spam and the confirmations below are missed -- measured 2026-08-10,
# where the folder command "failed" purely because its reply window caught the loading screen.
$demoReady = $false
for ($i = 0; $i -lt 60; $i++) {
    $b = Send-Cmds 'mirv_filmmaker render range full' 1.2
    if ($b -match 'FullDemo \((\d+) \.\. (\d+)\)') { $demoReady = $true; break }
    Start-Sleep -Seconds 2
}
if (-not $demoReady) { throw 'demo never reached a playable state (no demo bounds)' }
"OK demo playing"

Send-Cmds 'mirv_filmmaker editor on' 1.5 | Out-Null
Send-Cmds 'mirv_filmmaker render open' 1.0 | Out-Null
# Retry the folder: one dropped/￼noisy reply must not fail the whole run.
$pathOut = ''
for ($i = 0; $i -lt 5; $i++) {
    $pathOut = Send-Cmds "mirv_filmmaker render output path `"$renderRoot`"" 1.5
    if ($pathOut -match 'output folder set to') { break }
    Start-Sleep -Seconds 1
}
if ($pathOut -notmatch 'output folder set to') { throw "could not set the render output folder:`n$pathOut" }
Send-Cmds 'mirv_filmmaker render output format mp4' | Out-Null
# 32 fps divides the 64 tick rate, which is the documented condition for frame-exact
# repeatability -- the determinism check below is only meaningful at such a rate.
Send-Cmds 'mirv_filmmaker render output fps 32' | Out-Null
Send-Cmds 'mirv_filmmaker render camera current' | Out-Null
Send-Cmds 'mirv_filmmaker render pass hud none' | Out-Null

# Realistic ranges (users render 40-50+ ticks minimum), anchored at a MID-DEMO action moment --
# 45% into the demo, where players are actually moving -- not the standing-around round start.
# The 1x case runs TWICE over the same range: two takes of the same range must be tick-identical
# (the alignment future multi-take passes ride on).
$cases = @(
    @{ scale = 1.0;   ticks = 64; label = '1x-a' },
    @{ scale = 1.0;   ticks = 64; label = '1x-b' },
    @{ scale = 0.01;  ticks = 48; label = '0.01x' },
    @{ scale = 0.001; ticks = 48; label = '0.001x' }
)

$failures = 0
$tickSequences = @{}
$full = Send-Cmds 'mirv_filmmaker render range full' 1.0
if ($full -notmatch 'FullDemo \((\d+) \.\. (\d+)\)') { throw "demo bounds unavailable:`n$full" }
$demoStart = [int]$Matches[1]; $demoEnd = [int]$Matches[2]
# 45% into the demo lands on/near a round start (freezetime, nobody moving); push the anchor
# ~19s further so the take covers actual mid-round gameplay with movement.
$sharedStartTick = $demoStart + [int](0.45 * ($demoEnd - $demoStart)) + 1200
"mid-demo anchor: tick $sharedStartTick (demo $demoStart..$demoEnd)"
Send-Cmds 'mirv_filmmaker render range custom' 1.0 | Out-Null
foreach ($case in $cases) {
    $scale = $case.scale; $ticks = $case.ticks; $label = $case.label
    "---- $label (dial $scale x, range $ticks tick(s)) ----"
    # ASSERT the dial actually took. Measured 2026-08-10: a dropped netcon command left the dial
    # on the previous case's value and the job silently re-ran the earlier speed (manifest said
    # 0.01 while the case claimed 0.001), which reads as a render bug but is a harness flake.
    $tsOut = Send-Cmds "mirv_filmmaker timescale $scale" 1.2
    $tsNow = Send-Cmds 'mirv_filmmaker timescale' 1.2
    if ($tsNow -notmatch [regex]::Escape("currently $scale")) {
        $tsOut2 = Send-Cmds "mirv_filmmaker timescale $scale" 1.5
        $tsNow = Send-Cmds 'mirv_filmmaker timescale' 1.2
        if ($tsNow -notmatch [regex]::Escape("currently $scale")) {
            "FAIL ${label}: timescale dial did not accept $scale (reply: $($tsNow -replace '\s+',' '))"
            $failures++
            continue
        }
    }

    $startTick = $sharedStartTick
    Send-Cmds "mirv_filmmaker render setrange $startTick $($startTick + $ticks)" | Out-Null

    $before = @{}
    foreach ($d in @(Get-ChildItem -LiteralPath $renderRoot -Directory -ErrorAction SilentlyContinue)) { $before[$d.FullName] = $true }
    $startOut = Send-Cmds 'mirv_filmmaker render start' 1.5
    if ($startOut -match 'cannot start') { throw "render start refused at ${label}:`n$startOut" }

    $takeDir = $null
    $spot = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $spot) {
        $new = @(Get-ChildItem -LiteralPath $renderRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { -not $before.ContainsKey($_.FullName) })
        if ($new.Count -gt 0) { $takeDir = $new[0].FullName; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $takeDir) { throw "no take directory appeared at $label" }

    $m = Wait-Manifest $takeDir
    if (-not $m) { throw "job at $label timed out" }
    if ($m.status -ne 'completed') { "FAIL ${label}: job $($m.status): $($m.notes)"; $failures++; continue }

    $expectCaptureFps = [math]::Round(32.0 / $scale)
    $ok = $true
    if ([math]::Abs($m.timescale - $scale) -gt 1e-9) { "FAIL ${label}: manifest timescale=$($m.timescale)"; $ok = $false }
    if ($m.captureFps -ne $expectCaptureFps) { "FAIL ${label}: manifest captureFps=$($m.captureFps), expected $expectCaptureFps"; $ok = $false }
    if ($m.framesWritten -ne $m.framesExpected) { "FAIL ${label}: framesWritten=$($m.framesWritten) != expected=$($m.framesExpected)"; $ok = $false }

    $csv = Import-Csv (Join-Path $takeDir 'frames.csv')
    if ($csv.Count -ne $m.framesExpected) { "FAIL ${label}: frames.csv rows=$($csv.Count) != $($m.framesExpected)"; $ok = $false }

    # Staircase: unique demo ticks spanned, and mean frames-per-tick vs captureFps/64.
    $tickVals = $csv | ForEach-Object { [int]$_.demo_tick }
    $tickSpan = ($tickVals | Measure-Object -Maximum).Maximum - ($tickVals | Measure-Object -Minimum).Minimum
    $framesPerTick = $csv.Count / [math]::Max(1, $tickSpan)
    $expectFramesPerTick = $expectCaptureFps / 64.0
    "     frames=$($csv.Count) tickSpan=$tickSpan framesPerTick=$([math]::Round($framesPerTick,2)) (expected $([math]::Round($expectFramesPerTick,2)))"
    if ($tickSpan -lt 1 -or $tickSpan -gt ($ticks + 2)) { "FAIL ${label}: tick span $tickSpan outside requested $ticks"; $ok = $false }
    if ([math]::Abs($framesPerTick - $expectFramesPerTick) -gt ($expectFramesPerTick * 0.15 + 1)) {
        "FAIL ${label}: frames-per-tick off (measured $framesPerTick, expected $expectFramesPerTick)"; $ok = $false
    }
    # Monotonic non-decreasing ticks: time never runs backwards inside a take.
    for ($i = 1; $i -lt $tickVals.Count; $i++) {
        if ($tickVals[$i] -lt $tickVals[$i-1]) { "FAIL ${label}: demo_tick went backwards at row $i"; $ok = $false; break }
    }
    # Start tick: within the measured 1-tick reading jitter of a paused demo (the tick reading
    # flickers by one while stopped; the world state under both readings is identical). Takes
    # still have to AGREE with each other -- that is checked after the loop.
    $startDev = [math]::Abs($tickVals[0] - $startTick)
    if ($startDev -gt 2) { "FAIL ${label}: frame 0 tick=$($tickVals[0]) is $startDev ticks off startTick=$startTick"; $ok = $false }
    else { "     frame0 tick $($tickVals[0]) (requested $startTick, dev $startDev)" }
    # Model: frame i should sample tick start + floor(i*64/captureFps), within one tick.
    $maxDev = 0
    $base = $tickVals[0]
    for ($i = 0; $i -lt $tickVals.Count; $i++) {
        $want = $base + [math]::Floor($i * 64.0 / $expectCaptureFps)
        $dev = [math]::Abs($tickVals[$i] - $want)
        if ($dev -gt $maxDev) { $maxDev = $dev }
    }
    # One sampling STEP of slack: at 1x a frame already spans 64/30 = 2.13 ticks, so a 1-tick
    # tolerance is tighter than the model's own quantisation. In slow motion the step is < 1 tick
    # and this collapses back to the strict check.
    $stepTicks = [math]::Ceiling(64.0 / $expectCaptureFps)
    $schedTol = [math]::Max(1, $stepTicks)
    "     max tick deviation from schedule: $maxDev (tolerance $schedTol)"
    if ($maxDev -gt $schedTol) { "FAIL ${label}: tick schedule deviates by $maxDev (>$schedTol)"; $ok = $false }
    $tickSequences[$label] = ($tickVals -join ',')
    # Container rate: the file must be muxed at the PLAYBACK fps (30), never the sampling rate --
    # otherwise the slow motion collapses back to real time on playback.
    $mp4 = Get-ChildItem -LiteralPath $takeDir -Recurse -Filter '*.mp4' | Select-Object -First 1
    if ($mp4) {
        $rate = & ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 $mp4.FullName 2>$null
        "     container rate: $rate"
        if ("$rate".Trim() -ne '32/1') { "FAIL ${label}: container rate '$rate' != 32/1"; $ok = $false }
    } else { "FAIL ${label}: no mp4 produced"; $ok = $false }
    if ($ok) { "OK ${label}: completed, timing matches the model" } else { $failures++ }
}

# Determinism: the two 1x takes over the same range must be tick-identical.
if ($tickSequences.ContainsKey('1x-a') -and $tickSequences.ContainsKey('1x-b')) {
    if ($tickSequences['1x-a'] -ne $tickSequences['1x-b']) { "FAIL: repeat takes of the same range differ tick-for-tick"; $failures++ }
    else { "OK repeat takes are tick-identical" }
}

Send-Cmds 'mirv_filmmaker timescale 1' | Out-Null
Send-Cmds 'mirv_filmmaker render close' | Out-Null
if ($failures -gt 0) { throw "$failures case(s) failed" }
"VERIFY-RENDER-TIMESCALE: PASS"
