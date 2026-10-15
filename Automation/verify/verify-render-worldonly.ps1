#requires -Version 5
<#
Live verification of the World Only render pass (Docs/agent/render-system-contract.md 9c).

Assumes CS2 is already up with a demo loaded (Automation\launch\demo.bat) and netcon answering.

What it proves, on ONE paused frame so before/after are the same shot:
  1. World Only removes subjects: players / viewmodel / weapons draw counts are all dropped,
     and none of those classes is ever kept.
  2. World Only keeps the world: world + prop draws are all kept and none is dropped.
  3. The frame actually changes (image diff before -> world only is above a floor).
  4. It is exactly reversible: turning it off restores the original frame (diff ~ 0).
  5. Nothing survives by accident: the mvm_debug probe log is scanned for a KEEP verdict on a
     subject-looking material path, which is how a leftover object is traced to its rule.

Artifacts (screenshots, probe log slice, result json): Automation\output\render_worldonly\
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [int]$GotoTick = 22134,
    [int]$ProbeNames = 400,
    [string]$OutDir = "Automation\output\render_worldonly",
    # Diff floor for "the frame visibly changed". Deliberately low: a shot may contain only one
    # small subject, and this test must not demand a busy frame to pass.
    [double]$MinChange = 0.05,
    # Diff ceiling for "the frame came back". Not exactly 0: CS2 keeps animating sky/cloud
    # materials and dust particles even while the demo is paused.
    [double]$MaxRestoreDiff = 2.0,
    # Diffs are measured on the 3D viewport only. Source MVM's own chrome (director panel top
    # right, controls card left, timeline bottom, scoreboard top) carries a live clock, so
    # including it would put a floating baseline under every comparison. 1600x1200 default.
    [string]$Crop = "300,220,1320,1080"
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'
$logFile = Join-Path $outAbs 'worldonly.log'
"=== verify-render-worldonly $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$m) { $failures.Add($m); Write-Host "  FAIL  $m" -ForegroundColor Red }
function Pass([string]$m) { Write-Host "  ok    $m" -ForegroundColor Green }

function NC([string]$batch, [double]$read = 1.5) {
    $t = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') `
        -Port $Port -CommandBatch $batch -ReadSeconds $read | Out-String
    Add-Content -LiteralPath $logFile -Value ("--- $batch ---`n$t")
    return $t
}

function Capture([string]$name) {
    $path = Join-Path $outAbs $name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}

function Diff-Mean([string]$a, [string]$b) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $Crop
    $m = [regex]::Match(($json | Out-String), '"mean":\s*([0-9.]+)')
    if (-not $m.Success) { throw "image_diff produced no mean for $a vs $b" }
    return [double]$m.Groups[1].Value
}

# Per-class kept/dropped counters straight out of `render worldonly status`.
function Get-Counters([string]$statusText) {
    $c = @{}
    foreach ($line in ($statusText -split "`r?`n")) {
        $m = [regex]::Match($line, '^\s*(world|props|players|viewmodel|weapons|bomb)\s+kept=(\d+)\s+dropped=(\d+)')
        if ($m.Success) {
            $c[$m.Groups[1].Value] = @{ kept = [long]$m.Groups[2].Value; dropped = [long]$m.Groups[3].Value }
        }
    }
    return $c
}

Write-Host "World Only verification (tick $GotoTick)" -ForegroundColor Cyan

# --- 1. park the demo on one tick so before/after are the same shot -----------------------------
# Close the editor workspace first. Every diff here is about the 3D frame, and the Camera Editor
# is a large panel that re-lays itself out (a tab switch between two captures was measured as a
# 47-mean "regression" that had nothing to do with the filter). The console arms World Only
# directly, so nothing in this script needs the workspace open.
NC "mirv_filmmaker render close;;mirv_filmmaker editor off" 2.0 | Out-Null
NC "mvm_debug start;;mirv_filmmaker render worldonly off" 1.5 | Out-Null
NC "demo_gototick $GotoTick" 3.0 | Out-Null
Start-Sleep -Milliseconds 1200

