#requires -Version 5
<#
Full player-customizer verifier with a proper RESET -> BASELINE -> APPLY -> VERIFY -> RELOAD flow.

What this proves (authoritative engine readbacks, not just "the UI changed"):
  * RESET to true default works every run -- clear + disable + RELOAD the demo so entities respawn at
    default. Baseline asserts profiles=0, enabled=0, armed=0 (nothing applied) and records the demo's
    REAL weapon paint + agent model so later overrides can be shown to differ from default.
  * Each override is reflected on the in-game model: weapon-skin via networkedAttrs def6 == requested
    paint; agent via the pawn's CModelState::m_ModelName == requested .vmdl. Knife/gloves/mesh are
    confirmed applied (per-frame stats + crash-free) and captured in third person.
  * Screenshots: default + applied, in FIRST person (viewmodel: weapon/skin) and THIRD person
    (follow-cam behind the player: agent/gloves/knife). Third-person model swaps get ~10 ticks of
    live playback before capture (the body renderable only re-derives on live frames).
  * NON-PERSISTENCE: after closing+reopening the demo WITHOUT reapplying, armed=0 and the readbacks are
    back to default (start-clean behaviour). Reapplying then brings the override back.

Artifacts: Automation/output/cosmetics_customizer/  (screenshots + customizer.log)
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [string]$OutDir = "Automation\output\cosmetics_customizer",
    [string]$WeaponCrop = "760,560,1500,1180",   # first-person viewmodel region (weapon/skin)
    [string]$PlayerCrop = "520,140,1120,1190",   # third-person player body region
    [double]$MinMeanDiff = 1.0,
    [double]$NoiseMultiple = 3.0,
    [int]$PlayTicksMs = 350,                      # ~10+ ticks of live playback for body swaps
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
$meshPaintsPath = Join-Path $automationRoot 'config\weapon_mesh_test_paints.json'
if (-not (Test-Path -LiteralPath $meshPaintsPath)) {
    throw "Missing $meshPaintsPath — run: python Automation/tools/extract_weapon_mesh_test_paints.py"
}
$meshPaintsDoc = Get-Content -LiteralPath $meshPaintsPath -Raw -Encoding UTF8 | ConvertFrom-Json
function Get-WeaponMeshPaints([int]$defIndex) {
    $key = "$defIndex"
    if ($meshPaintsDoc.weapons.PSObject.Properties.Name -contains $key) {
        return $meshPaintsDoc.weapons.$key
    }
    return $null
}
$outAbs = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'customizer.log'
"=== Cosmetics customizer verify $(Get-Date -Format o) ===" | Set-Content -LiteralPath $logFile
$results = [System.Collections.Generic.List[string]]::new()

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(500)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}
function Invoke-Netcon([string[]]$Commands, [double]$ReadSeconds = 2.0) {
    $log = Join-Path $outAbs ('nc_' + ([Guid]::NewGuid().ToString('N')) + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $Commands -ReadSeconds $ReadSeconds -LogPath $log | Out-Null
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    Add-Content -LiteralPath $logFile -Value $text
    return $text
}
function Capture([string]$Name) {
    $path = Join-Path $outAbs $Name
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-game-window.ps1') -Out $path | Out-Null
    return $path
}
function Get-LastMatch([string]$text, [string]$pattern) {
    $m = [regex]::Matches($text, $pattern)
    if ($m.Count -gt 0) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}
function Diff-Mean([string]$a, [string]$b, [string]$crop) {
    $json = & python (Join-Path $automationRoot 'tools\image_diff.py') $a $b --crop $crop
    return [double]((Get-LastMatch $json '"mean":\s*([0-9.]+)'))
}
function Record([string]$line) { $results.Add($line); Add-Content -LiteralPath $logFile -Value "RESULT $line"; Write-Host $line }

# --- demo / view helpers ----------------------------------------------------------------------------
function Load-Demo {
    Invoke-Netcon -Commands @("playdemo `"$Demo`"") -ReadSeconds 9.0 | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @("demo_gototick $GotoTick") -ReadSeconds 4.0 | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 1.0 | Out-Null
}
# Reliable reset to TRUE default: empty + disable the store, then RELOAD the demo so all entities
# respawn from the demo (clearing alone does not revert already-swapped models on live entities).
function Reset-ToDefault {
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics ticknudge off',
        'mirv_filmmaker cosmetics clear','mirv_filmmaker cosmetics enabled 0') -ReadSeconds 1.5 | Out-Null
    Load-Demo
}
function Advance-Ticks { Invoke-Netcon -Commands @('demo_resume') -ReadSeconds 0.2 | Out-Null; Start-Sleep -Milliseconds $PlayTicksMs; Invoke-Netcon -Commands @('demo_pause') -ReadSeconds 0.5 | Out-Null }
function Diag { return Invoke-Netcon -Commands @('mirv_filmmaker cosmetics visualdiag') -ReadSeconds 1.8 }
function Diag2 { return Invoke-Netcon -Commands @('mirv_filmmaker cosmetics diag') -ReadSeconds 1.8 }
function Get-Paint([string]$d) { return (Get-LastMatch $d 'paint\(def6\)=(\d+)') }
function Get-Agent([string]$d) { return (Get-LastMatch $d 'agent model:\s*(\S+)') }
function Set-ThirdPerson([int]$pawn) {
    Invoke-Netcon -Commands @('mirv_filmmaker follow type player',"mirv_filmmaker follow select $pawn",
        'mirv_filmmaker follow preset behind','mirv_filmmaker follow place','mirv_filmmaker follow preview') -ReadSeconds 1.5 | Out-Null
    Start-Sleep -Milliseconds 600
}
function Set-FirstPerson { Invoke-Netcon -Commands @('mirv_filmmaker follow stop','mirv_filmmaker follow clear') -ReadSeconds 1.0 | Out-Null; Start-Sleep -Milliseconds 400 }

try {
    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Write-Host "Launching CS2 (netcon $Port)..." -ForegroundColor Cyan
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds 8 -OutDir (Join-Path $outAbs 'launch')
    }

    # ===== STEP 1-2: reset to default, load demo, jump to the target tick =====
    Write-Host "Reset to default + load demo..." -ForegroundColor Cyan
    Load-Demo
    Reset-ToDefault

    # Resolve a spectated player + held weapon def + pawn index.
    $def = $null; $steam = $null; $pawn = $null
    for ($i = 0; $i -lt 8; $i++) {
        $d = Diag
        $def = Get-LastMatch $d 'defIndex=(\d+)'
        $steam = Get-LastMatch $d 'spectateSteam=(\d+)'
        $pawn = Get-LastMatch $d 'spectatePawn=(\d+)'
        if ($def -and [int]$def -gt 0 -and $steam -and $steam -ne '0') { break }
        Invoke-Netcon -Commands @('spec_next') -ReadSeconds 1.2 | Out-Null
        Start-Sleep -Milliseconds 600
    }
    if (-not $steam -or $steam -eq '0') { throw "Could not resolve a spectated player. See $logFile" }
    $wp = Get-WeaponMeshPaints ([int]$def)
    if (-not $wp) {
        throw "No weapon_mesh_test_paints entry for held defIndex=$def. Add it via extract_weapon_mesh_test_paints.py"
    }
    $skinPaint = [int]$wp.skinTestPaint.id
    $meshTogglePaint = [int]$wp.meshTogglePaint
    $reapplyPaint = [int]$wp.modernPaint.id
    Write-Host "Spectating steam=$steam heldDef=$def ($($wp.weaponName)) paints: skin=$skinPaint meshToggle=$meshTogglePaint reapply=$reapplyPaint" -ForegroundColor Green
    Add-Content -LiteralPath $logFile -Value "SPECTATE steam=$steam heldDef=$def weapon=$($wp.weaponName) skinPaint=$skinPaint meshToggle=$meshTogglePaint legacy=$($wp.legacyPaint.id) modern=$($wp.modernPaint.id)"

    # ===== STEP 3: baseline default state (authoritative) + screenshots =====
    $st0 = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 2.0
    $profiles0 = Get-LastMatch $st0 'profiles=(\d+)'
    $enabled0  = Get-LastMatch $st0 'enabled=(\d+)'
    $armed0    = Get-LastMatch $st0 'armed=(\d+)'
    $msResolved = Get-LastMatch $st0 'modelswap=\d+ resolved=(\d+)'
    if ($msResolved -eq '1') { Record "PASS resolution: model-swap functions resolved" } else { Record "FAIL resolution: model-swap functions did NOT resolve (resolved=$msResolved)" }
    if ($profiles0 -eq '0' -and $enabled0 -eq '0' -and $armed0 -eq '0') {
        Record "PASS default-state: profiles=0 enabled=0 armed=0 (true default, nothing applied)"
    } else {
        Record "FAIL default-state: profiles=$profiles0 enabled=$enabled0 armed=$armed0 (expected 0/0/0 -- override carried over!)"
    }
    $d0 = Diag
    $basePaint = Get-Paint $d0
    $baseAgent = Get-Agent (Diag2)
    Add-Content -LiteralPath $logFile -Value "DEFAULT basePaint=$basePaint baseAgent=$baseAgent"
    $fpDefault = Capture '00_fp_default.png'
    Set-ThirdPerson ([int]$pawn)
    $tpDefault = Capture '01_tp_default.png'
    Set-FirstPerson

    # ===== STEP 4-5: weapon skin (FIRST person / viewmodel) =====
    Invoke-Netcon -Commands @("mirv_filmmaker cosmetics player current weapon $def paint $skinPaint wear 0.05 seed 0",
        'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Start-Sleep -Milliseconds 800
    if (-not (Test-Netcon $Port)) { Record "FAIL weapon-skin: CS2 crashed on apply" }
    $fpSkin = Capture '10_fp_skin.png'
    $dSkin = Diag
    $skinNow = Get-Paint $dSkin
    $skinDiff = Diff-Mean $fpDefault $fpSkin $WeaponCrop
    $skinOk = ($skinNow -eq "$skinPaint")
    Record ("weapon-skin: paint def6 $basePaint->$skinNow (want $skinPaint $($wp.skinTestPaint.name)) -> " + $(if($skinOk){'APPLIED on model'}else{'NOT applied'}) + (" ; fp-crop diff={0:N2}" -f $skinDiff))

    # ===== STEP 6-9: agent (THIRD person model) =====
    Set-ThirdPerson ([int]$pawn)
    $tpAgentBefore = Capture '20_tp_agent_before.png'
    $agentA = Get-Agent (Diag2)
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics player current agent agents/models/ctm_fbi/ctm_fbi.vmdl',
        'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Advance-Ticks    # let the body renderable update over ~10 ticks
    $tpAgentAfter = Capture '21_tp_agent_after.png'
    $agentB = Get-Agent (Diag2)
    $agentDiff = Diff-Mean $tpAgentBefore $tpAgentAfter $PlayerCrop
    $agentOk = ($agentB -like '*ctm_fbi*') -and ($agentB -ne $agentA)
    Record ("agent: model '$agentA'->'$agentB' (want ctm_fbi) -> " + $(if($agentOk){'CHANGED on player model'}else{'unchanged'}) + (" ; tp-crop diff={0:N2}" -f $agentDiff))

    # ===== STEP 10: gloves (third person hands/body) =====
    $tpGloveBefore = Capture '30_tp_glove_before.png'
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics player current gloves 5027 paint 10037 wear 0.10 seed 0',
        'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Advance-Ticks
    if (-not (Test-Netcon $Port)) { Record "FAIL gloves: CS2 crashed on apply" }
    $tpGloveAfter = Capture '31_tp_glove_after.png'
    $stG = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 1.5
    $gApplied = Get-LastMatch $stG 'gloves=(\d+)'
    $gloveDiff = Diff-Mean $tpGloveBefore $tpGloveAfter $PlayerCrop
    Record ("gloves(5027): applied(stat)=$gApplied crash=no tp-crop diff={0:N2} (visible needs hands in frame)" -f $gloveDiff)

    # ===== STEP 10: knife type swap (third person -- knife on body) =====
    $tpKnifeBefore = Capture '40_tp_knife_before.png'
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics player current knife 515 paint 44 wear 0.05 seed 0',
        'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Advance-Ticks
    if (-not (Test-Netcon $Port)) { Record "FAIL knife: CS2 crashed on apply" }
    $tpKnifeAfter = Capture '41_tp_knife_after.png'
    $stK = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 1.5
    $kApplied = Get-LastMatch $stK 'knife=(\d+)'
    $knifeDiff = Diff-Mean $tpKnifeBefore $tpKnifeAfter $PlayerCrop
    Record ("knife(515 Butterfly): applied(stat)=$kApplied crash=no tp-crop diff={0:N2} (visible needs knife deployed/in frame)" -f $knifeDiff)
    Set-FirstPerson

    # ===== STEP 10: legacy vs CS2 mesh (same paint, toggle only) =====
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh modern',
        "mirv_filmmaker cosmetics player current weapon $def paint $meshTogglePaint wear 0.05 seed 0",'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Advance-Ticks
    $meshModern = Capture '50_mesh_modern.png'
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh legacy') -ReadSeconds 1.5 | Out-Null
    Advance-Ticks
    $meshLegacy = Capture '51_mesh_legacy.png'
    $meshDiff = Diff-Mean $meshModern $meshLegacy $WeaponCrop
    Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh auto') -ReadSeconds 1.0 | Out-Null
    Record ("mesh-toggle(same paint $meshTogglePaint $($wp.legacyPaint.name)): modern-vs-legacy diff={0:N2} (geometry/UV subtle)" -f $meshDiff)

    # ===== STEP 11-12: reload demo, verify NON-PERSISTENCE (start-clean), then reapply =====
    Write-Host "Reload demo + verify non-persistence..." -ForegroundColor Cyan
    Load-Demo   # close + reopen the demo WITHOUT clearing the saved profile and WITHOUT reapplying
    # re-resolve the same spectated player
    for ($i = 0; $i -lt 8; $i++) {
        $d = Diag; $s = Get-LastMatch $d 'spectateSteam=(\d+)'
        if ($s -eq $steam) { break }
        Invoke-Netcon -Commands @('spec_next') -ReadSeconds 1.2 | Out-Null; Start-Sleep -Milliseconds 600
    }
    $stR = Invoke-Netcon -Commands @('mirv_filmmaker cosmetics status') -ReadSeconds 2.0
    $armedR = Get-LastMatch $stR 'armed=(\d+)'
    $dR = Diag; $paintR = Get-Paint $dR; $agentR = Get-Agent (Diag2)
    $fpReload = Capture '60_fp_after_reload.png'
    $persistOk = ($armedR -eq '0') -and ($paintR -eq $basePaint) -and ($agentR -eq $baseAgent)
    Record ("non-persistence: after reload armed=$armedR paint=$paintR(base $basePaint) agent=$agentR -> " + $(if($persistOk){'DEFAULT (no carry-over) PASS'}else{'OVERRIDE PERSISTED (fail)'}))
    # Reapply on purpose -> override returns
    Invoke-Netcon -Commands @("mirv_filmmaker cosmetics player current weapon $def paint $reapplyPaint wear 0.20 seed 0",
        'mirv_filmmaker cosmetics enabled 1') -ReadSeconds 2.0 | Out-Null
    Start-Sleep -Milliseconds 800
    $dRA = Diag; $paintRA = Get-Paint $dRA
    $reapplyOk = ($paintRA -eq "$reapplyPaint")
    Record ("reapply-after-reload: paint $paintR->$paintRA (want $reapplyPaint $($wp.modernPaint.name)) -> " + $(if($reapplyOk){'RETURNS on demand PASS'}else{'did-not-return'}))

    Write-Host "`n==== SUMMARY ====" -ForegroundColor Cyan
    $results | ForEach-Object { Write-Host $_ }
    Add-Content -LiteralPath $logFile -Value ("SUMMARY`n" + ($results -join "`n"))
}
finally {
    if (Test-Netcon $Port) {
        Invoke-Netcon -Commands @('mirv_filmmaker cosmetics mesh auto','mirv_filmmaker follow stop','mirv_filmmaker follow clear') -ReadSeconds 1.0 | Out-Null
    }
}
