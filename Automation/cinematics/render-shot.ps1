param(
  [Parameter(Mandatory = $true)][string]$Shots,
  [Parameter(Mandatory = $true)][string]$Shot,
  [Parameter(Mandatory = $true)][string]$Folder,
  [string]$Prefix = '',
  [int]$TimeoutSeconds = 900
)
# Render one planned shot as a take: write its markers into the demo's project.mvm
# (shotplan.py install), reload them in the Camera Editor, then run a Render-workspace job with
# the camera path as the camera source over the shot's range at the shot's fps.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$netcon = Join-Path $repo 'Automation\netcon\cs2-netcon.ps1'
$plan = Join-Path $PSScriptRoot 'shotplan.py'
function Nc([string]$batch) {
  & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $netcon -CommandBatch $batch 2>&1 | Out-String -Width 4096
}
$spec = Get-Content -Raw $Shots | ConvertFrom-Json
$def = $spec.shots | Where-Object { $_.name -eq $Shot } | Select-Object -First 1
if (-not $def) { throw "no shot named $Shot" }
$fps = if ($def.fps) { [int]$def.fps } elseif ($def.style -eq 'snyder') { 256 } else { 64 }
$range = $def.range

# Editor first, install second: opening the Camera Editor saves the path it holds in memory, which
# overwrote a fresh install with the previous shot's markers (the take rendered the wrong path).
Nc 'mirv_filmmaker follow stop;;mirv_filmmaker follow clear;;mirv_input end;;mirv_filmmaker editor on' | Out-Null
Start-Sleep -Seconds 2
Nc 'mirv_filmmaker marker save' | Out-Null
Start-Sleep -Seconds 1
$installOut = & python $plan install $Shots $Shot
$installOut | Where-Object { $_ -notmatch '^SHAKE' }
# marker interp rebuilds the evaluated curve; hook builds before the CameraPath::Load fix kept the
# previous path's curve after marker load.
Nc 'mirv_filmmaker marker load;;mirv_filmmaker marker interp bezier;;mirv_filmmaker marker list' | Select-String 'campath' | ForEach-Object { $_.Line }
# A scrub left over from the editor timeline re-pushes its held pose every frame and freezes the
# take on it (hook builds before the RenderSessionDriver scrub release). End it before the job.
Nc 'mirv_filmmaker camtl stop' | Out-Null
# Path shake rides on the keyframes (CamPathShake): clear what the previous shot left, then set this
# shot's per-key shakes live (marker index, style name, intensity, speed).
$shakeLine = ($installOut | Where-Object { $_ -match '^SHAKE' } | Select-Object -First 1) -replace '^SHAKE\s*', ''
$shakeCmds = @('mirv_filmmaker pathshake clearall', 'mirv_filmmaker pathshake on')
foreach ($s in ($shakeLine -split ';' | Where-Object { $_.Trim() })) { $shakeCmds += "mirv_filmmaker pathshake set $($s.Trim())" }
Nc ($shakeCmds -join ';;') | Out-Null
if ($shakeCmds.Count -gt 2) { "path shake: $($shakeCmds.Count - 2) key(s) -> $shakeLine" }

New-Item -ItemType Directory -Force -Path $Folder | Out-Null
$before = @(Get-ChildItem -LiteralPath $Folder -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
$name = "$Prefix$Shot"
Nc ("mirv_filmmaker render open;;mirv_filmmaker render output path $Folder;;mirv_filmmaker render output project $name;;" +
    "mirv_filmmaker render output fps $fps;;mirv_filmmaker render pass hud none;;mirv_filmmaker render camera path;;" +
    "mirv_filmmaker render range custom $($range[0]) $($range[1]);;mirv_filmmaker render validate") | Out-Null
$reply = Nc 'mirv_filmmaker render start'
if ($reply -match 'open the Render workspace first') {
  Nc 'mirv_filmmaker render open' | Out-Null
  Start-Sleep -Seconds 1
  $reply = Nc 'mirv_filmmaker render start'
}
($reply -split "`r?`n" | Where-Object { $_ -match '(?i)render|cannot|warning' } | Select-Object -Last 2) -join "`n"
$t0 = Get-Date
Start-Sleep -Seconds 5
while ((Get-Date) -lt $t0.AddSeconds($TimeoutSeconds)) {
  if ((Nc 'mirv_filmmaker render status') -match 'job active\s*:\s*no') { break }
  Start-Sleep -Seconds 4
}
$new = Get-ChildItem -LiteralPath $Folder -Directory | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $new) { throw "no take folder appeared for $name" }
$manifest = Get-Content -LiteralPath (Join-Path $new.FullName 'manifest.json') -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
$video = Get-ChildItem -LiteralPath $new.FullName -Recurse -Filter *.mp4 | Select-Object -First 1
"take=$($new.Name) status=$($manifest.status) frames=$($manifest.framesWritten)/$($manifest.framesExpected) fps=$fps elapsed=$([int]((Get-Date) - $t0).TotalSeconds)s"
if ($video) { $video.FullName }
# Verify the take actually moved like the plan (the camera the render used, from frames.csv).
& python $plan takecheck $new.FullName --allow-whip --shots $Shots --shot $Shot
