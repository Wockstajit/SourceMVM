param(
    [int]$Port = 29010,
    [double]$Seconds = 12,
    [string]$OutDir = 'build/verification/environment-toggles'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$net = Join-Path $repo 'Automation/netcon/cs2-netcon.ps1'
$out = Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Send([string]$batch, [double]$read = .12) {
    ((& $net -Port $Port -CommandBatch $batch -ReadSeconds $read 6>&1) | Out-String)
}
function Climate([string]$settings) {
    (($settings -split ';' | ForEach-Object { 'mirv_filmmaker environment climate ' + $_ }) -join ';;')
}
# Deliberately excludes quality, looks/presets, ray tracing and scene-object commands.
# Run in a paused demo with a fixed camera; preserves the incoming scene through a saved snapshot.
$snapshot = 'toggle-audit-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$null = Send "demo_pause;;mirv_filmmaker environment save $snapshot"
$neutral = 'enabled 1;sky 0;clouds 0;fogDensity 0;snow 0;wet 0;puddles 0;relighting 0;worldGi 0;sceneReflections 0;storm 0;bloom 0;vignette 0;grain 0;chromatic 0'
$cases = @(
    @('neutral', ''),
    @('sky', 'sky 1'),
    @('clouds', 'clouds 1'),
    @('fog', 'fogDensity 0.35'),
    @('snow-coating', 'snow 0.5'),
    @('wetness', 'wet 0.5'),
    @('puddles', 'puddles 0.4'),
    @('relighting', 'relighting 1'),
    @('relighting-no-shadows', 'relighting 1;contactShadows 0'),
    @('bounce', 'relighting 1;contactShadows 1;worldGi 1'),
    @('bounce-reflections', 'relighting 1;worldGi 1;sceneReflections 1;wet 0.5'),
    @('fog-no-shafts', 'fogDensity 0.35;lightShafts 0'),
    @('thunderstorm', 'storm 1;stormHold 1'),
    @('finishing', 'bloom 0.35;vignette 0.2;grain 0.012;chromatic 1'),
    @('layered', 'sky 1;clouds 1;fogDensity 0.35;snow 0.5;wet 0.5;puddles 0.4'),
    @('all-passes', 'sky 1;clouds 1;fogDensity 0.35;snow 0.5;wet 0.5;puddles 0.4;relighting 1;worldGi 1;sceneReflections 1;storm 1;stormHold 1;bloom 0.35;vignette 0.2;grain 0.012')
)
$rows = @()
try {
    foreach ($case in $cases + @(1..6 | ForEach-Object { ,@("precipitation-$_", '', $_) })) {
        $name = $case[0]
        $null = Send ((Climate ($neutral + ';contactShadows 1;lightShafts 1;stormHold 0')) + ';;mirv_filmmaker environment weather set type 0')
        if ($case[1]) { $null = Send (Climate $case[1]) }
        if ($case.Count -gt 2) { $null = Send "mirv_filmmaker environment weather set type $($case[2])" }
        if ($name -eq 'all-passes') { $null = Send 'mirv_filmmaker environment weather set type 1' }
        # Let queued scene edits and the previous asynchronous GPU query retire.
        $null = Send 'mirv_filmmaker health' 1
        $start = Send 'mvm_debug start' $Seconds
        $match = [regex]::Match($start, 'file: ([^\r\n]+\.log)')
        if (!$match.Success) { throw "Debug capture failed: $start" }
        $finish = Send 'mvm_debug stop;;mirv_filmmaker environment status'
        [IO.File]::WriteAllText((Join-Path $out "$name-console.txt"), $start + $finish)
        $path = Join-Path $out "$name-runtime.log"
        Copy-Item -LiteralPath $match.Groups[1].Value.Trim() -Destination $path -Force
        $samples = @([regex]::Matches([IO.File]::ReadAllText($path), '\[environment.gpu\].*?ms=([0-9.]+)') | ForEach-Object { [double]$_.Groups[1].Value } | Sort-Object)
        if (!$samples.Count) { throw "No GPU samples for $name; increase Seconds or inspect renderer status." }
        $row = [pscustomobject]@{ case=$name; samples=$samples.Count; medianMs=$samples[[int][math]::Floor($samples.Count/2)]; minMs=$samples[0]; maxMs=$samples[-1] }
        $rows += $row
        $row | Format-Table | Out-String | Write-Output
        $rows | Export-Csv -NoTypeInformation -Path (Join-Path $out 'timings.csv')
    }
} finally {
    $null = Send "mvm_debug stop;;mirv_filmmaker environment load $snapshot"
}
