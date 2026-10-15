#requires -Version 5
<#
End-to-end FP mesh proof with verbose logs:
  * mesh_proof.log       — full netcon transcript
  * enumeration.log      — every spectate target per tick (def/class/paint)
  * steps.jsonl          — one JSON object per automation step
  * proof.json           — structured pass/fail summary + screenshot names
  * agent_debug_tail.log — copy of debug-af2ef9.log mesh lines
  * screenshots          — per-weapon legacy/modern paint + mesh toggle

Artifacts: Automation/output/cosmetics_mesh_proof/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "",
    [string]$ProbeTicks = "4000,8000,12000,16000,24000,32000",
    [string]$OutDir = "Automation\output\cosmetics_mesh_proof",
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
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'mesh_proof.log'
$enumFile = Join-Path $outAbs 'enumeration.log'
$stepsFile = Join-Path $outAbs 'steps.jsonl'
$proofFile = Join-Path $outAbs 'proof.json'
$agentTailFile = Join-Path $outAbs 'agent_debug_tail.log'
"=== mesh proof $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile
"=== spectate enumeration $(Get-Date -Format o) ===" | Set-Content -LiteralPath $enumFile

$meshPaintsPath = Join-Path $automationRoot 'config\weapon_mesh_test_paints.json'
$meshDoc = Get-Content -LiteralPath $meshPaintsPath -Raw -Encoding UTF8 | ConvertFrom-Json
$targetDefs = @('7', '34', '61')
$probeTickList = @($ProbeTicks -split '[,\s]+' | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ })
if ($probeTickList.Count -eq 0) { $probeTickList = @(4000) }

function Write-Step([string]$phase, [hashtable]$data = @{}) {
    $row = [ordered]@{ ts = (Get-Date -Format o); phase = $phase }
    foreach ($k in $data.Keys) { $row[$k] = $data[$k] }
    ($row | ConvertTo-Json -Compress) | Add-Content -LiteralPath $stepsFile -Encoding UTF8
}

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0, [string]$StepTag = '') {
    if (-not (Test-Netcon $Port)) {
        Write-Step 'netcon_dead' @{ tag = $StepTag; commands = ($Commands -join ' | ') }
        throw "CS2 netcon not reachable on port $Port (tag=$StepTag)"
    }
    $log = Join-Path $outAbs ('nc_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    try {
        & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log | Out-Null
    } catch {
        Write-Step 'netcon_error' @{ tag = $StepTag; error = $_.Exception.Message }
        throw
    }
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    Add-Content -LiteralPath $logFile -Value ("--- NETCON $StepTag ---`n" + $text)
    if ($StepTag) { Write-Step 'netcon' @{ tag = $StepTag; commands = ($Commands -join ' | '); bytes = $text.Length } }
    return $text
}

function Get-LastMatch([string]$text, [string]$pattern) {
    if ([string]::IsNullOrEmpty($text)) { return $null }
    $m = [regex]::Matches($text, $pattern)
    if ($m.Count -gt 0) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function Parse-VisualDiag([string]$text) {
    return [ordered]@{
        defIndex = Get-LastMatch $text 'defIndex=(\d+)'
        steam = Get-LastMatch $text 'spectateSteam=(\d+)'
        pawn = Get-LastMatch $text 'spectatePawn=(\d+)'
        weaponClass = Get-LastMatch $text "class='([^']+)'"
        paint = Get-LastMatch $text 'paint\(def6\)=(\d+)'
        wear = Get-LastMatch $text 'wear\(def8\)=([0-9.]+)'
        worldModel = Get-LastMatch $text 'worldModel:\s*(\S+)'
    }
}

function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    Write-Step 'screenshot' @{ file = $Name; exists = (Test-Path -LiteralPath $path) }
    return $path
}

function Diff-Mean([string]$a, [string]$b, [string]$crop) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $crop
    $mean = [double]((Get-LastMatch $json '"mean":\s*([0-9.]+)'))
    Write-Step 'image_diff' @{ a = (Split-Path $a -Leaf); b = (Split-Path $b -Leaf); mean = $mean }
    return $mean
}

