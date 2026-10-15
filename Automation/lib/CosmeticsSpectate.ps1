#requires -Version 5
# Shared spectate + crash.veh helpers for cosmetics automation scripts.

$Script:CosmeticsDbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

function Get-ActualDemoTick {
    <#
    Reads the engine's own idea of the current demo tick via "mirv_skip tick"
    with no delta argument, which just echoes "Current demo tick: N" without
    moving anything (see Shared/MirvSkip.cpp).
    #>
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Netcon,
        [double]$ReadSeconds = 0.6
    )
    $t = & $Netcon @('mirv_skip tick') $ReadSeconds 'ticktap'
    if ($t -match 'Current demo tick:\s*(-?\d+)') { return [int]$Matches[1] }
    return $null
}

function Wait-DemoTickLanded {
    <#
    demo_gototick issues a seek that the engine finishes asynchronously by
    replaying packets to the requested position. A fixed
    sleep after sending the command is not proof the seek has landed: on a cold
    seek (first one after a fresh demo load, or a long jump) the engine can
    still be mid-replay when a fixed window like 3s expires, so the very next
    command we send gets queued behind an in-flight seek and the state we read
    afterward reflects a tick well past the one we asked for. Poll the engine's
    own tick counter until it actually matches (within tolerance) instead of
    trusting a guessed wait time.
    #>
    param(
        [Parameter(Mandatory = $true)][int]$TargetTick,
        [Parameter(Mandatory = $true)][scriptblock]$Netcon,
        [int]$Tolerance = 4,
        [int]$MaxWaitMs = 6000,
        [int]$PollMs = 300
    )
    $deadline = (Get-Date).AddMilliseconds($MaxWaitMs)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        $actual = Get-ActualDemoTick -Netcon $Netcon
        if ($null -ne $actual) {
            $last = $actual
            if ([Math]::Abs($actual - $TargetTick) -le $Tolerance) { return @{ landed = $true; actual = $actual } }
        }
        Start-Sleep -Milliseconds $PollMs
    }
    return @{ landed = $false; actual = $last }
}

function Test-Cs2Netcon {
    param(
        [int]$Port = 29010,
        [int]$TimeoutMs = 1500
    )
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $Port, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne($TimeoutMs)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    }
    catch { return $false }
    finally { $t.Dispose() }
}

function Wait-Cs2Netcon {
    param(
        [int]$Port = 29010,
        [int]$Retries = 20,
        [int]$TimeoutMs = 1500,
        [int]$SleepMs = 250
    )
    for ($i = 0; $i -lt $Retries; $i++) {
        if (Test-Cs2Netcon -Port $Port -TimeoutMs $TimeoutMs) { return $true }
        Start-Sleep -Milliseconds $SleepMs
    }
    return $false
}

function Get-Cs2ProcessAlive {
    return [bool](Get-Process -Name cs2 -ErrorAction SilentlyContinue)
}

function Get-MvmDebugLogFile {
    return Get-ChildItem $Script:CosmeticsDbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
}

function Get-CrashVehLines([string]$LogPath) {
    if (-not $LogPath -or -not (Test-Path -LiteralPath $LogPath)) { return @() }
    return @(Get-Content -LiteralPath $LogPath | Select-String 'crash\.veh')
}

function Get-CrashVehCount {
    $log = Get-MvmDebugLogFile
    if (-not $log) { return 0 }
    return @(Get-CrashVehLines $log.FullName).Count
}

function Export-CrashVehReport {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string]$OutDir,
        [int]$Port = 29010,
        [int]$BaselineCount = -1,
        [switch]$FailOnCrash
    )

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $netcon = Test-Cs2Netcon -Port $Port
    $cs2Proc = Get-Cs2ProcessAlive
    $log = Get-MvmDebugLogFile
    $crash = @()
    $logPath = $null
    if ($log) {
        $logPath = $log.FullName
        $crash = @(Get-CrashVehLines $logPath)
        $dest = Join-Path $OutDir ("mvm_debug_{0}.log" -f ($Tag -replace '[^\w.-]', '_'))
        Copy-Item -LiteralPath $logPath -Destination $dest -Force
    }
    $crashCount = $crash.Length
    $crashDelta = if ($BaselineCount -ge 0) { [Math]::Max(0, $crashCount - $BaselineCount) } else { $crashCount }
    $report = [ordered]@{
        tag = $Tag
        at = (Get-Date -Format o)
        netconAlive = $netcon
        cs2ProcessAlive = $cs2Proc
        crashVehCount = $crashCount
        crashVehDelta = $crashDelta
        crashVehBaseline = $(if ($BaselineCount -ge 0) { $BaselineCount } else { $null })
        crashVehLines = @($crash | ForEach-Object { $_.Line })
        mvmLog = $logPath
    }
    $reportFile = Join-Path $OutDir 'crash_report.jsonl'
    ($report | ConvertTo-Json -Depth 6 -Compress) | Add-Content -LiteralPath $reportFile -Encoding UTF8

    $crashForAlert = if ($BaselineCount -ge 0) { $crashDelta } else { $crashCount }
    $likelyCrash = (-not $cs2Proc) -or ($crashForAlert -gt 0)
    if ($likelyCrash -or (-not $netcon -and -not $cs2Proc)) {
        Write-Host ""
        Write-Host "======== CS2 / NETCON PROBLEM ($Tag) ========" -ForegroundColor Red
        Write-Host ("cs2.exe      : {0}" -f $cs2Proc) -ForegroundColor Red
        Write-Host ("netcon port  : {0}" -f $netcon) -ForegroundColor Red
        Write-Host ("crash.veh    : {0} (delta {1})" -f $crashCount, $crashDelta) -ForegroundColor Red
        if ($logPath) { Write-Host ("mvm log      : {0}" -f $logPath) -ForegroundColor DarkGray }
        $crash | Select-Object -Last 8 | ForEach-Object { Write-Host ("  {0}" -f $_.Line) -ForegroundColor Red }
        Write-Host "=============================================" -ForegroundColor Red
        if ($FailOnCrash) { throw "crash.veh=$crashCount cs2=$cs2Proc netcon=$netcon at $Tag" }
    }

    return $report
}

