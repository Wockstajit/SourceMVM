#requires -Version 5
<#
Regression test for the knife-type-swap crash fix (Docs/cosmetics/cosmetics-cs2-methodology-notes.md §11, approach #3).

A knife TYPE swap to a .vmdl whose animation resource was never loaded in the demo used to crash the game
(client.dll+0x3399cc, null per-model sequence list, on the anim worker thread). The fix is a client.dll
detour (CosmeticAnimFix.cpp) that substitutes an EMPTY sequence list for the builder's null out-param.

This test opens a demo, spectates a player, swaps their knife to a (typically unloaded) model, resumes
playback so the knife deploys and the engine animates it, and asserts:
  PASS  = CS2/netcon still alive AND the newest mvm_debug log has 0 crash.veh lines
          (bonus: knife.animfix fired, proving the detour neutralized real nulls).
  FAIL  = netcon died (crash) or a crash.veh line was recorded.

Artifacts: Automation/output/knife_swap_nocrash/
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = '',
    [string]$Demo = "replays/match730_003827583940474962026_0709708846_125",
    [int]$GotoTick = 4000,
    [int]$TargetKnife = 522,   # Stiletto: not carried in this demo -> unloaded -> exercises the fix
    [int]$WatchSeconds = 20,
    [string]$OutDir = "Automation\output\knife_swap_nocrash",
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
$dbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try { $c = $t.BeginConnect('127.0.0.1', $p, $null, $null); $ok = $c.AsyncWaitHandle.WaitOne(500); if ($ok) { $t.EndConnect($c) }; return $ok }
    catch { return $false } finally { $t.Dispose() }
}
function NC([string[]]$cmds, [double]$read = 2.0) {
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -ErrorAction SilentlyContinue
    return $t
}
function LastMatch([string]$t, [string]$p) { $m = [regex]::Matches($t, $p); if ($m.Count) { $m[$m.Count-1].Groups[1].Value } else { $null } }

if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
    Write-Host "Launching CS2 (netcon $Port)..." -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
        -Port $Port -Cs2Dir $Cs2Dir -SettleSeconds 6 -OutDir (Join-Path $outAbs 'launch')
}

NC @('mvm_debug start') 1.5 | Out-Null
NC @("playdemo `"$Demo`"") 9.0 | Out-Null
Start-Sleep 2
NC @("demo_gototick $GotoTick") 4.0 | Out-Null
Start-Sleep 2
NC @('demo_pause') 1.0 | Out-Null

$def=$null;$steam=$null
for ($i=0; $i -lt 8; $i++) {
    $d = NC @('mirv_filmmaker cosmetics visualdiag') 2.0
    $def = LastMatch $d 'defIndex=(\d+)'; $steam = LastMatch $d 'spectateSteam=(\d+)'
    if ($def -and [int]$def -gt 0 -and $steam -and $steam -ne '0') { break }
    NC @('spec_mode 4','spec_next') 1.5 | Out-Null; Start-Sleep -Milliseconds 600
}
if (-not $steam) { throw "no spectated player resolved" }
Write-Host "Spectating steam=$steam heldDef=$def ; swapping knife -> $TargetKnife" -ForegroundColor Green

NC @('mirv_filmmaker cosmetics enabled 1') 1.0 | Out-Null
NC @("mirv_filmmaker cosmetics player $steam knife $TargetKnife paint 0 wear 0.01 seed 0") 2.0 | Out-Null
NC @('demo_resume') 0.5 | Out-Null
Write-Host "Resuming playback; watching ${WatchSeconds}s for a crash..." -ForegroundColor Cyan
$crashed = $false
for ($s=0; $s -lt $WatchSeconds; $s++) { Start-Sleep 1; if (-not (Test-Netcon $Port)) { $crashed=$true; break } }

$alive = Test-Netcon $Port
$log = Get-ChildItem $dbgDir -Filter '*.log' | Sort-Object LastWriteTime -Desc | Select-Object -First 1
$crashLines = 0; $animfix = 0; $swaps = 0
if ($log) {
    $c = Get-Content $log.FullName
    $crashLines = ($c | Select-String 'crash\.veh').Count
    $animfix = ($c | Select-String 'knife\.animfix').Count
    $swaps = (($c | Select-String 'knife\.swap.*BEGIN').Count)
    Copy-Item $log.FullName (Join-Path $outAbs $log.Name) -Force
}
Write-Host "`n================ RESULT ================" -ForegroundColor Yellow
Write-Host ("netcon alive : {0}" -f $alive)
Write-Host ("crash.veh    : {0}" -f $crashLines)
Write-Host ("knife.animfix: {0}" -f $animfix)
Write-Host ("knife swaps  : {0}" -f $swaps)
Write-Host ("log          : {0}" -f $log.FullName)
Write-Host "=======================================" -ForegroundColor Yellow

if (-not $alive -or $crashLines -gt 0) {
    Write-Host "FAIL: knife swap crashed (alive=$alive crash.veh=$crashLines)." -ForegroundColor Red
    exit 1
}
Write-Host "PASS: $swaps knife swap(s), 0 crashes (animfix fired ${animfix}x)." -ForegroundColor Green
exit 0
