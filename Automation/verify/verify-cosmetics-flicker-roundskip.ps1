#requires -Version 5
<#
Verifies:
  1) Weapon skin apply does NOT trigger cosmetics tick-nudge (no demo_resume flicker).
  2) Aggressive round-style seeks with cosmetics armed do not crash (crash.veh = 0).

Artifacts: Automation/output/cosmetics_flicker_roundskip/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [int]$SkinPaint = 637,
    [int]$RoundSkips = 14,
    [string]$OutDir = "Automation\output\cosmetics_flicker_roundskip",
    [switch]$NoLaunch
)

# CS2 install root. Empty means auto-detect (Steam registry + libraryfolders.vdf). This
# used to default to the author's own Steam library, a path no other machine has.
# Override with the parameter or $env:SOURCEMVM_CS2_DIR.
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) { $Cs2Dir = & (Join-Path $PSScriptRoot '..\lib\Resolve-Cs2Dir.ps1') }
if ([string]::IsNullOrWhiteSpace($Cs2Dir)) {
    throw 'Counter-Strike 2 was not found. Pass the install root explicitly or set SOURCEMVM_CS2_DIR.'
}

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'verify.log'
$proofFile = Join-Path $outAbs 'proof.json'
"=== flicker+roundskip $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

function NC([string[]]$cmds, [double]$read = 2.0) {
    if (-not (Test-Netcon $Port)) { throw "netcon dead before: $($cmds -join ' | ')" }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -ErrorAction SilentlyContinue
    Add-Content -LiteralPath $logFile -Value ("--- $($cmds -join ' | ') ---`n" + $t)
    return $t
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function Get-MvmLogTail([int]$afterLine) {
    $log = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $log) { return @{ path = ''; lines = @(); after = $afterLine } }
    $all = Get-Content $log.FullName
    $slice = if ($afterLine -lt $all.Count) { $all[$afterLine..($all.Count - 1)] } else { @() }
    return @{ path = $log.FullName; lines = $slice; total = $all.Count }
}

$results = [ordered]@{}