function Test-PlayerDeadFromDiag {
    param([hashtable]$Diag)
    if ($Diag.isDead) { return $true }
    if ($Diag.health -and [int]$Diag.health -le 0) { return $true }
    if (-not $Diag.steam -or "$($Diag.steam)" -eq '0') { return $true }
    $raw = "$($Diag.raw)"
    if ($raw -match 'is dead \(health=') { return $true }
    if ($raw -match 'no spectated pawn|pawn=\d+ not resolvable') { return $true }
    return $false
}

function Get-VisualDiagPaint {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $m = [regex]::Matches($Text, 'paint\(def6\)=(\d+)')
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    $m2 = [regex]::Matches($Text, 'fallback: paint=(\d+)')
    if ($m2.Count) { return $m2[$m2.Count - 1].Groups[1].Value }
    return $null
}

function Parse-VisualDiagFields {
    param([string]$Text)
    $health = if ($Text -match 'spectateHealth=(\d+)') { $Matches[1] } else { $null }
    $isDead = ($Text -match 'is dead \(health=') -or ($health -and [int]$health -le 0) -or ($Text -match 'no spectated pawn')
    return @{
        defIndex = if ($Text -match 'defIndex=(\d+)') { $Matches[1] } else { $null }
        steam = if ($Text -match 'spectateSteam=(\d+)') { $Matches[1] } else { $null }
        ownerXuid = if ($Text -match 'ownerXuid=(\d+)') { $Matches[1] } else { $null }
        weaponEntityId = if ($Text -match 'weapon: index=(\d+)') { $Matches[1] } else { $null }
        weaponClass = if ($Text -match "class='([^']+)'") { $Matches[1] } else { $null }
        pawn = if ($Text -match 'spectatePawn=(\d+)') { $Matches[1] } else { $null }
        paint = (Get-VisualDiagPaint $Text)
        hasNetworkedPaint = (Test-VisualDiagHasNetworkedPaint $Text)
        worldMeshMask = if ($Text -match 'worldMask=(\d+)') { $Matches[1] } else { $null }
        vmMeshMask = if ($Text -match 'vmMask=(\d+)') { $Matches[1] } else { $null }
        vmEntityId = if ($Text -match 'vmIdx=(\d+)') { $Matches[1] } else { $null }
        paintLegacy = if ($Text -match 'paintLegacy=(-?\d+)') { $Matches[1] } else { $null }
        meshMode = if ($Text -match 'meshMode=(\w+)') { $Matches[1] } else { $null }
        worldModel = if ($Text -match 'worldModel: (.+)') { ($Matches[1] -replace '\s+$','') } else { $null }
        reloadEvent = if ($Text -match 'm_nCustomEconReloadEventId\s*=\s*(-?\d+)') { $Matches[1] } else { $null }
        health = $health
        raw = $Text
        text = $Text
        isDead = [bool]$isDead
    }
}

function Get-ExpectedMeshMaskForPaint {
    # $Wp is untyped on purpose: it comes from ConvertFrom-Json (PSCustomObject with
    # nested PSCustomObject values), and PowerShell's implicit PSCustomObject-to-
    # Hashtable conversion is not reliable for nested shapes -- a [hashtable] type
    # constraint here throws and aborts the whole verify run on the first weapon
    # whose paint-pair object doesn't happen to convert cleanly.
    param([int]$PaintId, $Wp)
    if (-not $Wp) { return $null }
    if ($PaintId -eq [int]$Wp.legacyPaint.id) { return 2 }
    if ($PaintId -eq [int]$Wp.modernPaint.id) { return 1 }
    return $null
}

