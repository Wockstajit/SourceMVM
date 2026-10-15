# Modern muzzle-FX alignment verify -- IN-GAME UNIT DISTANCE, not screenshot pixels.
#
# Drives a running CS2 (launching one if needed) over netcon:
#   fx on + weaponfx/tracers modern, `fx align on` (the hook-side probe in
#   AfxHookSource2/Filmmaker/Movie/FxAlign.cpp), plays the all-weapons demo, then reads the NDJSON
#   samples the hook wrote to %APPDATA%\HLAE\fx_align.jsonl and passes/fails per
#   weapon-class/effect on SOURCE-UNIT distance from the muzzle attachment.
#
# Pass/fail per (class, effect) group: MEDIAN distance <= threshold. Missing required
# groups fail. The old screenshot audit (audit-modern-muzzle-alignment.py) is no longer
# part of the verdict.
#
# THRESHOLD RATIONALE (measured 2026-07-04, see Docs/features/filmmaker_effects_modifiers.md):
# the probe's reference is the WORLD weapon's muzzle attachment (or its origin when the
# model has no muzzle point) because the first-person viewmodel weapon entity does not
# resolve on demo POV pawns yet (vm_debug field per sample). First-person effects anchor
# at the VIEWMODEL muzzle, a consistent ~12u from the world muzzle, and origin-fallback
# references add up to ~a gun length. Default 20u therefore certifies "anchored at the
# weapon's muzzle region" and catches gross misalignment (wrong entity / world origin /
# detached anchor); barrel-tip (~2.5u) certification needs the viewmodel-muzzle
# resolution fixed first.
#
# Output: Automation/runs/fx-align/<timestamp>/
#   fx_align.jsonl              raw per-shot samples (copied from %APPDATA%\HLAE)
#   alignment-units-summary.txt human-readable table + verdict
#   summary.json                machine-readable aggregates
#   report-console.log          the in-game `fx align report` output
param(
    [int]$Port = 29010,
    [string]$DemoName = 'all weapon test .dem',
    [double]$Timescale = 1.0,
    [double]$Threshold = 20.0,
    [int]$StallSeconds = 25,
    [int]$MaxMinutes = 12
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$automationRoot = Join-Path $repo 'Automation'
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
$netconScript = Join-Path $automationRoot 'netcon\cs2-netcon.ps1'
$alignJsonl = Join-Path $env:APPDATA 'HLAE\fx_align.jsonl'

function Test-Netcon([int]$p) {
    try {
        $c = [System.Net.Sockets.TcpClient]::new()
        $r = $c.BeginConnect('127.0.0.1', $p, $null, $null)
        if (-not $r.AsyncWaitHandle.WaitOne(2000)) { $c.Close(); return $false }
        $c.EndConnect($r); $c.Close(); return $true
    } catch { return $false }
}

# cs2-netcon.ps1 streams console text via Write-Host only -- that goes to the host UI,
# NOT the success/output stream, so `& $netconScript ... | Out-String` captures nothing
# (confirmed live 2026-07-04: the SEEN-counter stall loop below silently never saw a
# value, so it spun for the full -MaxMinutes ceiling every run instead of detecting the
# demo was done in ~40s). Always route through -LogPath and read the file back instead
# of relying on the pipeline.
$script:netconScratchLog = Join-Path $env:TEMP "fx-align-netcon-$PID.log"
function Send-Netcon([string[]]$commands, [double]$readSeconds = 0.8, [string]$logPath) {
    $target = $logPath
    if (-not $target) { $target = $script:netconScratchLog }
    & $netconScript -Port $Port -Commands $commands -ReadSeconds $readSeconds -LogPath $target | Out-Null
    Get-Content -Raw $target -ErrorAction SilentlyContinue
}

if (-not (Test-Netcon $Port)) {
    Write-Host "CS2 netcon not up on $Port -- launching..." -ForegroundColor Yellow
    & (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') -Port $Port
    if (-not (Test-Netcon $Port)) { throw "Netcon still unavailable on $Port after launch." }
}

$ts = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $automationRoot "runs\fx-align\$ts"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Save-AutomationRunMetadata -RunDirectory $outDir -AutomationName 'verify-modern-muzzle-alignment' -Additional @{
    port = $Port; demoName = $DemoName; timescale = $Timescale; threshold = $Threshold; metric = 'source-units'
} | Out-Null

Write-Host "=== Modern muzzle alignment verify (Source units) ===" -ForegroundColor Cyan
Write-Host "Output: $outDir"

$failures = New-Object System.Collections.Generic.List[string]
try {
    # -- arm the probe --
    Send-Netcon @(
        'mirv_filmmaker fx on quiet',
        'mirv_filmmaker fx set weaponfx modern quiet',
        'mirv_filmmaker fx set tracers modern quiet',
        "mirv_filmmaker fx align threshold $Threshold",
        'mirv_filmmaker fx align clear',
        'mirv_filmmaker fx align on'
    ) 0.5 | Out-Null
    Send-Netcon @("playdemo `"$DemoName`"") 2.0 | Out-Null
    Start-Sleep -Seconds 8   # demo load
    Send-Netcon @("demo_timescale $Timescale", 'mirv_filmmaker fx align') 0.8 | Out-Null

    # -- observe until the hook's SEEN counter stalls (demo over/paused). The align sample
    # file is the wrong stall signal: the all-weapons demo has long non-shooting gaps
    # (walking, weapon pickups) where no muzzle swap fires but the demo is very much
    # still playing -- the seen counter grows on ANY particle creation instead.
    $deadline = (Get-Date).AddMinutes($MaxMinutes)
    $lastSeen = -1
    $lastGrowth = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 10
        $stateOut = (Send-Netcon @('mirv_filmmaker fx state') 0.8 | Out-String)
        $seen = -1
        if ($stateOut -match '"seen":(\d+)') { $seen = [int]$Matches[1] }
        if ($seen -gt $lastSeen) { $lastSeen = $seen; $lastGrowth = Get-Date }
        elseif ($seen -ge 0 -and ((Get-Date) - $lastGrowth).TotalSeconds -ge $StallSeconds) {
            Write-Host "Particle SEEN counter stalled ${StallSeconds}s at $seen -- demo assumed done." -ForegroundColor Gray
            break
        }
    }
    $sampleBytes = 0
    if (Test-Path $alignJsonl) { $sampleBytes = (Get-Item $alignJsonl).Length }
    if ($sampleBytes -eq 0) { $failures.Add('no samples were written at all (demo did not play, hook not armed, or no Modern swaps fired)') }
} finally {
    # -- teardown --
    $tear = @()
    $tear += 'mirv_filmmaker fx align off'
    $tear += 'mirv_filmmaker fx align report'
    Send-Netcon $tear 1.5 (Join-Path $outDir 'report-console.log') | Out-Null
}

# -- parse + aggregate ---------------------------------------------------------------
if (Test-Path $alignJsonl) { Copy-Item $alignJsonl (Join-Path $outDir 'fx_align.jsonl') -Force }
$samples = @()
if (Test-Path $alignJsonl) {
    $samples = Get-Content $alignJsonl | Where-Object { $_.Trim() } | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object { $_ }
}

# Required coverage. Muzzleflash: every class in the all-weapons demo.
$required = @{
    muzzleflash = @('assaultrifle','smg','shotgun','pistol','deagle','revolver','lmg','autosniper','awp','rifle_silenced','smg_silenced')
}

$groups = $samples | Group-Object { "$($_.weapon_class)|$($_.effect)" }
$agg = @{}
foreach ($g in $groups) {
    $d = @($g.Group | ForEach-Object { [double]$_.distance_units } | Sort-Object)
    $median = if ($d.Count % 2) { $d[[int][math]::Floor($d.Count / 2)] }
              else { ($d[$d.Count / 2 - 1] + $d[$d.Count / 2]) / 2 }
    $cpN = @($g.Group | Where-Object { $_.method -eq 'cp-scan' }).Count
    $attachN = @($g.Group | Where-Object { $_.muzzle_attachment }).Count
    $agg[$g.Name] = [pscustomobject]@{
        class = $g.Name.Split('|')[0]; effect = $g.Name.Split('|')[1]
        n = $g.Count
        median = [math]::Round($median, 3)
        mean = [math]::Round(($d | Measure-Object -Average).Average, 3)
        max = [math]::Round(($d | Measure-Object -Maximum).Maximum, 3)
        cpScan = $cpN
        attachRef = $attachN
        pass = ([double]$median -le $Threshold)
    }
}

foreach ($effect in $required.Keys) {
    foreach ($cls in $required[$effect]) {
        $k = "$cls|$effect"
        if (-not $agg.ContainsKey($k)) {
            $failures.Add("MISSING: $cls $effect (no samples)")
        } elseif (-not $agg[$k].pass) {
            $a = $agg[$k]
            $failures.Add("FAIL: $cls $effect median=$($a.median)u mean=$($a.mean)u max=$($a.max)u (n=$($a.n))")
        }
    }
}

# -- report ---------------------------------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("Modern muzzle alignment (Source units from muzzle attachment). threshold=$Threshold demo='$DemoName' timescale=$Timescale")
$lines.Add(("{0,-14} {1,-12} {2,5} {3,9} {4,8} {5,8} {6,6} {7,7} {8}" -f 'class','effect','n','median_u','mean_u','max_u','scan','attach','verdict'))
foreach ($a in ($agg.Values | Sort-Object class, effect)) {
    $req = $required.ContainsKey($a.effect) -and $required[$a.effect] -contains $a.class
    $verdict = if ($a.pass) { 'pass' } elseif ($req) { 'FAIL' } else { 'fail (not required)' }
    $lines.Add(("{0,-14} {1,-12} {2,5} {3,9} {4,8} {5,8} {6,6} {7,7} {8}" -f $a.class,$a.effect,$a.n,$a.median,$a.mean,$a.max,$a.cpScan,$a.attachRef,$verdict))
}
$lines.Add('')
if ($failures.Count) {
    $lines.Add("RESULT: FAIL ($($failures.Count) issue(s))")
    $failures | ForEach-Object { $lines.Add("  $_") }
} else {
    $lines.Add('RESULT: PASS (all required class/effect groups within threshold)')
}
$summaryPath = Join-Path $outDir 'alignment-units-summary.txt'
$lines | Set-Content $summaryPath
$lines | ForEach-Object { Write-Host $_ }

[pscustomobject]@{
    threshold = $Threshold; demo = $DemoName; timescale = $Timescale
    sampleCount = $samples.Count
    groups = @($agg.Values | Sort-Object class, effect)
    failures = @($failures)
    pass = ($failures.Count -eq 0)
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outDir 'summary.json')

Write-Host "`nRun folder: $outDir" -ForegroundColor Green
if ($failures.Count) { exit 1 }
exit 0
