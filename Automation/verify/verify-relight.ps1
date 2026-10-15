[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$OutputDir = 'Automation/output/relight-verification',
    [ValidateSet('Baseline', 'Cost', 'Clusters', 'Probes')][string]$Stage = 'Cost',
    [ValidateRange(5, 60)][int]$Seconds = 15,
    [switch]$NoCaptures
)
$ErrorActionPreference = 'Stop'
# Run in offline Mirage with authored lights loaded. This script holds a fixed pose,
# changes transient relight diagnostics, and restores the prior diagnostic settings.
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -Port $Port -OutputDir $OutputDir
[void](Wait-Map de_mirage)
$status = Send-Netcon @('mirv_relight status') .2
if ($status -notmatch 'enabled=1 .*lights=\d+ map=''de_mirage''') {
    throw 'Load Mirage authored lights and enable mirv_relight before verification.'
}
$restore = [Collections.Generic.List[string]]::new()
foreach ($setting in @(@('hoist', 'hoist', @('0','1')), @('cost', 'cost', @('full','word','one')),
                        @('view', 'view', @('off','lights','direct','coverage','probes')))) {
    $match = [regex]::Match($status, "$($setting[0])=(\d+)")
    if ($match.Success) { $restore.Add("mirv_relight $($setting[1]) $($setting[2][[int]$match.Groups[1].Value])") }
}
$match = [regex]::Match($status, 'cached=(\d+)')
if ($match.Success) { $restore.Add("mirv_relight shadowlights $($match.Groups[1].Value)") }
$match = [regex]::Match($status, 'clusters=(\d+)')
if ($match.Success) { $restore.Add("mirv_relight clusters $($match.Groups[1].Value)") }
foreach ($setting in @('gi','bounce','probecheck')) {
    $match = [regex]::Match($status, "$setting=([0-9.]+)")
    if ($match.Success) { $restore.Add("mirv_relight $setting $($match.Groups[1].Value)") }
}
$events = [Collections.Generic.List[object]]::new()
$results = [Collections.Generic.List[object]]::new()
$captures = [Collections.Generic.List[object]]::new()
function Command([string[]]$Commands) {
    $reply = Send-Netcon $Commands .15
    $events.Add(@{ time = [DateTime]::UtcNow.ToString('o'); commands = $Commands; reply = $reply })
    if ($reply -match 'relight: error|Unknown command') { throw $reply }
    return $reply
}
function Measure-Relight([string]$Name) {
    Start-Sleep -Seconds 3 # Let cached shadow faces converge before timing.
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $first = Command @('mirv_filmmaker health')
    $start = $timer.Elapsed.TotalSeconds
    Start-Sleep -Seconds $Seconds
    $last = Command @('mirv_filmmaker health')
    $elapsed = $timer.Elapsed.TotalSeconds - $start
    $a = [regex]::Match($first, 'Before_Present\)\s*:\s*(\d+)')
    $b = [regex]::Match($last, 'Before_Present\)\s*:\s*(\d+)')
    if (-not $a.Success -or -not $b.Success) { throw 'Missing render-pump counters.' }
    $frames = [long]$b.Groups[1].Value - [long]$a.Groups[1].Value
    if ($frames -le 0) { throw 'The render pump stopped or restarted during measurement.' }
    $fps = $frames / $elapsed
    $results.Add(@{ name = $Name; frames = $frames; seconds = $elapsed; fps = $fps; frameMs = 1000 / $fps })
    Write-Host ("{0}: {1:N2} fps, {2:N2} ms" -f $Name, $fps, (1000 / $fps))
    if (-not $NoCaptures) { $captures.Add((Capture-Frame de_mirage $Name)) }
}
try {
    [void](Command @('mvm_debug stop', 'mvm_debug start', 'sv_cheats 1', 'bot_kick', 'bot_quota 0',
        'mp_ignore_round_win_conditions 1', 'sv_gravity 0', 'r_drawviewmodel 0', 'cl_drawhud 0',
        'fov_cs_debug 0', 'mirv_fov 110', 'setpos_exact -1800 -1100 -60', 'setang_exact -5 300 0',
        'getpos', 'mirv_relight view off'))
    if ($Stage -eq 'Baseline') { Measure-Relight 'baseline' }
    elseif ($Stage -eq 'Cost') {
        foreach ($mode in @(@('per-draw', '0', 'full'), @('hoisted', '1', 'full'),
                            @('one-word', '1', 'word'), @('one-record', '1', 'one'))) {
            [void](Command @("mirv_relight hoist $($mode[1])", "mirv_relight cost $($mode[2])"))
            Measure-Relight $mode[0]
        }
    } elseif ($Stage -eq 'Probes') {
        [void](Command @('r_drawviewmodel 1', 'mirv_relight hoist 1', 'mirv_relight cost full',
            'mirv_relight clusters 1', 'mirv_relight gi 0', 'mirv_relight probecheck 0'))
        Measure-Relight 'probes-off'
        [void](Command @('mirv_relight gi 1', 'mirv_relight rebuild'))
        # Require the actual publish event, not just an arbitrary convergence delay.
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        $published = $false
        do {
            Start-Sleep -Milliseconds 500
            $log = Get-ChildItem -LiteralPath (Join-Path $env:APPDATA 'HLAE/debuglogs') -Filter 'mvm_debug_*.log' |
                Sort-Object LastWriteTimeUtc | Select-Object -Last 1
            if ($log) { $published = [bool](Select-String -LiteralPath $log.FullName -Pattern 'relight.probe.gi.*published' -Quiet) }
        } until ($published -or [DateTime]::UtcNow -gt $deadline)
        if (-not $published) { throw 'Probe volume did not publish within 45 seconds.' }
        Measure-Relight 'probes-on'
        foreach ($yaw in @(296,300,304)) {
            [void](Command @("setang_exact -5 $yaw 0"))
            if (-not $NoCaptures) { $captures.Add((Capture-Frame de_mirage "probes-motion-$yaw")) }
        }
    } else {
        [void](Command @('mirv_relight hoist 1', 'mirv_relight cost full'))
        foreach ($shadowCount in @(0, 64)) {
            [void](Command @("mirv_relight shadowlights $shadowCount"))
            foreach ($mode in @(0, 2, 1)) {
                [void](Command @("mirv_relight clusters $mode"))
                Measure-Relight "shadows-$shadowCount-clusters-$mode"
            }
        }
        # Multiple live frames/poses expose tile and depth boundary discontinuities.
        foreach ($yaw in @(296, 298, 300, 302, 304)) {
            foreach ($mode in @(0, 1, 2)) {
                [void](Command @("setang_exact -5 $yaw 0", "mirv_relight clusters $mode"))
                if (-not $NoCaptures) { $captures.Add((Capture-Frame de_mirage "motion-$yaw-clusters-$mode")) }
            }
        }
        foreach ($x in @(-1900,-1800,-1700)) {
            [void](Command @("setpos_exact $x -1100 -60",'setang_exact -5 300 0'))
            Start-Sleep -Seconds 1 # Let camera interpolation settle before comparing masks.
            foreach ($mode in @(0,1,2)) {
                [void](Command @("mirv_relight clusters $mode"))
                if (-not $NoCaptures) { $captures.Add((Capture-Frame de_mirage "depth-$x-clusters-$mode")) }
            }
        }
        foreach ($mode in @(0,1)) {
            [void](Command @("mirv_relight clusters $mode",'mirv_relight view lights'))
            if (-not $NoCaptures) { $captures.Add((Capture-Frame de_mirage "heat-clusters-$mode")) }
        }
    }
} finally {
    if ($restore.Count) { [void](Send-Netcon $restore.ToArray() .15) }
    [void](Send-Netcon @('setang_exact -5 300 0', 'mvm_debug stop') .15)
    $log = Get-ChildItem -LiteralPath (Join-Path $env:APPDATA 'HLAE/debuglogs') -Filter 'mvm_debug_*.log' |
        Sort-Object LastWriteTimeUtc | Select-Object -Last 1
    if ($log) { Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $output "$Stage-debug.log") }
    @{ stage = $Stage; initialStatus = $status; results = $results; events = $events; captures = $captures;
        limitations = 'Probe GI reaches native probe-enabled props; world lightmaps remain disabled. AO is not verified here.' } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output "$Stage.json")
}
