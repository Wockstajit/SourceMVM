param(
  [Parameter(Mandatory = $true)][string]$Shots,
  [Parameter(Mandatory = $true)][string]$Shot,
  [Parameter(Mandatory = $true)][string]$Ticks,   # comma-separated
  [string]$OutDir = ''
)
# In-game stills of a planned shot: for each tick, seek the paused demo there, put the free camera
# on the shot's pose at that tick (shotplan.py pose) and capture the game window. Pairs each still
# with the planner's preview of the same pose, so the offline preview can be checked against the
# real frame without playing the shot.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$netcon = Join-Path $repo 'Automation\netcon\cs2-netcon.ps1'
$capture = Join-Path $repo 'Automation\capture\capture-game-window.ps1'
$plan = Join-Path $PSScriptRoot 'shotplan.py'
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent (Resolve-Path $Shots)) 'stills' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Nc([string]$batch) {
  & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $netcon -CommandBatch $batch 2>&1 | Out-String -Width 4096
}
function Probe {
  $p = Nc 'mirv_filmmaker demoprobe'
  if ($p -match 'tick=(\d+) paused=(\d)') { return @([int]$Matches[1], [int]$Matches[2]) }
  return @(-1, -1)
}

# A follow camera or an open editor/Render workspace would own the view instead of the free camera.
Nc 'mirv_filmmaker follow stop;;mirv_filmmaker follow clear;;mirv_filmmaker marker previewstop;;mirv_filmmaker editor off;;r_drawviewmodel 0;;cl_drawhud 0;;mirv_input camera' | Out-Null
foreach ($tick in ($Ticks -split ',' | ForEach-Object { [int]$_ })) {
  $s = Probe
  if ($s[1] -eq 0) { Nc 'demo_pause' | Out-Null; Start-Sleep -Milliseconds 300 }
  Nc "demo_gototick $tick" | Out-Null
  $deadline = (Get-Date).AddSeconds(40)
  do { Start-Sleep -Milliseconds 800; $s = Probe } while ((Get-Date) -lt $deadline -and [math]::Abs($s[0] - $tick) -gt 2)
  if ($s[1] -eq 0) { Nc 'demo_pause' | Out-Null; Start-Sleep -Milliseconds 300 }
  $preview = Join-Path $OutDir ("{0}_{1}_plan.png" -f $Shot, $tick)
  $pose = (& python $plan pose $Shots $Shot $tick --still $preview | Select-Object -First 1)
  Nc ("mirv_input camera;;" + $pose) | Out-Null
  Start-Sleep -Milliseconds 900
  $full = Join-Path $OutDir ("{0}_{1}_game.full.png" -f $Shot, $tick)
  $small = Join-Path $OutDir ("{0}_{1}_game.png" -f $Shot, $tick)
  & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $capture -Out $full | Out-Null
  & ffmpeg -v error -y -i $full -vf 'scale=1280:-1' $small
  Remove-Item -LiteralPath $full -ErrorAction SilentlyContinue
  "tick $tick (demo at $($s[0])) -> $small"
}
