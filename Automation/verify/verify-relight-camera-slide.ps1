[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RouteFile,
    [string]$OutputDir = 'Automation/output/relight-camera-slide',
    [ValidateRange(2,60)][double]$Duration = 8,
    [ValidateRange(24,240)][int]$Screenshots = 24,
    [ValidateSet('0.5','1')][string]$GiScale = '0.5'
)

# Move the live free camera continuously; never create or play a camera path.
# Record every rendered frame, then retain evenly spaced, lossless native-size PNGs.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'verify-environment-competitive-maps.ps1') -LibraryOnly -OutputDir $OutputDir
$route = Get-Content -LiteralPath $RouteFile -Raw | ConvertFrom-Json
foreach ($pose in @($route.start,$route.finish)) {
    if ($pose.Count -ne 6) { throw 'Each route pose must contain x, y, z, pitch, yaw, roll.' }
    foreach ($value in $pose) { if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { throw 'Route values must be finite.' } }
}
$culture = [Globalization.CultureInfo]::InvariantCulture
$fov = if ($route.fov) { [double]$route.fov } else { 110 }
[void](Send-Netcon @('mirv_input camera','mirv_input cfg offsetMode ownLast',
    'mirv_input cfg msens 0','mirv_input cfg ksens 0',"mirv_fov $fov","mirv_relight giscale $GiScale",'mvm_debug start') .06)
Start-Sleep -Milliseconds 600
$client = [Net.Sockets.TcpClient]::new('127.0.0.1',29010)
$client.NoDelay = $true
$stream = $client.GetStream()
$buffer = New-Object byte[] 65536
function Write-Command([string]$command) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($command+"`n")
    $stream.Write($bytes,0,$bytes.Length)
    while ($stream.DataAvailable) { $null=$stream.Read($buffer,0,$buffer.Length) }
}
function Write-Pose($pose) {
    $position = ($pose[0..2] | ForEach-Object { ([double]$_).ToString('F6',$culture) }) -join ' '
    $angles = ($pose[3..5] | ForEach-Object { ([double]$_).ToString('F6',$culture) }) -join ' '
    Write-Command "mirv_input position $position`nmirv_input angles $angles"
}
$manifest = @{ route=$route; duration=$Duration; giScale=$GiScale; directions=@() }
try {
$sceneBefore=Send-Netcon @('mirv_filmmaker environment status','mirv_relight status') .15
if ($sceneBefore -notmatch 'relight: enabled=1') { throw 'Enable Relight and finish map startup before motion verification.' }
$manifest.sceneBefore=$sceneBefore
function Climate-FromStatus([string]$status) {
    $flat=$status.Replace("`r",'').Replace("`n",'')
    $start=$flat.IndexOf('{"version":');$end=$flat.LastIndexOf('}')
    if ($start -lt 0 -or $end -le $start) { throw 'Environment status is incomplete.' }
    return ($flat.Substring($start,$end-$start+1) | ConvertFrom-Json).climate | ConvertTo-Json -Compress
}
$climateBefore=Climate-FromStatus $sceneBefore
    foreach ($direction in @('forward','reverse')) {
        $a=$route.start; $b=$route.finish
        if ($direction -eq 'reverse') { $a=$route.finish; $b=$route.start }
        Write-Pose $a
        Start-Sleep -Milliseconds 1500
        $directory = Join-Path $output $direction
        Write-Command ('mirv_streams record name "'+$directory+'"')
        Write-Command 'mirv_streams record format bmp'
        Write-Command 'mirv_streams record screen enabled 1'
        Write-Command 'mirv_streams record start'
        Start-Sleep -Milliseconds 150
        $startUtc=[DateTime]::UtcNow.ToString('o')
        $clock=[Diagnostics.Stopwatch]::StartNew(); $samples=@(); $next=0
        do {
            # Explicit double avoids PowerShell selecting Math.Min(int,int), which
            # would hold the first pose and then jump instead of interpolating.
            $t=[Math]::Min([double]1,$clock.Elapsed.TotalSeconds/$Duration)
            $pose=@(); for ($axis=0;$axis -lt 6;$axis++) { $pose+=($a[$axis]+($b[$axis]-$a[$axis])*$t) }
            Write-Pose $pose
            $samples+=@{ ms=$clock.Elapsed.TotalMilliseconds; t=$t; pose=$pose }
            $next+=1000/60
            $wait=$next-$clock.Elapsed.TotalMilliseconds
            if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
        } while ($t -lt 1)
        Start-Sleep -Milliseconds 500
        Write-Command 'mirv_streams record end'
        Start-Sleep -Milliseconds 700
        $take=Get-ChildItem -LiteralPath $directory -Directory | Sort-Object LastWriteTimeUtc | Select-Object -Last 1
        if (-not $take) { throw "No native capture take for $direction." }
        $frames=@(Get-ChildItem -LiteralPath $take.FullName -Filter '*.bmp' | Sort-Object Name)
        if ($frames.Count -lt $Screenshots) { throw "Only $($frames.Count) native frames; required $Screenshots." }
        $pngDir=Join-Path $output "$direction-png"
        New-Item -ItemType Directory -Force -Path $pngDir | Out-Null
        for ($index=0;$index -lt $Screenshots;$index++) {
            $source=$frames[[int][Math]::Round($index*($frames.Count-1)/($Screenshots-1))]
            $bitmap=$null
            for ($attempt=0;$attempt -lt 30 -and -not $bitmap;$attempt++) {
                try { $bitmap=[Drawing.Bitmap]::FromFile($source.FullName) }
                catch { if ($attempt -eq 29) { throw }; Start-Sleep -Milliseconds 100 }
            }
            try {
                if ($bitmap.Width -lt 1500 -or $bitmap.Height -lt 1000) { throw 'Use a native game viewport of at least 1500x1000 for visual verification.' }
                $bitmap.Save((Join-Path $pngDir ($index.ToString('D2')+'.png')),[Drawing.Imaging.ImageFormat]::Png)
            } finally { $bitmap.Dispose() }
        }
        Compress-Take $take.FullName
        $samples | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output "$direction-poses.json")
        $manifest.directions+=@{ name=$direction; motionStartUtc=$startUtc; frames=$frames.Count; screenshots=$Screenshots; nativeTake=$take.FullName }
        Write-Output "$direction : $($frames.Count) native frames, $Screenshots full-size PNGs"
    }
    $manifest.sceneAfter=Send-Netcon @('mirv_filmmaker environment status','mirv_relight status') .15
    if ($manifest.sceneAfter -notmatch 'relight: enabled=1' -or (Climate-FromStatus $manifest.sceneAfter) -ne $climateBefore) {
        throw 'Lighting settings changed during the capture; do not use this sequence for comparison.'
    }
} finally {
    try { Write-Command 'mirv_streams record end'; Write-Pose $route.finish } finally { $client.Dispose() }
    [void](Send-Netcon @('mirv_relight giscale 0.5','mirv_input cfg msens 0.1','mirv_input cfg ksens 1') .06)
    $manifest | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $output 'capture.json')
}