# Every diff in this script assumes before/after are the SAME frame, so the demo has to be really
# stopped before the baseline is taken. Two measured facts make "just send demo_pause" unreliable:
# `demo_pause` is a TOGGLE, and `demo_gototick` does not dependably leave the demo paused (it came
# back Playing), while a long seek can still be settling when the next command arrives -- in which
# case the pause is simply lost and every diff then measures demo time instead of the filter.
#
# So don't assume: PROVE the frame is static by capturing twice and comparing, and re-issue the
# pause until it is. Resume-then-pause makes the toggle deterministic whichever semantics the
# build has.
function Wait-ForStaticFrame([int]$tries = 6) {
    for ($i = 0; $i -lt $tries; $i++) {
        NC "demo_resume" 0.6 | Out-Null
        NC "demo_pause" 1.0 | Out-Null
        Start-Sleep -Milliseconds 900
        $a = Capture 'probe-static-a.png'
        Start-Sleep -Milliseconds 800
        $b = Capture 'probe-static-b.png'
        $drift = Diff-Mean $a $b
        if ($drift -lt 0.5) { return $drift }
        Write-Host ("  (frame still moving, drift {0:N2} - re-pausing)" -f $drift) -ForegroundColor DarkYellow
    }
    return $null
}

$drift = Wait-ForStaticFrame
if ($null -eq $drift) {
    Write-Host "  FAIL  the demo would not hold still; diffs would be meaningless" -ForegroundColor Red
    exit 1
}
Write-Host ("  paused frame is static (drift {0:N3})" -f $drift)
$before = Capture '01-before.png'

# --- 2. arm and capture -------------------------------------------------------------------------
NC "mirv_filmmaker render worldonly probe $ProbeNames;;mirv_filmmaker render worldonly on" 1.5 | Out-Null
Start-Sleep -Milliseconds 1500
$after = Capture '02-worldonly.png'
$status = NC "mirv_filmmaker render worldonly status" 1.5
$counters = Get-Counters $status

# --- 3. disarm and confirm the frame comes back -------------------------------------------------
NC "mirv_filmmaker render worldonly off" 1.2 | Out-Null
Start-Sleep -Milliseconds 1500
$restored = Capture '03-restored.png'

$changed = Diff-Mean $before $after
$restoreDiff = Diff-Mean $before $restored
Write-Host ("  diff before->worldonly = {0:N3}   before->restored = {1:N3}" -f $changed, $restoreDiff)

# --- 4. assertions ------------------------------------------------------------------------------
foreach ($cls in @('players', 'viewmodel', 'weapons')) {
    if (-not $counters.ContainsKey($cls)) { Fail "status did not report the '$cls' class"; continue }
    if ($counters[$cls].kept -ne 0) { Fail "$cls draws were KEPT ($($counters[$cls].kept)) - the filter admits a subject" }
    else { Pass "$cls never kept" }
}
if ($counters.ContainsKey('players') -and $counters['players'].dropped -le 0) {
    Fail "no player draws were dropped - either the shot has no players or the filter never ran"
} else { Pass "player draws dropped: $($counters['players'].dropped)" }

foreach ($cls in @('world', 'props')) {
    if (-not $counters.ContainsKey($cls)) { Fail "status did not report the '$cls' class"; continue }
    if ($counters[$cls].dropped -ne 0) { Fail "$cls draws were DROPPED ($($counters[$cls].dropped)) - the map is being eaten" }
    else { Pass "$cls never dropped" }
}
if ($counters.ContainsKey('world') -and $counters['world'].kept -le 0) {
    Fail "no world draws were kept - the plate would be empty"
} else { Pass "world draws kept: $($counters['world'].kept)" }

if ($changed -lt $MinChange) { Fail "the frame barely changed (diff $changed < $MinChange) - nothing was removed on screen" }
else { Pass "the rendered frame changed (diff $changed)" }

# "The frame came back" is a RELATIVE question. A paused CS2 demo is not a still image -- tracers,
# smoke, sky and dust keep animating -- so an absolute ceiling alone is flaky (measured 0.06 on a
# quiet frame, 2.11 on the same range with tracers in flight). What actually distinguishes "restored"
# from "still filtered" is that the residual is far smaller than the removal effect itself.
$restoreOk = ($restoreDiff -le $MaxRestoreDiff) -or ($restoreDiff -lt ($changed * 0.5))
if (-not $restoreOk) { Fail "turning World Only off did not restore the frame (diff $restoreDiff; removal effect was $changed)" }
else { Pass "disarming restored the frame (diff $restoreDiff vs removal effect $changed)" }

# --- 4b. the bomb: kept only while planted, and ONLY the bomb ------------------------------------
# Two independent proofs, because "keep the planted bomb" is the one rule that could quietly
# readmit a whole weapon family:
#   (a) the counters -- the bomb class is kept iff the planted_c4 entity exists, and no OTHER
#       subject class ever gains a kept draw;
#   (b) an image A/B against `bomb off` (the control that drops the planted bomb too). Whatever
#       differs between those two frames IS what the bomb rule adds. If it adds more than the
#       bomb, that difference is large and obvious rather than a judgement call.
$bombPlanted = $status -match 'bomb:\s*PLANTED'
Write-Host ("  bomb state: {0}" -f ($(if ($bombPlanted) { 'PLANTED' } else { 'not planted' })))

