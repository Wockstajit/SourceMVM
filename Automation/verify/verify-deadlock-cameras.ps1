# Exercises Deadlock's camera systems against a loaded replay and scores the results.
#
# WHAT THIS PROVES, and why a screenshot cannot. Every one of these features ends in one number --
# where the view went -- and a still frame cannot tell a camera path that moved the camera two units
# from one that did not move it at all. So each check reads `mirv_filmmaker camera`, which reports the
# pose the engine actually used after every override and which feature owns it, and compares poses.
#
#   1. the entity list resolves and finds hero pawns
#   2. a camera path plays, and the pose CHANGES while it does
#   3. scrubbing the camera timeline previews the path WITHOUT moving the demo
#   4. the follow camera solves in lock-on, and tracks its hero
#   5. attach mode solves, and puts the camera somewhere different from lock-on
#
#     powershell -File Automation\verify\verify-deadlock-cameras.ps1
#     powershell -File Automation\verify\verify-deadlock-cameras.ps1 -Replay replays/104938837 -SkipLoad
#
# The game must already be running with the hook answering on the netcon port; this script does not
# launch it, because a launch is minutes and a check is seconds and they should not be one command.
#
# Every reply is saved under Automation\runs\deadlock-cameras-<timestamp>\ together with summary.json
# and a copy of the newest mvm_debug log, so a failure can be read afterwards instead of re-run.