function Test-MeshMaskOk {
    param([hashtable]$Diag, [int]$ExpectedMask)
    if ($ExpectedMask -le 0) { return $true }
    # worldMask/vmMask are printed as %llu (unsigned 64-bit). An unresolved mask
    # comes back as the sentinel 18446744073709551615 (UInt64.MaxValue), which
    # overflows [int] and throws -- [uint64] holds the full range without
    # overflowing, and still compares correctly against the small (1/2) expected
    # mask values.
    $world = if ($Diag.worldMeshMask) { [uint64]$Diag.worldMeshMask } else { [uint64]0 }
    $vm = if ($Diag.vmMeshMask) { [uint64]$Diag.vmMeshMask } else { [uint64]0 }
    if ($world -eq $ExpectedMask) { return $true }
    if ($vm -eq $ExpectedMask) { return $true }
    return $false
}

function Test-VisualDiagHasNetworkedPaint {
    param([string]$Text)
    return ($Text -match 'paint\(def6\)=(\d+)')
}

function Resolve-WeaponPaintSteamId {
    param(
        [hashtable]$Diag,
        [string]$SpectateSteam = ''
    )
    $spectate = if ($SpectateSteam) { "$SpectateSteam" } else { "$($Diag.steam)" }
    $owner = if ($Diag.ownerXuid) { "$($Diag.ownerXuid)" } else { '' }
    $pickup = ($owner -and $spectate -and $owner -ne '0' -and $spectate -ne '0' -and $owner -ne $spectate)
    $paintSteam = if ($pickup) { $owner } else { $spectate }
    return @{
        paintSteam = $paintSteam
        spectateSteam = $spectate
        ownerSteam = $owner
        pickup = [bool]$pickup
    }
}

function Read-WeaponDiag {
    param(
        [int]$ExpectedDef,
        [string]$SpectateSteam,
        [scriptblock]$DiagReader,
        [scriptblock]$SpectateFn,
        [int]$Retries = 8,
        [int]$SleepMs = 200
    )
    for ($i = 0; $i -lt $Retries; $i++) {
        if ($i -gt 0 -and $SpectateSteam) {
            & $SpectateFn $SpectateSteam | Out-Null
            Start-Sleep -Milliseconds $SleepMs
        }
        $d = & $DiagReader
        if ($d.isDead) { return $d }
        # A spectate switch can echo the PREVIOUS player's data for one read --
        # reproduced directly right after a fresh demo load, where the very first
        # spectate call silently no-ops for a beat. Confirm we're actually looking
        # at the intended player before trusting either the def check or an
        # unknown-def "don't care" pass-through.
        $steamOk = (-not $SpectateSteam) -or ("$($d.steam)" -eq "$SpectateSteam")
        if ($steamOk) {
            if ($ExpectedDef -le 0) { return $d }
            if ($d.defIndex -and [int]$d.defIndex -eq $ExpectedDef) { return $d }
        }
        Start-Sleep -Milliseconds $SleepMs
    }
    return (& $DiagReader)
}

function Invoke-CosmeticsSpectate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SteamId,
        [Parameter(Mandatory = $true)][scriptblock]$Netcon
    )
    $text = & $Netcon @("mirv_filmmaker cosmetics spectate $SteamId") 0.9 "spec_$SteamId"
    $dead = ($text -match 'spectate: steam=\d+ dead')
    $ok = ($text -match 'spectate: steam=\d+ specIndex=\d+ health=\d+ ok')
    return @{ ok = $ok; dead = $dead; raw = $text }
}

function Invoke-CosmeticsSpectateAndDiag {
    <#
    Combined spectate+visualdiag in ONE netcon call instead of two. Each NC() call
    spawns its own powershell.exe + TCP connect (see cs2-netcon.ps1) -- for a candidate
    that turns out to be holding a knife/grenade (a skip, not a keep), that overhead is
    pure waste, and there can be many such candidates per probe tick. Halves it.
    Commands are still processed in order within one script invocation (the same
    trusted pattern already used for e.g. "demo_gototick" + "demo_pause"), so the
    diag read still reflects the just-applied spectate switch.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SteamId,
        [Parameter(Mandatory = $true)][scriptblock]$Netcon,
        [double]$ReadSeconds = 0.75
    )
    $text = & $Netcon @("mirv_filmmaker cosmetics spectate $SteamId", 'mirv_filmmaker cosmetics visualdiag') $ReadSeconds "specdiag_$SteamId"
    $dead = ($text -match 'spectate: steam=\d+ dead')
    $ok = ($text -match 'spectate: steam=\d+ specIndex=\d+ health=\d+ ok')
    $diag = Parse-VisualDiagFields $text
    return @{ ok = $ok; dead = $dead; diag = $diag; raw = $text }
}

function Lock-PlayerBySteam {
    param(
        [Parameter(Mandatory = $true)][string]$SteamId,
        [hashtable]$SlotMap = @{},
        [Parameter(Mandatory = $true)][scriptblock]$Netcon,
        [scriptblock]$ParseDiag = {},
        [int]$SpecSleepMs = 0
    )
    $r = Invoke-CosmeticsSpectate -SteamId $SteamId -Netcon $Netcon
    return $r.ok
}