if (-not $counters.ContainsKey('bomb')) { Fail "status did not report the 'bomb' class" }
elseif ($bombPlanted) {
    if ($counters['bomb'].dropped -ne 0) { Fail "the bomb is planted but $($counters['bomb'].dropped) C4 draw(s) were dropped" }
    else { Pass "planted bomb never dropped (kept $($counters['bomb'].kept) draw(s))" }
} else {
    if ($counters['bomb'].kept -ne 0) { Fail "the bomb is NOT planted but $($counters['bomb'].kept) C4 draw(s) were kept - a carried or loose bomb would float" }
    elseif ($counters['bomb'].dropped -eq 0) {
        # Honest labelling: no C4 was drawn at all, so this run did not exercise the gate. Not a
        # failure, but it must not be read as one either -- run a tick where a T carries the bomb.
        Write-Host "  note  no C4 was drawn this frame, so the unplanted case is UNPROVEN here" -ForegroundColor Yellow
    }
    else { Pass "unplanted bomb dropped ($($counters['bomb'].dropped) draw(s)), never kept" }
}

$bombOnlyDiff = $null
if ($bombPlanted) {
    NC "mirv_filmmaker render worldonly bomb off;;mirv_filmmaker render worldonly on" 1.2 | Out-Null
    Start-Sleep -Milliseconds 1500
    $noBomb = Capture '04-worldonly-nobomb.png'
    NC "mirv_filmmaker render worldonly bomb auto;;mirv_filmmaker render worldonly off" 1.2 | Out-Null
    $bombOnlyDiff = Diff-Mean $after $noBomb
    Write-Host ("  diff worldonly -> worldonly(bomb dropped) = {0:N3}" -f $bombOnlyDiff)
    if ($bombOnlyDiff -le 0.0) {
        Fail "dropping the bomb changed nothing - the bomb was not visible, so this run does not prove the rule"
    } elseif ($bombOnlyDiff -ge $changed) {
        Fail "the bomb rule changes as much of the frame as removing every subject did ($bombOnlyDiff vs $changed) - it is admitting more than the bomb"
    } else { Pass "the bomb rule adds only a small, bomb-sized part of the frame ($bombOnlyDiff)" }
}

# --- 5. probe log: did anything subject-looking survive as a KEEP? -------------------------------
$log = Get-ChildItem (Join-Path $dbgDir 'mvm_debug_*.log') -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$leaks = @()
if ($log) {
    $verdicts = Select-String -LiteralPath $log.FullName -Pattern '\] (KEEP|DROP) via=' |
        ForEach-Object { ($_.Line -split '\] ')[-1] }
    Set-Content -LiteralPath (Join-Path $outAbs 'probe-verdicts.txt') -Value $verdicts
    # class=bomb is the one legitimate subject KEEP (rule 9c), and only while planted -- the
    # counter assertions above already police that. Everything else matching a subject path is a
    # leak.
    $leaks = @($verdicts | Where-Object {
        $_ -like 'KEEP*' -and $_ -notmatch 'class=bomb' -and
        ($_ -match "material='[^']*(characters/|weapons/|w_models/|compmat_|/arms/|glove)")
    })
    Write-Host "  probe verdicts: $($verdicts.Count) (distinct material x draw path)"
}
if (-not $log) { Fail 'no mvm_debug log found - the probe evidence is missing' }
elseif ($leaks.Count -gt 0) {
    Fail "a subject material was KEPT: $($leaks -join '; ')"
} else { Pass 'no subject-looking material was kept' }

# Draw-path coverage is context for a leak, not a pass/fail: an unhooked descriptor only matters
# if something actually renders through it.
NC "mirv_filmmaker render worldonly coverage" 2.0 | Out-Null

$result = [ordered]@{
    tick = $GotoTick
    counters = $counters
    diffBeforeToWorldOnly = $changed
    diffBeforeToRestored = $restoreDiff
    bombPlanted = $bombPlanted
    diffBombRuleAdds = $bombOnlyDiff
    keptSubjectMaterials = $leaks
    failures = $failures
    pass = ($failures.Count -eq 0)
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outAbs 'result.json')

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host "WORLD ONLY: PASS  ($outAbs)" -ForegroundColor Green
    exit 0
}
Write-Host "WORLD ONLY: FAIL ($($failures.Count)) ($outAbs)" -ForegroundColor Red
exit 1