try {
    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Write-Host "Launching CS2..." -ForegroundColor Cyan
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -SettleSeconds 8 -OutDir (Join-Path $outAbs 'launch')
    }

    NC @('mvm_debug start') 2.5 | Out-Null
    Start-Sleep -Seconds 2
    if (-not (Test-Netcon $Port)) { throw 'CS2 netcon lost after mvm_debug start' }
    $mvmBefore = Get-MvmLogTail 0
    $lineMark = $mvmBefore.total

    NC @("playdemo `"$Demo`"") 9.0 | Out-Null
    Start-Sleep 2
    NC @("demo_gototick $GotoTick", 'demo_pause') 4.5 | Out-Null
    Start-Sleep 1

    $def = $null; $steam = $null
    for ($i = 0; $i -lt 10; $i++) {
        $d = NC @('mirv_filmmaker cosmetics visualdiag') 2.0
        $def = LastMatch $d 'defIndex=(\d+)'
        $steam = LastMatch $d 'spectateSteam=(\d+)'
        if ($def -and [int]$def -gt 0 -and $steam -and $steam -ne '0') { break }
        NC @('spec_next') 1.2 | Out-Null
        Start-Sleep -Milliseconds 500
    }
    if (-not $steam) { throw 'no spectated player' }
    Write-Host "Spectating steam=$steam def=$def" -ForegroundColor Green

    NC @('mirv_filmmaker cosmetics ticknudge on', 'mirv_filmmaker cosmetics enabled 1') 1.5 | Out-Null

    # --- flicker test: weapon skin should NOT schedule tick nudge ---
    $lineMark = (Get-MvmLogTail 0).total
    NC @("mirv_filmmaker cosmetics player current weapon $def paint $SkinPaint wear 0.05 seed 0") 2.5 | Out-Null
    Start-Sleep -Seconds 2.5
    if (-not (Test-Netcon $Port)) { throw 'CS2 died during skin apply' }

    $tail = Get-MvmLogTail $lineMark
    $nudgeStart = @($tail.lines | Where-Object { $_ -match 'cosmetics\.nudge.*START' }).Count
    $demoResume = @($tail.lines | Where-Object { $_ -match 'demo_resume|CGameRules - unpaused' }).Count
    $d = NC @('mirv_filmmaker cosmetics visualdiag') 2.0
    $paintNow = LastMatch $d 'paint\(def6\)=(\d+)'
    $skinOk = ($paintNow -eq "$SkinPaint")

    $results.flicker_nudgeStarts = $nudgeStart
    $results.flicker_demoResumeLines = $demoResume
    $results.flicker_paintApplied = $skinOk
    $results.flicker_pass = ($nudgeStart -eq 0) -and $skinOk

    Write-Host ("FLICKER: nudgeStarts=$nudgeStart demoResume=$demoResume paint=$paintNow want=$SkinPaint pass=$($results.flicker_pass)") -ForegroundColor $(if ($results.flicker_pass) { 'Green' } else { 'Red' })

    # Tick-back + resume playback (the user-reported flicker repro).
    $lineMarkPlay = (Get-MvmLogTail 0).total
    $tickBack = [Math]::Max(1, $GotoTick - 64)
    NC @("demo_gototick $tickBack", 'demo_pause') 5.0 | Out-Null
    Start-Sleep -Milliseconds 1200
    $lineMarkPlay = (Get-MvmLogTail 0).total
    NC @('demo_resume') 0.5 | Out-Null
    Write-Host "Playing 3s after tick-back from $GotoTick to $tickBack..." -ForegroundColor Cyan
    for ($s = 0; $s -lt 3; $s++) {
        if (-not (Test-Netcon $Port)) { throw "CS2 died during tick-back playback second $s" }
        Start-Sleep -Seconds 1
    }
    NC @('demo_pause') 1.0 | Out-Null
    $playTail = Get-MvmLogTail $lineMarkPlay
    $compositeDuringPlay = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.composite' }).Count
    $meshFullDuringPlay = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.weapon\.mesh.*fullRefresh=1' }).Count
    $vmMirrorDuringPlay = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.weapon\.vm.*MIRROR ' }).Count
    $nudgeDuringPlay = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.nudge.*START' }).Count
    $diagPlay = NC @('mirv_filmmaker cosmetics visualdiag') 2.0
    $paintAfterPlay = LastMatch $diagPlay 'paint\(def6\)=(\d+)'
    $results.playback_compositeCalls = $compositeDuringPlay
    $results.playback_meshFullRefresh = $meshFullDuringPlay
    $results.playback_vmMirrorCalls = $vmMirrorDuringPlay
    $results.playback_nudgeStarts = $nudgeDuringPlay
    $results.playback_paint = $paintAfterPlay
    # During 3s playback, debounced paths should not hammer composite/mesh/vm every frame.
    $results.playback_flicker_pass = ($compositeDuringPlay -le 8) -and ($meshFullDuringPlay -le 4) -and ($vmMirrorDuringPlay -le 4) -and ($nudgeDuringPlay -eq 0) -and ($paintAfterPlay -eq "$SkinPaint")
    Write-Host ("PLAYBACK: composite=$compositeDuringPlay meshFull=$meshFullDuringPlay vmMirror=$vmMirrorDuringPlay nudge=$nudgeDuringPlay paint=$paintAfterPlay pass=$($results.playback_flicker_pass)") -ForegroundColor $(if ($results.playback_flicker_pass) { 'Green' } else { 'Yellow' })

    # Round-skip stress with weapon skin only (no glove/knife nudge racing seeks)
    $seekTicks = @(4000, 15234, 28342, 40085, 58831, 72618, 87563, 104802, 108268, 113717, 124444, 135118, 96000, 32000, 8000)
    $lineMarkSeek = (Get-MvmLogTail 0).total
    for ($r = 0; $r -lt $RoundSkips; $r++) {
        if (-not (Test-Netcon $Port)) { throw "CS2 died on round-skip iteration $r" }
        $t = $seekTicks[$r % $seekTicks.Count]
        NC @("demo_gototick $t", 'demo_pause') 5.0 | Out-Null
        Start-Sleep -Seconds 1.5
        NC @('mirv_skip tick 6400') 3.0 | Out-Null
        Start-Sleep -Seconds 1.5
        Start-Sleep -Seconds 3
        NC @('spec_next') 1.0 | Out-Null
        Start-Sleep -Seconds 2
        Write-Host "  round-skip $r tick=$t alive=$(Test-Netcon $Port)" -ForegroundColor DarkGray
    }

    $alive = Test-Netcon $Port
    $mvmLog = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    $crashLines = 0; $seekLines = 0; $nudgeAbort = 0
    if ($mvmLog) {
        $c = Get-Content $mvmLog.FullName
        $crashLines = ($c | Select-String 'crash\.veh').Count
        $seekLines = ($c | Select-String 'cosmetics\.seek').Count
        $nudgeAbort = ($c | Select-String 'cosmetics\.nudge.*ABORT').Count
        Copy-Item $mvmLog.FullName (Join-Path $outAbs $mvmLog.Name) -Force
    }

    $results.roundskip_alive = $alive
    $results.roundskip_crashVeh = $crashLines
    $results.roundskip_seekLogged = $seekLines
    $results.roundskip_nudgeAborts = $nudgeAbort
    $results.roundskip_pass = $alive -and ($crashLines -eq 0)

    Write-Host ("ROUND-SKIP: alive=$alive crash.veh=$crashLines seek=$seekLines nudgeAbort=$nudgeAbort pass=$($results.roundskip_pass)") -ForegroundColor $(if ($results.roundskip_pass) { 'Green' } else { 'Red' })

    $results.flicker_pass = ($nudgeStart -eq 0) -and $skinOk
    $results.overall_pass = $results.flicker_pass -and $results.playback_flicker_pass -and $results.roundskip_pass
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $proofFile -Encoding UTF8

    if (-not $results.overall_pass) { exit 1 }
    Write-Host "PASS: flicker + round-skip checks OK. proof=$proofFile" -ForegroundColor Green
    exit 0
}
catch {
    Add-Content -LiteralPath $logFile -Value "ERROR $($_.Exception.Message)"
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    @{ error = $_.Exception.Message; partial = $results } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    exit 1
}