[CmdletBinding()]
param(
    [string]$Replay = 'replays/104938837',
    [int]$Port = 29020,
    [switch]$SkipLoad,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# Deadlock's own netcon client: it reads for 3 s by default. The CS2 client this used to call reads
# for 1.5 s, which lost replies that arrive while a seek is still settling.
$netcon = Join-Path $repoRoot 'Automation\netcon\deadlock-netcon.ps1'

if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot ('Automation\runs\deadlock-cameras-{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:Failures = 0
$script:Results = New-Object System.Collections.Generic.List[object]
$script:Seq = 0

function Send-Cmd {
    param([string]$Batch, [double]$ReadSeconds = 3)
    $text = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $netcon `
        -Port $Port -ReadSeconds $ReadSeconds -CommandBatch $Batch 2>&1 | Out-String
    $script:Seq++
    $name = '{0:D3}-{1}.txt' -f $script:Seq, (($Batch -replace '[^A-Za-z0-9]+', '_').Trim('_'))
    if ($name.Length -gt 90) { $name = $name.Substring(0, 90) + '.txt' }
    Set-Content -LiteralPath (Join-Path $OutDir $name) -Value $text -Encoding UTF8
    return $text
}

function Get-Pose {
    $text = Send-Cmd 'mirv_filmmaker camera'
    $m = [regex]::Match($text, 'pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+ang\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)')
    if (-not $m.Success) { return $null }
    [pscustomobject]@{
        X = [double]$m.Groups[1].Value; Y = [double]$m.Groups[2].Value; Z = [double]$m.Groups[3].Value
        Pitch = [double]$m.Groups[4].Value; Yaw = [double]$m.Groups[5].Value
        Owner = ([regex]::Match($text, 'owner:\s+(.+)')).Groups[1].Value.Trim()
    }
}

function Get-Tick {
    $text = Send-Cmd 'mirv_filmmaker path'
    $m = [regex]::Match($text, 'playhead:\s+tick=([\d.]+)')
    if ($m.Success) { return [double]$m.Groups[1].Value }
    return $null
}

function Distance {
    param($A, $B)
    if (-not $A -or -not $B) { return -1 }
    [math]::Sqrt([math]::Pow($A.X - $B.X, 2) + [math]::Pow($A.Y - $B.Y, 2) + [math]::Pow($A.Z - $B.Z, 2))
}

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail)
    if ($Ok) {
        Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL  {0}" -f $Name) -ForegroundColor Red
        $script:Failures++
    }
    if ($Detail) { Write-Host ("        {0}" -f $Detail) }
    $script:Results.Add([pscustomobject]@{ name = $Name; result = $(if ($Ok) { 'pass' } else { 'fail' }); detail = $Detail })
}

function Skip {
    param([string]$Name, [string]$Reason)
    Write-Host ("  SKIP  {0}" -f $Name) -ForegroundColor Yellow
    Write-Host ("        {0}" -f $Reason)
    $script:Results.Add([pscustomobject]@{ name = $Name; result = 'skip'; detail = $Reason })
}

# ---------------------------------------------------------------------------------------------
Write-Host 'Deadlock camera systems' -ForegroundColor Cyan
Write-Host ("  evidence: {0}" -f $OutDir)

if (-not $SkipLoad) {
    Write-Host "  loading $Replay ..."
    Send-Cmd "playdemo $Replay" | Out-Null
    # A replay load is a map load: blocking, and several seconds even warm.
    Start-Sleep -Seconds 35
}

# A CLEAN SLATE BEFORE ANY OWNER CHECK. The `camera` owner line reports a console override first, so a
# `mirv_fov`, an imported .cam or an enabled `mirv_campath` left over from a manual session would fail
# every owner check below while the feature under test worked fine. And the free cam must be off:
# it PINS the view, every marker then snapshots the same place, and the path evaluates to one pose.
Send-Cmd 'mirv_fov default;;mirv_camio import end;;mirv_campath enabled 0' | Out-Null
Send-Cmd 'mirv_filmmaker freecam off;;mirv_filmmaker follow clear;;demo_pause' | Out-Null
Start-Sleep -Seconds 1

# ---- 1. entities -----------------------------------------------------------------------------
$ents = Send-Cmd 'mirv_filmmaker entities'
$entitiesDisabled = $ents -match 'disabled'
$heroMatch = [regex]::Match($ents, '(\d+) hero pawn')
$heroes = if ($heroMatch.Success) { [int]$heroMatch.Groups[1].Value } else { 0 }
if ($entitiesDisabled) {
    Skip 'entity list resolves' 'entity integration is compiled out of this build'
    Skip 'hero pawns found' 'entity integration is compiled out of this build'
} else {
    $listOk = $ents -match 'list=resolved' -and $ents -match 'accessor=resolved'
    Check 'entity list resolves' $listOk (($ents -split "`n" | Where-Object { $_ -match 'entities:|fields ' }) -join ' | ')
    Check 'hero pawns found' ($heroes -gt 0) "$heroes hero pawn(s)"
}

$firstHero = -1
$m = [regex]::Match($ents, '\[\s*(\d+)\]\s+\S')
if ($m.Success) { $firstHero = [int]$m.Groups[1].Value }

# ---- 2. the camera path plays ------------------------------------------------------------------
#
# Two markers, ten seconds apart, placed from two DIFFERENT spectator positions -- the replay's own
# camera has moved a long way by then, so the path has somewhere to go. Placing both at the same
# tick, or without letting the camera move between them, produces a path that evaluates to one pose
# everywhere and passes "it plays" while proving nothing.
$anchor = 5000
$span = 640   # ten seconds at 64 tick

Send-Cmd 'mirv_filmmaker marker clear' | Out-Null
Send-Cmd ('mirv_filmmaker seek {0}' -f $anchor) | Out-Null
Start-Sleep -Seconds 3
Send-Cmd 'mirv_filmmaker marker add' | Out-Null

Send-Cmd ('mirv_filmmaker seek {0}' -f ($anchor + $span)) | Out-Null
Start-Sleep -Seconds 3
Send-Cmd 'mirv_filmmaker marker add' | Out-Null

$shot = Send-Cmd 'mirv_filmmaker path'
$markerMatch = [regex]::Match($shot, 'markers=(\d+)')
$markers = if ($markerMatch.Success) { [int]$markerMatch.Groups[1].Value } else { 0 }
$spanMatch = [regex]::Match($shot, 'ticks=(\d+)\.\.(\d+)')
$spanOk = $spanMatch.Success -and ([int]$spanMatch.Groups[2].Value - [int]$spanMatch.Groups[1].Value) -gt 100
Check 'two markers placed' ($markers -ge 2) (($shot -split "`n" | Where-Object { $_ -match 'shot:' }) -join ' ')
Check 'the markers span the shot' $spanOk 'a path whose markers share a tick evaluates to one pose'

# Back to the first marker, then play. The owner is read IMMEDIATELY: this shot is 10 seconds of
# footage and a sample taken after it ends reports the game camera, which looks identical to a path
# that never engaged.
Send-Cmd ('mirv_filmmaker seek {0}' -f $anchor) | Out-Null
Start-Sleep -Seconds 3
$before = Get-Pose
Send-Cmd 'mirv_filmmaker path play' | Out-Null
Start-Sleep -Milliseconds 400
$during = Get-Pose
Start-Sleep -Seconds 2
$later = Get-Pose
Send-Cmd 'mirv_filmmaker path pause' | Out-Null

$moved = Distance $during $later
Check 'the path owns the view while playing' ($during -and $during.Owner -eq 'camera path') ("owner '{0}'" -f $during.Owner)
Check 'the path moves the camera' ($moved -gt 1.0) (
    "moved {0:N1} units over ~2s of the shot ({1:N0},{2:N0},{3:N0} -> {4:N0},{5:N0},{6:N0})" -f
    $moved, $during.X, $during.Y, $during.Z, $later.X, $later.Y, $later.Z)

# ---- 3. scrubbing previews without seeking -----------------------------------------------------
#
# Scrub to the MIDDLE of the path: outside it the evaluation clamps to an end marker and the pose
# stops changing, which reads as "scrubbing does nothing". This runs on a PAUSED replay on purpose:
# a paused Deadlock replay still runs the view hook and applies new poses every frame (measured:
# view writes keep rising while the demo tick holds), so the preview must move here too.
Send-Cmd ('mirv_filmmaker seek {0}' -f $anchor) | Out-Null
Start-Sleep -Seconds 3
$tickBefore = Get-Tick
$poseBefore = Get-Pose
# The scrub target is relative to $tickBefore (where the seek ACTUALLY landed), not to $anchor (what
# was asked for): demo_gototick snaps to the nearest keyframe rather than the exact requested tick,
# and that snap can itself land past a target computed from the nominal anchor -- which then reads
# as the scrub moving the playhead BACKWARD, when the scrub itself asked to go forward. Clamped
# inside the marker span so it still lands strictly between the two markers.
$scrubTarget = [Math]::Min($anchor + $span - 10, [Math]::Max($anchor + 10, $tickBefore + [int]($span / 2)))
Send-Cmd ('mirv_filmmaker scrub {0}' -f $scrubTarget) | Out-Null
Start-Sleep -Milliseconds 600
$poseScrub = Get-Pose
$tickScrub = Get-Tick
$scrubMoved = Distance $poseBefore $poseScrub
Send-Cmd 'mirv_filmmaker scrubend' | Out-Null
Start-Sleep -Seconds 2

Check 'scrubbing previews the path' ($scrubMoved -gt 1.0) ("moved {0:N1} units while scrubbing" -f $scrubMoved)
Check 'the scrub moves the playhead' ($tickScrub -ne $null -and $tickBefore -ne $null -and $tickScrub -gt $tickBefore) `
    ("playhead {0} -> {1}" -f $tickBefore, $tickScrub)

# ---- 4. the follow camera ------------------------------------------------------------------------
if ($entitiesDisabled) {
    Skip 'follow camera solves (lock-on)' 'entity integration is compiled out of this build'
    Skip 'follow camera owns the view' 'entity integration is compiled out of this build'
    Skip 'follow camera tracks its hero' 'entity integration is compiled out of this build'
    Skip 'attach mode solves' 'entity integration is compiled out of this build'
    Skip 'attach differs from lock-on' 'entity integration is compiled out of this build'
} elseif ($firstHero -lt 0) {
    Check 'follow camera solves (lock-on)' $false 'no hero index to target'
} else {
    Send-Cmd ('mirv_filmmaker follow hero {0};;mirv_filmmaker follow mode lockon;;mirv_filmmaker follow place' -f $firstHero) | Out-Null
    Start-Sleep -Seconds 1
    $follow = Send-Cmd 'mirv_filmmaker follow'
    $solving = $follow -match 'SOLVING at'
    Check 'follow camera solves (lock-on)' $solving (($follow -split "`n" | Where-Object { $_ -match 'SOLVING|not solving' }) -join ' ')

    $lockPose = Get-Pose
    Check 'follow camera owns the view' ($lockPose -and $lockPose.Owner -eq 'follow camera') ("owner '{0}'" -f $lockPose.Owner)

    # Let the replay run. LOCK-ON KEEPS ITS PLACED POSITION FIXED AND ONLY TURNS TO AIM (P5) -- an
    # earlier, Deadlock-only orbit model moved the camera's own position around the hero every
    # frame, which this check used to require; that model is retired, so "tracking" now means the
    # AIM (pitch/yaw) changes while the POSITION does not.
    Send-Cmd 'demo_resume' | Out-Null
    Start-Sleep -Seconds 3
    Send-Cmd 'demo_pause' | Out-Null
    $trackPose = Get-Pose
    $tracked = Distance $lockPose $trackPose
    $aimMoved = $trackPose -and $lockPose -and
        ([math]::Abs($trackPose.Pitch - $lockPose.Pitch) -gt 0.1 -or [math]::Abs($trackPose.Yaw - $lockPose.Yaw) -gt 0.1)
    Check 'lock-on camera position stays exactly where it was placed' ($tracked -ge 0 -and $tracked -lt 0.5) `
        ("moved {0:N2} units over ~3s of replay (must be ~0)" -f $tracked)
    Check 'follow camera tracks its hero (aim changes)' $aimMoved (
        "pitch {0:N1}->{1:N1}  yaw {2:N1}->{3:N1}" -f $lockPose.Pitch, $trackPose.Pitch, $lockPose.Yaw, $trackPose.Yaw)

    # ---- 5. attach ------------------------------------------------------------------------------
    Send-Cmd 'mirv_filmmaker follow mode attach' | Out-Null
    Send-Cmd 'demo_resume' | Out-Null
    Start-Sleep -Seconds 2
    Send-Cmd 'demo_pause' | Out-Null
    Start-Sleep -Milliseconds 500
    $attachReport = Send-Cmd 'mirv_filmmaker follow'
    $attachPose = Get-Pose
    $apart = Distance $trackPose $attachPose
    Check 'attach mode solves' ($attachReport -match 'SOLVING at') `
        (($attachReport -split "`n" | Where-Object { $_ -match 'SOLVING|not solving' }) -join ' ')
    Check 'attach differs from lock-on' ($apart -gt 1.0) (
        "{0:N1} units apart (lock-on {1:N0},{2:N0},{3:N0} -> attach {4:N0},{5:N0},{6:N0})" -f
        $apart, $trackPose.X, $trackPose.Y, $trackPose.Z, $attachPose.X, $attachPose.Y, $attachPose.Z)

    Send-Cmd 'mirv_filmmaker follow clear' | Out-Null
}

# ---- evidence ------------------------------------------------------------------------------------
$logDir = Join-Path $env:APPDATA 'HLAE\debuglogs'
$newestLog = Get-ChildItem -LiteralPath $logDir -Filter 'mvm_debug_*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestLog) { Copy-Item -LiteralPath $newestLog.FullName -Destination $OutDir }

$summary = [pscustomobject]@{
    replay   = $Replay
    port     = $Port
    failures = $script:Failures
    passed   = @($script:Results | Where-Object result -eq 'pass').Count
    skipped  = @($script:Results | Where-Object result -eq 'skip').Count
    checks   = $script:Results
    log      = $(if ($newestLog) { $newestLog.Name } else { $null })
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir 'summary.json') -Encoding UTF8

Write-Host ''
if ($script:Failures -eq 0) {
    Write-Host ("all camera checks passed ({0} passed, {1} skipped)" -f $summary.passed, $summary.skipped) -ForegroundColor Green
    exit 0
}
Write-Host ("{0} check(s) failed" -f $script:Failures) -ForegroundColor Red
exit 1