function Resolve-DemoPath {
    $replayDir = Join-Path $Cs2Dir 'game\csgo\replays'
    if ($Demo) {
        $candidates = @(
            $Demo,
            (Join-Path $root $Demo),
            (Join-Path $Cs2Dir "game\csgo\$Demo"),
            (Join-Path $replayDir ($Demo -replace '^replays/', ''))
        )
        foreach ($c in $candidates) {
            $demFile = if ($c -match '\.dem$') { $c } else { "$c.dem" }
            if (Test-Path -LiteralPath $demFile) {
                $base = [System.IO.Path]::GetFileNameWithoutExtension($demFile)
                return "replays/$base"
            }
        }
    }
    $preferred = 'match730_003827583940474962026_0709708846_125'
    $pref = Join-Path $replayDir $preferred
    if (Test-Path -LiteralPath "$pref.dem") { return "replays/$preferred" }
    $any = Get-ChildItem -LiteralPath $replayDir -Filter '*.dem' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($any) { return "replays/$($any.BaseName)" }
    throw "No demo found under $replayDir"
}

function Load-Demo([string]$demoPath, [int]$tick) {
    Write-Step 'load_demo' @{ demo = $demoPath; tick = $tick }
    Invoke-Netcon -Commands @("playdemo `"$demoPath`"") -ReadSeconds 10.0 -StepTag "playdemo" | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @("demo_gototick $tick") -ReadSeconds 5.0 -StepTag "gototick_$tick" | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @('demo_pause', 'mirv_filmmaker follow stop', 'mirv_filmmaker follow clear',
        'mirv_filmmaker cosmetics ticknudge off') -ReadSeconds 1.5 -StepTag 'pause_fp' | Out-Null
}

function Enumerate-AllSpectateTargets([int]$tick, [int]$maxTries = 16) {
    $rows = [System.Collections.Generic.List[object]]::new()
    Add-Content -LiteralPath $enumFile -Value "`n--- tick $tick ---"
    for ($i = 0; $i -lt $maxTries; $i++) {
        $d = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0 -StepTag "enum_tick${tick}_try$i" 
        $p = Parse-VisualDiag $d
        $line = "try=$i def=$($p.defIndex) class=$($p.weaponClass) steam=$($p.steam) paint=$($p.paint) model=$($p.worldModel)"
        Add-Content -LiteralPath $enumFile -Value $line
        $rows.Add([ordered]@{
            tick = $tick; try = $i
            defIndex = $p.defIndex; weaponClass = $p.weaponClass
            steam = $p.steam; paint = $p.paint; worldModel = $p.worldModel
            isTarget = ($targetDefs -contains $p.defIndex)
        })
        Invoke-Netcon -Commands @('spec_next') -ReadSeconds 1.0 -StepTag "spec_next_$i" | Out-Null
        Start-Sleep -Milliseconds 400
    }
    return $rows
}

function Scan-SpectateWeapons([object[]]$enumRows) {
    $found = @{}
    foreach ($row in $enumRows) {
        $def = "$($row.defIndex)"
        if ($row.isTarget -and $row.steam -and $row.steam -ne '0' -and -not $found.ContainsKey($def)) {
            $found[$def] = @{ steam = $row.steam; tryIndex = $row.try; diag = $row }
            Write-Host "  TARGET def=$def steam=$($row.steam) class=$($row.weaponClass)" -ForegroundColor Green
            Write-Step 'target_found' @{ def = $def; steam = $row.steam; tick = $row.tick; class = $row.weaponClass }
        }
    }
    return $found
}

function Read-CosmeticsStatus {
    $st = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 1.8 -StepTag 'status'
    return [ordered]@{
        enabled = Get-LastMatch $st 'enabled=(\d+)'
        armed = Get-LastMatch $st 'armed=(\d+)'
        modelswapResolved = Get-LastMatch $st 'modelswap=\d+ resolved=(\d+)'
        weaponMesh = Get-LastMatch $st 'weaponMesh=(\d+)'
    }
}

function Test-WeaponMesh([string]$def, [hashtable]$info, [string]$demoPath, [int]$tick) {
    $wp = $meshDoc.weapons.$def
    if (-not $wp) { return $null }
    $steam = $info.steam
    $tag = "def${def}"
    $legacyId = [int]$wp.legacyPaint.id
    $modernId = [int]$wp.modernPaint.id
    $toggleId = [int]$wp.meshTogglePaint

    Write-Step 'weapon_test_begin' @{ def = $def; weapon = $wp.weaponName; steam = $steam; tick = $tick }

    for ($i = 0; $i -lt 14; $i++) {
        $d = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 1.8 -StepTag "lock_$def"
        $s = Get-LastMatch $d 'spectateSteam=(\d+)'
        if ($s -eq $steam) { break }
        Invoke-Netcon -Commands @('spec_next') -ReadSeconds 1.0 | Out-Null
    }

    $status0 = Read-CosmeticsStatus
    Invoke-Netcon -Commands @(
        'mirv_filmmaker cosmetics clear',
        'mirv_filmmaker cosmetics mesh auto',
        'mirv_filmmaker cosmetics enabled 1'
    ) -ReadSeconds 1.5 -StepTag "clear_$def" | Out-Null

    # --- legacy paint ---
    Invoke-Netcon -Commands @(
        "mirv_filmmaker cosmetics player current weapon $def paint $legacyId wear 0.05 seed 0"
    ) -ReadSeconds 2.5 -StepTag "legacy_paint_$def" | Out-Null
    Start-Sleep -Milliseconds 800
    $dLeg = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0 -StepTag "legacy_diag_$def"
    $paintLeg = Get-LastMatch $dLeg 'paint\(def6\)=(\d+)'
    $shotLeg = Capture "${tag}_legacy_paint_${legacyId}.png"
    Write-Step 'legacy_apply' @{ def = $def; want = $legacyId; got = $paintLeg; shot = (Split-Path $shotLeg -Leaf) }

    # --- modern paint ---
    Invoke-Netcon -Commands @(
        "mirv_filmmaker cosmetics player current weapon $def paint $modernId wear 0.05 seed 0"
    ) -ReadSeconds 2.5 -StepTag "modern_paint_$def" | Out-Null
    Start-Sleep -Milliseconds 800
    $dMod = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 2.0 -StepTag "modern_diag_$def"
    $paintMod = Get-LastMatch $dMod 'paint\(def6\)=(\d+)'
    $shotMod = Capture "${tag}_modern_paint_${modernId}.png"
    $paintDiff = Diff-Mean $shotLeg $shotMod $WeaponCrop
    Write-Step 'modern_apply' @{ def = $def; want = $modernId; got = $paintMod; shot = (Split-Path $shotMod -Leaf); diff = $paintDiff }

    # --- mesh toggle (same paint, geometry only) ---
    Invoke-Netcon -Commands @(
        'mirv_filmmaker cosmetics mesh modern',
        "mirv_filmmaker cosmetics player current weapon $def paint $toggleId wear 0.05 seed 0"
    ) -ReadSeconds 2.5 -StepTag "mesh_modern_cmd_$def" | Out-Null
    Start-Sleep -Milliseconds 500
    $dMeshM = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics diag') -ReadSeconds 2.0 -StepTag "mesh_modern_diag_$def"
    $shotMeshModern = Capture "${tag}_mesh_modern_${toggleId}.png"

    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh legacy') -ReadSeconds 1.5 -StepTag "mesh_legacy_cmd_$def" | Out-Null
    Start-Sleep -Milliseconds 500
    $dMeshL = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics diag') -ReadSeconds 2.0 -StepTag "mesh_legacy_diag_$def"
    $shotMeshLegacy = Capture "${tag}_mesh_legacy_${toggleId}.png"
    $meshDiff = Diff-Mean $shotMeshModern $shotMeshLegacy $WeaponCrop

    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh auto') -ReadSeconds 1.0 -StepTag "mesh_auto_$def" | Out-Null
    $statusEnd = Read-CosmeticsStatus

    $vmMeshBefore = Get-LastMatch $dMeshM 'vmMesh(?:Mask)?=(\d+)'
    if (-not $vmMeshBefore) { $vmMeshBefore = Get-LastMatch $dMeshM 'meshMask=(\d+)' }
    $vmMeshAfter = Get-LastMatch $dMeshL 'vmMesh(?:Mask)?=(\d+)'
    if (-not $vmMeshAfter) { $vmMeshAfter = Get-LastMatch $dMeshL 'meshMask=(\d+)' }

    Write-Step 'mesh_toggle' @{
        def = $def; togglePaint = $toggleId
        meshModernShot = (Split-Path $shotMeshModern -Leaf)
        meshLegacyShot = (Split-Path $shotMeshLegacy -Leaf)
        meshDiff = $meshDiff
        vmMeshModern = $vmMeshBefore; vmMeshLegacy = $vmMeshAfter
    }

    return [ordered]@{
        defIndex = [int]$def
        weaponName = $wp.weaponName
        steam = $steam
        demo = $demoPath
        tick = $tick
        statusStart = $status0
        statusEnd = $statusEnd
        legacyPaint = @{
            id = $legacyId; name = $wp.legacyPaint.name
            applied = ($paintLeg -eq "$legacyId"); readback = $paintLeg
            shot = (Split-Path $shotLeg -Leaf)
        }
        modernPaint = @{
            id = $modernId; name = $wp.modernPaint.name
            applied = ($paintMod -eq "$modernId"); readback = $paintMod
            shot = (Split-Path $shotMod -Leaf)
        }
        legacyVsModernPaintDiff = $paintDiff
        meshTogglePaint = @{ id = $toggleId; name = $wp.legacyPaint.name }
        meshModernShot = (Split-Path $shotMeshModern -Leaf)
        meshLegacyShot = (Split-Path $shotMeshLegacy -Leaf)
        meshToggleDiff = $meshDiff
        vmMeshModern = $vmMeshBefore
        vmMeshLegacy = $vmMeshAfter
        paintAppliedOk = (($paintLeg -eq "$legacyId") -and ($paintMod -eq "$modernId"))
        meshToggleVisible = ($meshDiff -ge 0.5)
        pass = (($paintLeg -eq "$legacyId") -and ($paintMod -eq "$modernId") -and (Test-Netcon $Port))
    }
}

function Save-AgentDebugTail {
    $agentLog = Join-Path $root 'debug-af2ef9.log'
    if (-not (Test-Path -LiteralPath $agentLog)) { return @() }
    $all = Get-Content -LiteralPath $agentLog -ErrorAction SilentlyContinue
    $meshLines = $all | Where-Object { $_ -match 'meshWrite|MIRROR|vm_mirror|modelMatch|RefreshViewmodel|meshBefore|meshAfter' }
    $tail = @($meshLines | Select-Object -Last 60)
    $tail | Set-Content -LiteralPath $agentTailFile -Encoding UTF8
    return $tail
}

function Find-HlaeDebugTail {
    $dir = Join-Path $env:APPDATA 'HLAE\debuglogs'
    if (-not (Test-Path -LiteralPath $dir)) { return $null }
    $latest = Get-ChildItem -LiteralPath $dir -Filter 'mvm_debug_*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) { return $null }
    $dest = Join-Path $outAbs 'hlae_debug_tail.log'
    $lines = Get-Content -LiteralPath $latest.FullName -Tail 120 -ErrorAction SilentlyContinue
    $lines | Set-Content -LiteralPath $dest -Encoding UTF8
    return @{ file = $latest.Name; dest = (Split-Path $dest -Leaf); lines = $lines.Count }
}

# --- main ---
$demoPath = Resolve-DemoPath
Write-Host "Demo: $demoPath" -ForegroundColor Cyan
Write-Step 'run_start' @{ demo = $demoPath; ticks = ($probeTickList -join ',') }

$proof = [ordered]@{
    runAt = (Get-Date -Format o)
    demo = $demoPath
    meshPaintsConfig = $meshPaintsPath
    targetDefs = $targetDefs
    enumeration = @()
    weaponsTested = @()
    errors = @()
    summary = @{}
}

try {
if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    Write-Host "Launching CS2..." -ForegroundColor Cyan
    Get-Process -Name cs2,hlae -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 12 -OutDir (Join-Path $outAbs 'launch')
    Write-Step 'cs2_launched' @{ port = $Port }
}

foreach ($tick in $probeTickList) {
    Write-Host "`n=== Tick $tick ===" -ForegroundColor Cyan
    Load-Demo $demoPath $tick
    $enumRows = Enumerate-AllSpectateTargets $tick
    $proof.enumeration += @($enumRows)
    $scan = Scan-SpectateWeapons $enumRows
    Add-Content -LiteralPath $logFile -Value ("SCAN tick=$tick targets=" + ($scan.Keys -join ','))

    foreach ($def in $scan.Keys) {
        if ($proof.weaponsTested | Where-Object { $_.defIndex -eq [int]$def }) { continue }
        Write-Host "Testing $($meshDoc.weapons.$def.weaponName) (def $def)..." -ForegroundColor Yellow
        try {
            $result = Test-WeaponMesh $def $scan[$def] $demoPath $tick
            if ($result) { $proof.weaponsTested += $result }
        } catch {
            $err = $_.Exception.Message
            Write-Step 'weapon_test_error' @{ def = $def; error = $err }
            $proof.errors += [ordered]@{ def = $def; tick = $tick; error = $err }
            Add-Content -LiteralPath $logFile -Value "ERROR def=$def tick=$tick $err"
        }
        if (-not (Test-Netcon $Port)) {
            Write-Step 'cs2_crashed' @{ def = $def; tick = $tick }
            Add-Content -LiteralPath $logFile -Value "CS2 crashed during def $def"
            break
        }
    }
    if ($proof.weaponsTested.Count -ge $targetDefs.Count) { break }
    if (-not (Test-Netcon $Port)) { break }
}

} catch {
    $proof.errors += [ordered]@{ fatal = $_.Exception.Message }
    Write-Step 'fatal' @{ error = $_.Exception.Message }
    Add-Content -LiteralPath $logFile -Value "FATAL $($_.Exception.Message)"
} finally {
$agentLines = Save-AgentDebugTail
$hlae = Find-HlaeDebugTail
$proof.agentDebugTailLines = $agentLines.Count
$proof.hlaeDebug = $hlae

$testedDefs = @($proof.weaponsTested | ForEach-Object { "$($_.defIndex)" })
$missing = @($targetDefs | Where-Object { $testedDefs -notcontains $_ })
$proof.summary = [ordered]@{
    weaponsTested = $proof.weaponsTested.Count
    weaponsPassed = @($proof.weaponsTested | Where-Object { $_.pass }).Count
    missingTargets = $missing
    allTargetsFound = ($missing.Count -eq 0)
    allPassed = (($proof.weaponsTested | Where-Object { -not $_.pass }).Count -eq 0) -and ($proof.weaponsTested.Count -gt 0)
    cs2Alive = (Test-Netcon $Port)
}

$proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $proofFile -Encoding UTF8
}

Write-Host "`n==== RESULTS ====" -ForegroundColor Cyan
foreach ($w in $proof.weaponsTested) {
    $status = if ($w.pass) { 'PASS' } else { 'FAIL' }
    Write-Host "[$status] $($w.weaponName): paint legacy=$($w.legacyPaint.applied) modern=$($w.modernPaint.applied) | paintDiff=$([math]::Round($w.legacyVsModernPaintDiff,2)) meshDiff=$([math]::Round($w.meshToggleDiff,2)) vmMesh $($w.vmMeshModern)->$($w.vmMeshLegacy)"
}
Write-Host "Logs: $logFile"
Write-Host "      $enumFile"
Write-Host "      $stepsFile"
Write-Host "      $agentTailFile"
Write-Host "Proof: $proofFile"

if ($proof.weaponsTested.Count -eq 0) {
    Write-Host "WARNING: No target weapons tested. See enumeration.log and mesh_proof.log" -ForegroundColor Yellow
    exit 2
}
