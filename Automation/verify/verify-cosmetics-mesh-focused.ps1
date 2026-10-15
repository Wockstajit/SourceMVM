#requires -Version 5
<#
Focused FP mesh + skin test: ONE demo load, stay on each player while their gun is active.

Per weapon (primary AK=7, secondary USP=61, SMG MP9=34):
  1. spec_next until that defIndex is the ACTIVE weapon (max tries, no demo reload)
  2. FP view (follow off), pause
  3. Apply legacy paint -> readback + screenshot
  4. Apply modern paint -> readback + screenshot (legacy body <-> CS2 body via paint kit)
  5. Same paint, toggle mesh modern|legacy -> geometry-only screenshots
  6. Tick back 64, play 2s, pause -> paint still correct (flicker smoke check)

Artifacts: Automation/output/cosmetics_mesh_focused/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [string]$WeaponDefs = "7,61",
    [string]$OutDir = "Automation\output\cosmetics_mesh_focused",
    [string]$WeaponCrop = "760,560,1500,1180",
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
$logFile = Join-Path $outAbs 'focused.log'
$proofFile = Join-Path $outAbs 'proof.json'
"=== mesh focused $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile

$meshPaintsPath = Join-Path $automationRoot 'config\weapon_mesh_test_paints.json'
$meshDoc = Get-Content -LiteralPath $meshPaintsPath -Raw -Encoding UTF8 | ConvertFrom-Json
$defList = @($WeaponDefs -split '[,\s]+' | Where-Object { $_ -match '^\d+$' })

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

function NC([string[]]$cmds, [double]$read = 2.0, [string]$tag = '') {
    if (-not (Test-Netcon $Port)) { throw "netcon dead ($tag): $($cmds -join ' | ')" }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -ErrorAction SilentlyContinue
    Add-Content -LiteralPath $logFile -Value ("--- $tag ---`n" + $t)
    return $t
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function Capture([string]$name) {
    $path = Join-Path $outAbs $name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}

function Diff-Mean([string]$a, [string]$b) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $WeaponCrop
    return [double]((LastMatch $json '"mean":\s*([0-9.]+)'))
}

function Get-MvmLogLines([int]$afterLine) {
    $log = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if (-not $log) { return @{ lines = @(); total = $afterLine } }
    $all = Get-Content $log.FullName
    $slice = if ($afterLine -lt $all.Count) { $all[$afterLine..($all.Count - 1)] } else { @() }
    return @{ lines = $slice; total = $all.Count; path = $log.FullName }
}

function Lock-PlayerWithWeapon([int]$wantDef, [int]$maxTries = 24) {
    for ($i = 0; $i -lt $maxTries; $i++) {
        $d = NC @('mirv_filmmaker cosmetics visualdiag') 1.8 "lock_def${wantDef}_$i"
        $def = LastMatch $d 'defIndex=(\d+)'
        $steam = LastMatch $d 'spectateSteam=(\d+)'
        $cls = LastMatch $d "class='([^']+)'"
        if ($def -eq "$wantDef" -and $steam -and $steam -ne '0') {
            Write-Host "  Locked def=$wantDef steam=$steam class=$cls (try $i)" -ForegroundColor Green
            return @{ steam = $steam; def = $def; class = $cls; tries = $i }
        }
        NC @('spec_next') 1.0 "spec_next_$i" | Out-Null
        Start-Sleep -Milliseconds 350
    }
    return $null
}

