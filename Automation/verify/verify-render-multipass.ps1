# Verifies a multipass render job end to end against a live CS2 demo session.
#
# Opens the Camera Editor + Render workspace over netcon, selects the given passes, renders a short
# custom range as TGA sequences, then checks:
#   * every pass got its own folder, each with exactly the frames.csv frame count;
#   * the manifest lists every stream and completed;
#   * frame SYNC: Automation/verify/render_pass_sync.py scores each pass against Beauty at frame
#     offsets -1 / 0 / +1 -- offset 0 must win for every pass (a one-frame offset is a failure).
#
#   powershell -ExecutionPolicy Bypass -File Automation\verify\verify-render-multipass.ps1 `
#       -OutRoot C:\path\to\out -Passes beauty:all,beauty:worldplayers,beauty:players,beauty:gun,depth:all,normal:all
param(
    [int]$Port = 29010,
    [Parameter(Mandatory = $true)]
    [string]$OutRoot,
    [string[]]$Passes = @('beauty:all', 'depth:all', 'normal:all', 'beauty:gun'),
    [int]$StartTick = -1,
    [int]$RangeTicks = 64,
    [int]$Fps = 60,
    [int]$JobTimeoutSeconds = 600,
    [switch]$SkipSync
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$netcon = Join-Path $automationRoot 'netcon\cs2-netcon.ps1'
if (-not (Test-Path -LiteralPath $netcon)) { throw "netcon helper not found: $netcon" }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$renderRoot = Join-Path $OutRoot 'takes'
New-Item -ItemType Directory -Force -Path $renderRoot | Out-Null
# A single `-Passes a,b` through powershell -File arrives as one string.
$Passes = @($Passes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$netconLog = Join-Path $OutRoot 'netcon.log'
function Send-Cmds([string]$batch, [double]$read = 0.8) {
    if (Test-Path -LiteralPath $netconLog) { Remove-Item -LiteralPath $netconLog -Force }
    & $netcon -Port $Port -CommandBatch $batch -ReadSeconds $read -LogPath $netconLog *> $null
    if (-not (Test-Path -LiteralPath $netconLog)) { return '' }
    return (Get-Content -Raw -LiteralPath $netconLog)
}

$health = Send-Cmds 'mirv_filmmaker health' 1.0
if ([string]::IsNullOrWhiteSpace($health)) { throw 'no netcon reply to mirv_filmmaker health' }
"OK health replied"

Send-Cmds 'mirv_filmmaker editor on' 1.5 | Out-Null
Send-Cmds 'mirv_filmmaker render open' 1.0 | Out-Null
$pathOut = Send-Cmds "mirv_filmmaker render output path `"$renderRoot`"" 1.0
if ($pathOut -notmatch 'output folder set to') { throw "could not set the render output folder:`n$pathOut" }
Send-Cmds "mirv_filmmaker render output format tga;;mirv_filmmaker render output fps $Fps;;r_drawviewmodel 1" | Out-Null

$passCmds = @('mirv_filmmaker render pass clear')
$expected = @()
foreach ($p in $Passes) {
    $t, $e = $p -split ':'
    $passCmds += "mirv_filmmaker render pass add $t $e"
    $expected += $(if ($e -eq 'all') { $t } else { "${t}_$e" })
}
$passOut = Send-Cmds ($passCmds -join ';;') 1.0
"OK passes: $($expected -join ', ')"

$statusOut = Send-Cmds 'mirv_filmmaker render range custom' 1.0
if ($StartTick -lt 0) {
    if ($statusOut -match 'custom\s+(\d+)') { $StartTick = [int]$Matches[1] } else { $StartTick = 20000 }
}
$endTick = $StartTick + $RangeTicks
Send-Cmds "mirv_filmmaker render setrange $StartTick $endTick" | Out-Null
"OK range $StartTick..$endTick at $Fps fps"

$before = @(Get-ChildItem -LiteralPath $renderRoot -Directory -ErrorAction SilentlyContinue)
$startOut = Send-Cmds 'mirv_filmmaker render start' 1.5
if ($startOut -match 'cannot start') { throw "render start refused:`n$startOut" }
"OK job started"

$deadline = (Get-Date).AddSeconds($JobTimeoutSeconds)
$done = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $st = Send-Cmds 'mirv_filmmaker render status' 1.0
    if ($st -match 'job active\s*:\s*no') { $done = $true; break }
}
if (-not $done) { throw "render job did not finish within $JobTimeoutSeconds s" }
"OK job finished"

$after = @(Get-ChildItem -LiteralPath $renderRoot -Directory -ErrorAction SilentlyContinue)
$takeDir = $after | Where-Object { $before.FullName -notcontains $_.FullName } |
    Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $takeDir) { throw 'no take directory was created' }
"OK take dir: $($takeDir.FullName)"

$manifest = Join-Path $takeDir.FullName 'manifest.json'
$framesCsv = Join-Path $takeDir.FullName 'frames.csv'
if (-not (Test-Path -LiteralPath $manifest)) { throw 'manifest.json missing' }
if (-not (Test-Path -LiteralPath $framesCsv)) { throw 'frames.csv missing' }
$manifestText = Get-Content -Raw -LiteralPath $manifest
$manifestJson = $manifestText | ConvertFrom-Json
if ($manifestText -notmatch '"status":\s*"completed"') { throw "manifest not completed:`n$manifestText" }
$csvRows = (Get-Content -LiteralPath $framesCsv | Measure-Object -Line).Lines - 1
"OK manifest completed, frames.csv rows: $csvRows"

$failures = 0
foreach ($name in $expected) {
    if ($manifestText -notmatch ('"name":\s*"' + [regex]::Escape($name) + '"')) { "FAIL manifest lacks stream $name"; $failures++ }
    # TGA is an image sequence: each stream's frames are in <type>\<variant>\ (manifest folder).
    $entry = @($manifestJson.streams | Where-Object { $_.name -eq $name })[0]
    $frames = @()
    if ($entry) {
        $frames = @(Get-ChildItem -LiteralPath (Join-Path $takeDir.FullName ([string]$entry.folder)) -File -Filter *.tga -ErrorAction SilentlyContinue)
    }
    if ($frames.Count -ne $csvRows) { "FAIL $name has $($frames.Count) frame(s), expected $csvRows"; $failures++ }
    else { "OK $name : $($frames.Count) frames" }
}

if (-not $SkipSync) {
    $py = Join-Path $PSScriptRoot 'render_pass_sync.py'
    & python $py $takeDir.FullName
    if ($LASTEXITCODE -ne 0) { "FAIL frame sync"; $failures++ } else { "OK frame sync" }
}

Send-Cmds 'mirv_filmmaker render close' | Out-Null
if ($failures) { "VERIFY-RENDER-MULTIPASS: FAIL ($failures)"; exit 1 }
"VERIFY-RENDER-MULTIPASS: PASS"