function Test-OneWeapon([int]$def) {
    $key = "$def"
    $wp = $meshDoc.weapons.$key
    if (-not $wp) { throw "No mesh paints config for def $def" }

    $lock = Lock-PlayerWithWeapon $def
    if (-not $lock) {
        return [ordered]@{
            defIndex = $def; weaponName = $wp.weaponName; found = $false
            pass = $false; error = "no player holding def $def at tick $GotoTick"
        }
    }

    NC @(
        'mirv_filmmaker follow stop', 'mirv_filmmaker follow clear',
        'demo_pause', 'mirv_filmmaker cosmetics ticknudge off',
        'mirv_filmmaker cosmetics mesh auto', 'mirv_filmmaker cosmetics enabled 1'
    ) 1.5 "setup_def$def" | Out-Null
    Start-Sleep -Milliseconds 400

    $legacyId = [int]$wp.legacyPaint.id
    $modernId = [int]$wp.modernPaint.id
    $toggleId = [int]$wp.meshTogglePaint
    $tag = "def${def}"

    # Legacy paint (legacy mesh body)
    NC @("mirv_filmmaker cosmetics player current weapon $def paint $legacyId wear 0.05 seed 0") 2.5 "legacy_$def" | Out-Null
    Start-Sleep -Milliseconds 700
    $dLeg = NC @('mirv_filmmaker cosmetics visualdiag') 2.0 "legacy_diag_$def"
    $paintLeg = LastMatch $dLeg 'paint\(def6\)=(\d+)'
    $shotLeg = Capture "${tag}_legacy_${legacyId}.png"

    # Modern paint (CS2 mesh body)
    NC @("mirv_filmmaker cosmetics player current weapon $def paint $modernId wear 0.05 seed 0") 2.5 "modern_$def" | Out-Null
    Start-Sleep -Milliseconds 700
    $dMod = NC @('mirv_filmmaker cosmetics visualdiag') 2.0 "modern_diag_$def"
    $paintMod = LastMatch $dMod 'paint\(def6\)=(\d+)'
    $shotMod = Capture "${tag}_modern_${modernId}.png"
    $paintDiff = Diff-Mean $shotLeg $shotMod

    # Mesh geometry toggle on SAME paint (manual modern vs legacy body)
    NC @('mirv_filmmaker cosmetics mesh modern',
        "mirv_filmmaker cosmetics player current weapon $def paint $toggleId wear 0.05 seed 0") 2.0 "mesh_mod_$def" | Out-Null
    Start-Sleep -Milliseconds 500
    $shotMeshMod = Capture "${tag}_mesh_modern_${toggleId}.png"
    NC @('mirv_filmmaker cosmetics mesh legacy') 1.5 "mesh_leg_$def" | Out-Null
    Start-Sleep -Milliseconds 500
    $shotMeshLeg = Capture "${tag}_mesh_legacy_${toggleId}.png"
    $meshDiff = Diff-Mean $shotMeshMod $shotMeshLeg
    NC @('mirv_filmmaker cosmetics mesh auto') 1.0 "mesh_auto_$def" | Out-Null

    # Flicker smoke: tick back + short play (same player, same demo — no reload)
    $mvmMark = (Get-MvmLogLines 0).total
    $tickBack = [Math]::Max(1, $GotoTick - 64)
    NC @("demo_gototick $tickBack", 'demo_pause') 5.0 "tickback_$def" | Out-Null
    Start-Sleep -Milliseconds 900
    NC @('demo_resume') 0.5 "resume_$def" | Out-Null
    Start-Sleep -Seconds 2
    NC @('demo_pause') 1.0 "pause_after_play_$def" | Out-Null
    $dPlay = NC @('mirv_filmmaker cosmetics visualdiag') 2.0 "after_play_$def"
    $paintPlay = LastMatch $dPlay 'paint\(def6\)=(\d+)'
    $playTail = Get-MvmLogLines $mvmMark
    $nudge = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.nudge.*START' }).Count
    $composite = @($playTail.lines | Where-Object { $_ -match 'cosmetics\.composite' }).Count

    $paintOk = ($paintLeg -eq "$legacyId") -and ($paintMod -eq "$modernId") -and ($paintPlay -eq "$modernId")
    $visibleOk = ($paintDiff -ge 0.5) -and ($meshDiff -ge 0.1)

    return [ordered]@{
        defIndex = $def
        weaponName = $wp.weaponName
        found = $true
        steam = $lock.steam
        weaponClass = $lock.class
        legacyPaint = @{ want = $legacyId; got = $paintLeg; shot = (Split-Path $shotLeg -Leaf) }
        modernPaint = @{ want = $modernId; got = $paintMod; shot = (Split-Path $shotMod -Leaf); paintDiff = $paintDiff }
        meshToggle = @{ paint = $toggleId; meshDiff = $meshDiff; modernShot = (Split-Path $shotMeshMod -Leaf); legacyShot = (Split-Path $shotMeshLeg -Leaf) }
        playback = @{ paint = $paintPlay; nudgeStarts = $nudge; compositeCalls = $composite }
        paintAppliedOk = $paintOk
        visuallyChangedOk = $visibleOk
        pass = $paintOk -and (Test-Netcon $Port)
    }
}

$proof = [ordered]@{ runAt = (Get-Date -Format o); demo = $Demo; tick = $GotoTick; weapons = @() }

try {
    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Write-Host "Launching CS2..." -ForegroundColor Cyan
        Get-Process -Name cs2,hlae -EA SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 10 -OutDir (Join-Path $outAbs 'launch')
    }

    Write-Host "Load demo ONCE at tick $GotoTick (no per-weapon reload)" -ForegroundColor Cyan
    NC @('mvm_debug start') 2.0 'mvm_debug' | Out-Null
    Start-Sleep -Seconds 1
    NC @("playdemo `"$Demo`"") 10.0 'playdemo' | Out-Null
    Start-Sleep -Seconds 2
    NC @("demo_gototick $GotoTick", 'demo_pause',
        'mirv_filmmaker follow stop', 'mirv_filmmaker follow clear') 5.0 'gototick' | Out-Null
    Start-Sleep -Seconds 1

    foreach ($defStr in $defList) {
        $def = [int]$defStr
        Write-Host "`n=== Weapon def $def ===" -ForegroundColor Yellow
        if (-not (Test-Netcon $Port)) { throw "CS2 died before def $def" }
        $r = Test-OneWeapon $def
        $proof.weapons += $r
        $color = if ($r.pass) { 'Green' } else { 'Red' }
        Write-Host ("  $($r.weaponName): legacy=$($r.legacyPaint.got) modern=$($r.modernPaint.got) meshDiff=$($r.meshToggle.meshDiff) pass=$($r.pass)") -ForegroundColor $color
        if (-not $r.found) { Write-Host "  skipped: no player with this gun active at this tick" -ForegroundColor DarkYellow }
        # Return to pause + same tick before next weapon hunt
        NC @("demo_gototick $GotoTick", 'demo_pause') 4.0 "rewind_$def" | Out-Null
        Start-Sleep -Milliseconds 600
    }

    $proof.overall_pass = (-not @($proof.weapons | Where-Object { -not $_.pass }).Count) -and (Test-Netcon $Port)
    $proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $proofFile -Encoding UTF8

    $mvm = Get-ChildItem $dbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if ($mvm) { Copy-Item $mvm.FullName (Join-Path $outAbs $mvm.Name) -Force }

    if (-not $proof.overall_pass) { exit 1 }
    Write-Host "`nPASS - proof: $proofFile" -ForegroundColor Green
    exit 0
}
catch {
    Add-Content -LiteralPath $logFile -Value "ERROR $($_.Exception.Message)"
    Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
    @{ error = $_.Exception.Message; partial = $proof } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $proofFile -Encoding UTF8
    exit 1
}
