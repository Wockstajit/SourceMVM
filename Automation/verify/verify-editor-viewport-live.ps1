#requires -Version 5
<#
Live verifier for the camera-editor viewport/HUD remap.

Attach to an existing CS2 netcon:
  powershell.exe -ExecutionPolicy Bypass -File Automation\verify\verify-editor-viewport-live.ps1 -Demo "replays/<name>"

Launch and test the standard automation resolution:
  powershell.exe -ExecutionPolicy Bypass -File Automation\verify\verify-editor-viewport-live.ps1 -LaunchEachResolution -Demo "replays/<name>"

What it proves:
  * debug overlay publishes full-screen data before the editor is opened,
  * editor-open world blit matches the published viewport,
  * native HUD panel records exist for every tracked HUD/editor descriptor,
  * HUD panels stay inside the viewport and out of editor chrome,
  * closed/open/timeline/graph HUD positions stay locked in viewport-normalized pixel space,
  * graph/timeline editor roots are not scaled as game-HUD children.
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Demo = '',
    [int]$DemoIndex = 15,
    [int]$LoadPolls = 28,
    [string]$OutDir,
    [switch]$LaunchEachResolution,
    [string]$Cs2Dir = '',
    [int]$NetconConnectTimeoutSeconds = 15,
    [int]$SeekTick = -1,
    [int]$SeekSettleSeconds = 3
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
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = New-AutomationRunFolder -Name 'verify-editor-viewport' }
else { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$resolutions = @(
    @{ name = '4x3'; w = 1600; h = 1200 }
)
if (-not $LaunchEachResolution) {
    $resolutions = @(@{ name = 'attached'; w = 0; h = 0 })
}

Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-editor-viewport' -Additional @{
    port = $Port
    demo = $Demo
    launchEachResolution = [bool]$LaunchEachResolution
    netconConnectTimeoutSeconds = $NetconConnectTimeoutSeconds
    seekTick = $SeekTick
    seekSettleSeconds = $SeekSettleSeconds
} | Out-Null

$script:transcriptStarted = $false
$transcriptPath = Join-Path $OutDir 'verification.log'
try {
    Start-Transcript -LiteralPath $transcriptPath -Force | Out-Null
    $script:transcriptStarted = $true
    Write-Host "Log: $transcriptPath"
} catch {
    Write-Host "Transcript unavailable: $($_.Exception.Message)" -ForegroundColor Yellow
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$name, [bool]$cond) {
    if ($cond) { Write-Host "[ PASS ] $name" -ForegroundColor Green; $script:results.Add("PASS $name") | Out-Null }
    else { Write-Host "[ FAIL ] $name" -ForegroundColor Red; $script:results.Add("FAIL $name") | Out-Null }
}

$trackedHudPanels = @(
    'chat','money','health','armor','ammo','weapon-panel','death-notices','minimap',
    'minimap-content','score-timer','player-cards','player-cards-content',
    'weapon-panel-content','round-win-panel','round-win-container',
    'round-win-result','round-win-mvp-section','round-win-mvp','hud-alerts',
    'progress-bar','vote','instructor','reticle','damage-indicator','radio',
    'trueview-active'
)
$requiredPanelDescriptors = @(
    $trackedHudPanels + @('camera-editor-panel','graph-editor-panel','camera-timeline-panel')
)
$optionalMissingPanels = @(
    'round-win-container','round-win-result','round-win-mvp-section','round-win-mvp',
    'hud-alerts','progress-bar','vote','instructor','reticle','damage-indicator',
    'radio','trueview-active'
)

$capture = Join-Path $automationRoot 'capture\capture-main-monitor.ps1'
function Shot($runDir, [string]$tag) {
    if (-not (Test-Path $capture)) { return }
    $p = Join-Path $runDir ("{0}.png" -f $tag)
    try {
        $shotOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture -Out $p 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path $p)) {
            Write-Host "  shot -> $p"
        } else {
            $msg = (($shotOutput | Out-String).Trim())
            if ([string]::IsNullOrWhiteSpace($msg)) { $msg = "capture failed with exit code $LASTEXITCODE" }
            Write-Host "  shot skipped: $msg" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  shot skipped: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Connect-Netcon([int]$connectPort, [int]$timeoutSeconds) {
    $client = New-Object System.Net.Sockets.TcpClient
    $async = $null
    try {
        $async = $client.BeginConnect('127.0.0.1', $connectPort, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne([TimeSpan]::FromSeconds($timeoutSeconds))) {
            throw "timed out after ${timeoutSeconds}s"
        }
        $client.EndConnect($async)
        $client.NoDelay = $true
        return $client
    } catch {
        try { $client.Close() } catch {}
        throw "No CS2 netcon on 127.0.0.1:$connectPort ($($_.Exception.Message))."
    } finally {
        if ($async -and $async.AsyncWaitHandle) { $async.AsyncWaitHandle.Close() }
    }
}

function New-NetconApi($client) {
    $stream = $client.GetStream()
    $enc = [System.Text.Encoding]::ASCII
    $api = [ordered]@{}
    $api.Drain = {
        param([double]$seconds)
        $deadline = (Get-Date).AddSeconds($seconds)
        $sb = New-Object System.Text.StringBuilder
        $buf = New-Object byte[] 16384
        while ((Get-Date) -lt $deadline) {
            if ($stream.DataAvailable) {
                $n = $stream.Read($buf, 0, $buf.Length)
                if ($n -gt 0) { [void]$sb.Append($enc.GetString($buf, 0, $n)) }
            } else {
                Start-Sleep -Milliseconds 30
            }
        }
        $sb.ToString()
    }.GetNewClosure()
    $api.Send = {
        param([string]$command, [double]$seconds = 0.7)
        & $api.Drain 0.08 | Out-Null
        $bytes = $enc.GetBytes($command + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        & $api.Drain $seconds
    }.GetNewClosure()
    $api.ReadState = {
        $t = & $api.Send 'mirv_filmmaker editor state64' 1.3
        $m = [regex]::Matches($t, '\[cameraeditor\]\[state64\]\s+(\d+)/(\d+)\s+([A-Za-z0-9+/=]+)')
        if ($m.Count -eq 0) { return $null }
        $parts = @{}; $expected = 0
        foreach ($x in $m) {
            $i = [int]$x.Groups[1].Value
            $total = [int]$x.Groups[2].Value
            if ($i -eq 1) { $parts = @{}; $expected = $total }
            $parts[$i] = $x.Groups[3].Value
        }
        if ($expected -eq 0 -or $parts.Count -lt $expected) { return $null }
        $encoded = (1..$expected | ForEach-Object { $parts[$_] }) -join ''
        try { return ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded)) | ConvertFrom-Json) }
        catch { return $null }
    }.GetNewClosure()
    return $api
}

function Get-PanelMap($state) {
    $map = @{}
    $dbg = Get-ObjectProperty $state 'dbg' $null
    $panels = Get-ObjectProperty $dbg 'panels' $null
    if ($panels) {
        foreach ($p in @($panels)) {
            $name = Get-ObjectProperty $p 'name' ''
            if ($name) { $map[$name] = $p }
        }
    }
    return $map
}

function Get-ObjectProperty($obj, [string]$name, $default) {
    if ($null -eq $obj) { return $default }
    $prop = $obj.PSObject.Properties[$name]
    if ($null -eq $prop) { return $default }
    return $prop.Value
}

function Quote-ConsoleArg([string]$value) {
    if ($null -eq $value) { return '""' }
    return '"' + ($value -replace '"', '\"') + '"'
}

function Get-RectArea($rect) {
    if (-not $rect) { return 0.0 }
    return [math]::Round([math]::Max(0.0, (Get-RectField $rect 'w')) * [math]::Max(0.0, (Get-RectField $rect 'h')), 2)
}

function Get-PanelArea($panel, [string]$kind) {
    if (-not $panel) { return 0.0 }
    if ($kind -eq 'source') {
        $sourceArea = Get-ObjectProperty $panel 'sourceArea' $null
        if ($null -ne $sourceArea) { return [double]$sourceArea }
    }
    if ($kind -eq 'final') {
        $finalArea = Get-ObjectProperty $panel 'finalArea' $null
        if ($null -ne $finalArea) { return [double]$finalArea }
    }
    return Get-RectArea (Get-ObjectProperty $panel $kind $null)
}

function Format-PanelRect($rect) {
    if (-not $rect) { return 'n/a' }
    return ("x={0:n0} y={1:n0} w={2:n0} h={3:n0}" -f `
        (Get-RectField $rect 'x'), (Get-RectField $rect 'y'), (Get-RectField $rect 'w'), (Get-RectField $rect 'h'))
}

function Get-NormalizedRect($rect, $viewport) {
    if (-not $rect -or -not $viewport -or (Get-RectField $viewport 'w') -le 0 -or (Get-RectField $viewport 'h') -le 0) { return $null }
    [pscustomobject]@{
        x = ((Get-RectField $rect 'x') - (Get-RectField $viewport 'x')) / (Get-RectField $viewport 'w')
        y = ((Get-RectField $rect 'y') - (Get-RectField $viewport 'y')) / (Get-RectField $viewport 'h')
        w = (Get-RectField $rect 'w') / (Get-RectField $viewport 'w')
        h = (Get-RectField $rect 'h') / (Get-RectField $viewport 'h')
    }
}

function Convert-NormalizedToRect($norm, $viewport) {
    if (-not $norm -or -not $viewport) { return $null }
    [pscustomobject]@{
        x = (Get-RectField $viewport 'x') + (Get-RectField $norm 'x') * (Get-RectField $viewport 'w')
        y = (Get-RectField $viewport 'y') + (Get-RectField $norm 'y') * (Get-RectField $viewport 'h')
        w = (Get-RectField $norm 'w') * (Get-RectField $viewport 'w')
        h = (Get-RectField $norm 'h') * (Get-RectField $viewport 'h')
    }
}

function Get-MaxRectError($actual, $expected) {
    if (-not $actual -or -not $expected) { return 999999.0 }
    $errs = @(
        [math]::Abs((Get-RectField $actual 'x') - (Get-RectField $expected 'x')),
        [math]::Abs((Get-RectField $actual 'y') - (Get-RectField $expected 'y')),
        [math]::Abs((Get-RectField $actual 'w') - (Get-RectField $expected 'w')),
        [math]::Abs((Get-RectField $actual 'h') - (Get-RectField $expected 'h'))
    )
    return ($errs | Measure-Object -Maximum).Maximum
}

function Get-RectField($rect, [string]$field) {
    return [double](Get-ObjectProperty $rect $field 0.0)
}

function Test-PanelInactive($panel) {
    if (-not $panel) { return $true }
    if ([bool](Get-ObjectProperty $panel 'missing' $false)) { return $true }
    if ([bool](Get-ObjectProperty $panel 'inactive' $false)) { return $true }
    return ((Get-PanelArea $panel 'final') -le 1.0)
}

function Test-OptionalMissingPanel([string]$name) {
    return ($script:optionalMissingPanels -contains $name)
}

function Write-PanelBounds($state, [string]$prefix, [string[]]$names) {
    $map = Get-PanelMap $state
    Write-Host ""
    Write-Host "$prefix live HUD pixel bounds" -ForegroundColor Cyan
    foreach ($name in $names) {
        if (-not $map.ContainsKey($name)) {
            Write-Host ("  {0}: missing descriptor" -f $name) -ForegroundColor Yellow
            continue
        }
        $p = $map[$name]
        if ([bool](Get-ObjectProperty $p 'missing' $false)) {
            Write-Host ("  {0}: missing" -f $name) -ForegroundColor Yellow
            continue
        }
        $inactive = Test-PanelInactive $p
        $source = Get-ObjectProperty $p 'source' $null
        $final = Get-ObjectProperty $p 'final' $null
        $viewport = Get-ObjectProperty $p 'viewport' $null
        $cropPct = [double](Get-ObjectProperty $p 'cropPct' 0.0)
        $editorOverlapPct = [double](Get-ObjectProperty $p 'editorOverlapPct' 0.0)
        $status = if ($inactive) { 'inactive' } else { 'active' }
        Write-Host ("  {0} [{1}]: source {2} area={3:n0}; final {4} area={5:n0}; viewport {6}; crop={7:n1}% overlap={8:n1}%" -f `
            $name, $status, (Format-PanelRect $source), (Get-PanelArea $p 'source'), `
            (Format-PanelRect $final), (Get-PanelArea $p 'final'), `
            (Format-PanelRect $viewport), $cropPct, $editorOverlapPct)
    }
}

function Export-PanelSnapshot($state, [string]$runDir, [string]$tag) {
    if (-not (Has-PanelData $state)) { return }
    $dbg = Get-ObjectProperty $state 'dbg' $null
    $panels = @(Get-ObjectProperty $dbg 'panels' @())
    $jsonPath = Join-Path $runDir ("{0}-hud-panels.json" -f $tag)
    $csvPath = Join-Path $runDir ("{0}-hud-panels.csv" -f $tag)
    $panels | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
    $rows = foreach ($p in $panels) {
        $source = Get-ObjectProperty $p 'source' $null
        $final = Get-ObjectProperty $p 'final' $null
        $viewport = Get-ObjectProperty $p 'viewport' $null
        $scale = Get-ObjectProperty $p 'scale' $null
        $offset = Get-ObjectProperty $p 'offset' $null
        [pscustomobject]@{
            name = [string](Get-ObjectProperty $p 'name' '')
            id = [string](Get-ObjectProperty $p 'id' '')
            parent = [string](Get-ObjectProperty $p 'parent' '')
            missing = [bool](Get-ObjectProperty $p 'missing' $false)
            inactive = [bool](Get-ObjectProperty $p 'inactive' $false)
            adjusted = [bool](Get-ObjectProperty $p 'adjusted' $false)
            sourceX = Get-RectField $source 'x'
            sourceY = Get-RectField $source 'y'
            sourceW = Get-RectField $source 'w'
            sourceH = Get-RectField $source 'h'
            sourceArea = Get-PanelArea $p 'source'
            finalX = Get-RectField $final 'x'
            finalY = Get-RectField $final 'y'
            finalW = Get-RectField $final 'w'
            finalH = Get-RectField $final 'h'
            finalArea = Get-PanelArea $p 'final'
            viewportX = Get-RectField $viewport 'x'
            viewportY = Get-RectField $viewport 'y'
            viewportW = Get-RectField $viewport 'w'
            viewportH = Get-RectField $viewport 'h'
            cropPct = [double](Get-ObjectProperty $p 'cropPct' 0.0)
            editorOverlapPct = [double](Get-ObjectProperty $p 'editorOverlapPct' 0.0)
            scaleX = Get-RectField $scale 'x'
            scaleY = Get-RectField $scale 'y'
            offsetX = Get-RectField $offset 'x'
            offsetY = Get-RectField $offset 'y'
        }
    }
    $rows | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
    Write-Host "  panel data -> $jsonPath"
    Write-Host "  panel csv  -> $csvPath"
}

function Compare-PanelBounds($before, $after, [string]$prefix, [string]$slug, [string[]]$names, [string]$runDir) {
    $beforeMap = Get-PanelMap $before
    $afterMap = Get-PanelMap $after
    $rows = @()
    Write-Host ""
    Write-Host "$prefix HUD viewport-lock comparison" -ForegroundColor Cyan
    foreach ($name in $names) {
        if (-not $beforeMap.ContainsKey($name) -or -not $afterMap.ContainsKey($name)) {
            Write-Host ("  {0}: missing descriptor in one state" -f $name) -ForegroundColor Yellow
            continue
        }
        $b = $beforeMap[$name]
        $a = $afterMap[$name]
        if ([bool](Get-ObjectProperty $b 'missing' $false) -or [bool](Get-ObjectProperty $a 'missing' $false)) {
            Write-Host ("  {0}: missing in one state" -f $name) -ForegroundColor Yellow
            continue
        }
        if ((Test-PanelInactive $b) -or (Test-PanelInactive $a)) {
            Write-Host ("  {0}: inactive in one state; skipped viewport-lock comparison" -f $name) -ForegroundColor DarkYellow
            continue
        }
        $bFinal = Get-ObjectProperty $b 'final' $null
        $aFinal = Get-ObjectProperty $a 'final' $null
        $bViewport = Get-ObjectProperty $b 'viewport' $null
        $aViewport = Get-ObjectProperty $a 'viewport' $null
        $norm = Get-NormalizedRect $bFinal $bViewport
        $expected = Convert-NormalizedToRect $norm $aViewport
        $err = Get-MaxRectError $aFinal $expected
        $dx = (Get-RectField $aFinal 'x') - (Get-RectField $bFinal 'x')
        $dy = (Get-RectField $aFinal 'y') - (Get-RectField $bFinal 'y')
        $dw = (Get-RectField $aFinal 'w') - (Get-RectField $bFinal 'w')
        $dh = (Get-RectField $aFinal 'h') - (Get-RectField $bFinal 'h')
        $areaDelta = (Get-PanelArea $a 'final') - (Get-PanelArea $b 'final')
        $rows += [pscustomobject]@{
            name = $name
            closedX = Get-RectField $bFinal 'x'
            closedY = Get-RectField $bFinal 'y'
            closedW = Get-RectField $bFinal 'w'
            closedH = Get-RectField $bFinal 'h'
            closedArea = Get-PanelArea $b 'final'
            editorX = Get-RectField $aFinal 'x'
            editorY = Get-RectField $aFinal 'y'
            editorW = Get-RectField $aFinal 'w'
            editorH = Get-RectField $aFinal 'h'
            editorArea = Get-PanelArea $a 'final'
            deltaX = $dx
            deltaY = $dy
            deltaW = $dw
            deltaH = $dh
            deltaArea = $areaDelta
            expectedX = if ($expected) { Get-RectField $expected 'x' } else { 0.0 }
            expectedY = if ($expected) { Get-RectField $expected 'y' } else { 0.0 }
            expectedW = if ($expected) { Get-RectField $expected 'w' } else { 0.0 }
            expectedH = if ($expected) { Get-RectField $expected 'h' } else { 0.0 }
            viewportLockErrorPx = $err
        }
        Write-Host ("  {0}: closed {1} area={2:n0} -> editor {3} area={4:n0}; delta x={5:n1} y={6:n1} w={7:n1} h={8:n1}; viewport-lock err={9:n1}px" -f `
            $name, (Format-PanelRect $bFinal), (Get-PanelArea $b 'final'), `
            (Format-PanelRect $aFinal), (Get-PanelArea $a 'final'), $dx, $dy, $dw, $dh, $err)
        Check "$prefix HUD $name remains locked to viewport-normalized position" ($err -le 6.0)
    }
    if ($rows.Count -gt 0) {
        $safeSlug = $slug -replace '[^a-zA-Z0-9_.-]', '-'
        $jsonPath = Join-Path $runDir ("hud-{0}-compare.json" -f $safeSlug)
        $csvPath = Join-Path $runDir ("hud-{0}-compare.csv" -f $safeSlug)
        $rows | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
        $rows | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
        Write-Host "  compare json -> $jsonPath"
        Write-Host "  compare csv  -> $csvPath"
    }
}

function Test-PanelSet($state, [string]$prefix, [string[]]$hudNames, [switch]$ExpectEditor) {
    $map = Get-PanelMap $state
    foreach ($name in $script:requiredPanelDescriptors) {
        Check "$prefix panel descriptor exists: $name" ($map.ContainsKey($name))
    }
    foreach ($name in $hudNames) {
        if (-not $map.ContainsKey($name)) { continue }
        $p = $map[$name]
        $missing = [bool](Get-ObjectProperty $p 'missing' $false)
        $isOptionalMissing = $missing -and (Test-OptionalMissingPanel $name)
        Check "$prefix HUD $name has rect or explicit optional missing" ((-not $missing) -or $isOptionalMissing)
        if ((-not $missing) -and (Test-PanelInactive $p)) {
            Check "$prefix HUD $name inactive zero-area is ignored" $true
            continue
        }
        if (-not $missing) {
            Check "$prefix HUD $name crop near zero" ([double](Get-ObjectProperty $p 'cropPct' 0.0) -le 1.0)
            Check "$prefix HUD $name editor overlap zero" ([double](Get-ObjectProperty $p 'editorOverlapPct' 0.0) -le 0.1)
        }
    }
    if ($ExpectEditor) {
        foreach ($name in @('camera-editor-panel','graph-editor-panel','camera-timeline-panel')) {
            if (-not $map.ContainsKey($name)) { continue }
            $p = $map[$name]
            if (-not [bool](Get-ObjectProperty $p 'missing' $false)) {
                $scale = Get-ObjectProperty $p 'scale' $null
                Check "$prefix editor panel $name remains unscaled" ([math]::Abs((Get-RectField $scale 'x') - 1.0) -le 0.001 -and [math]::Abs((Get-RectField $scale 'y') - 1.0) -le 0.001)
            }
        }
    }
}

function Test-WorldBlit($state, [string]$prefix, [bool]$expectBlit) {
    $d = Get-ObjectProperty $state 'dbg' $null
    Check "$prefix debug state present" ($null -ne $d)
    if ($expectBlit) {
        $bbW = [double](Get-ObjectProperty $d 'bbW' 0.0)
        $bbH = [double](Get-ObjectProperty $d 'bbH' 0.0)
        $vw = [double](Get-ObjectProperty $d 'vw' 0.0)
        $vh = [double](Get-ObjectProperty $d 'vh' 0.0)
        $blitRan = [bool](Get-ObjectProperty $d 'blitRan' $false)
        Check "$prefix render target size reported" ($d -and $bbW -gt 0 -and $bbH -gt 0)
        Check "$prefix world blit ran" ($d -and $blitRan)
        Check "$prefix scaled HUD/world preview requested" ($d -and [bool](Get-ObjectProperty $d 'scaleReq' $false))
        Check "$prefix preview rect valid" ($d -and [bool](Get-ObjectProperty $d 'previewValid' $false))
        if ($d -and $blitRan -and $vw -gt 0 -and $vh -gt 0) {
            $arVp = $vw / $vh
            $arBb = $bbW / $bbH
            Check "$prefix world blit preserves render-target aspect" ([math]::Abs($arVp - $arBb) -le 0.02)
        }
    }
}

function Wait-State($api, [scriptblock]$predicate, [int]$polls, [int]$sleepMs) {
    $last = $null
    for ($i = 0; $i -lt $polls; $i++) {
        Start-Sleep -Milliseconds $sleepMs
        $last = & $api['ReadState']
        if (& $predicate $last) { return $last }
    }
    return $last
}

function Has-PanelData($state) {
    $dbg = Get-ObjectProperty $state 'dbg' $null
    $panels = Get-ObjectProperty $dbg 'panels' $null
    return ($state -and $dbg -and $panels -and @($panels).Count -ge 10)
}

function Write-StateDiagnostic($state, [string]$prefix) {
    if (-not $state) {
        Write-Host "$prefix state: no editor state64 payload received" -ForegroundColor Yellow
        return
    }
    $panelCount = 0
    $dbg = Get-ObjectProperty $state 'dbg' $null
    $panels = Get-ObjectProperty $dbg 'panels' $null
    if ($panels) { $panelCount = @($panels).Count }
    $blitRan = $false
    $previewValid = $false
    if ($dbg) {
        $blitRan = [bool](Get-ObjectProperty $dbg 'blitRan' $false)
        $previewValid = [bool](Get-ObjectProperty $dbg 'previewValid' $false)
    }
    Write-Host ("$prefix state: enabled={0} debug={1} bottom={2} panels={3} blitRan={4} previewValid={5}" -f `
        [bool](Get-ObjectProperty $state 'enabled' $false), [bool](Get-ObjectProperty $state 'debug' $false), [string](Get-ObjectProperty $state 'bottomMode' ''), $panelCount, $blitRan, $previewValid)
    if ($panelCount -gt 0) {
        $names = @($panels | ForEach-Object { Get-ObjectProperty $_ 'name' '' }) -join ', '
        Write-Host "$prefix panels: $names"
    }
}

Write-Host '=== Camera editor viewport / HUD live verifier ===' -ForegroundColor Cyan
foreach ($res in $resolutions) {
    $tag = $res.name
    $runDir = Join-Path $OutDir $tag
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null

    if ($LaunchEachResolution) {
        Write-Host "Launching ${tag}: $($res.w)x$($res.h)" -ForegroundColor Cyan
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Width $res.w -Height $res.h -Cs2Dir $Cs2Dir -OutDir $runDir
        if ($LASTEXITCODE -ne 0) {
            throw "launch-cs2-netcon.ps1 failed for ${tag} with exit code $LASTEXITCODE"
        }
        Write-Host "[$tag] launch script returned."
    }

    $client = $null
    try {
        Write-Host "[$tag] Connecting to netcon 127.0.0.1:$Port (timeout ${NetconConnectTimeoutSeconds}s)..."
        $client = Connect-Netcon $Port $NetconConnectTimeoutSeconds
        Write-Host "[$tag] Netcon connected."
        $api = New-NetconApi $client
        & $api['Drain'] 0.5 | Out-Null

        if ([string]::IsNullOrWhiteSpace($Demo)) {
            Write-Host "[$tag] Loading filmmaker demo index: $DemoIndex"
            & $api['Send'] ("mirv_filmmaker watch " + $DemoIndex) 1.0 | Out-Null
        } else {
            Write-Host "[$tag] Loading demo path: $Demo"
            & $api['Send'] ("playdemo " + (Quote-ConsoleArg $Demo)) 1.0 | Out-Null
        }
        Start-Sleep -Seconds 5
        if ($SeekTick -ge 0) {
            Write-Host "[$tag] Seeking demo tick: $SeekTick"
            & $api['Send'] ("demo_gototick " + $SeekTick) 0.9 | Out-Null
            if ($SeekSettleSeconds -gt 0) { Start-Sleep -Seconds $SeekSettleSeconds }
        }
        & $api['Send'] 'demo_pause' 0.4 | Out-Null
        & $api['Send'] 'mirv_filmmaker editor debug on' 0.4 | Out-Null
        & $api['Send'] 'mirv_filmmaker editor off' 0.4 | Out-Null

        $closed = Wait-State $api { param($s) $s -and (-not $s.enabled) -and $s.debug -and (Has-PanelData $s) } $LoadPolls 1200
        Write-StateDiagnostic $closed "[$tag closed]"
        Check "[$tag] debug overlay publishes while editor closed" ($closed -and (-not $closed.enabled) -and $closed.debug -and (Has-PanelData $closed))
        Test-WorldBlit $closed "[$tag closed]" $false
        Test-PanelSet $closed "[$tag closed]" $trackedHudPanels
        Write-PanelBounds $closed "[$tag closed]" $trackedHudPanels
        Export-PanelSnapshot $closed $runDir '00-editor-closed-debug'
        Shot $runDir '00-editor-closed-debug'

        & $api['Send'] 'mirv_filmmaker editor on' 0.8 | Out-Null
        & $api['Send'] 'mirv_filmmaker editor scale on' 0.4 | Out-Null
        & $api['Send'] 'mirv_filmmaker editor hud game' 0.6 | Out-Null
        $open = Wait-State $api { param($s) $s -and $s.enabled -and $s.debug -and (Has-PanelData $s) -and $s.dbg.blitRan } $LoadPolls 700
        Write-StateDiagnostic $open "[$tag open]"
        Check "[$tag] editor opened" ($open -and $open.enabled)
        Test-WorldBlit $open "[$tag open]" $true
        Test-PanelSet $open "[$tag open]" $trackedHudPanels -ExpectEditor
        Write-PanelBounds $open "[$tag open]" $trackedHudPanels
        Export-PanelSnapshot $open $runDir '01-editor-open'
        Compare-PanelBounds $closed $open "[$tag closed->open]" 'closed-vs-open' $trackedHudPanels $runDir
        Shot $runDir '01-editor-open'

        & $api['Send'] 'mirv_filmmaker editor curveeditor timeline' 0.7 | Out-Null
        $timeline = Wait-State $api { param($s) $s -and $s.enabled -and ($s.bottomMode -eq 'camera') -and (Has-PanelData $s) -and $s.dbg.blitRan } 18 700
        Write-StateDiagnostic $timeline "[$tag timeline]"
        Check "[$tag] camera timeline mode active" ($timeline -and $timeline.bottomMode -eq 'camera')
        Test-WorldBlit $timeline "[$tag timeline]" $true
        Test-PanelSet $timeline "[$tag timeline]" $trackedHudPanels -ExpectEditor
        Write-PanelBounds $timeline "[$tag timeline]" $trackedHudPanels
        Export-PanelSnapshot $timeline $runDir '02-camera-timeline'
        Compare-PanelBounds $closed $timeline "[$tag closed->timeline]" 'closed-vs-timeline' $trackedHudPanels $runDir
        Compare-PanelBounds $open $timeline "[$tag open->timeline]" 'open-vs-timeline' $trackedHudPanels $runDir
        Shot $runDir '02-camera-timeline'

        & $api['Send'] 'mirv_filmmaker editor curveeditor graph' 0.7 | Out-Null
        $graph = Wait-State $api { param($s) $s -and $s.enabled -and ($s.bottomMode -eq 'graph') -and (Has-PanelData $s) -and $s.dbg.blitRan } 18 700
        Write-StateDiagnostic $graph "[$tag graph]"
        Check "[$tag] graph mode active" ($graph -and $graph.bottomMode -eq 'graph')
        Test-WorldBlit $graph "[$tag graph]" $true
        Test-PanelSet $graph "[$tag graph]" $trackedHudPanels -ExpectEditor
        Write-PanelBounds $graph "[$tag graph]" $trackedHudPanels
        Export-PanelSnapshot $graph $runDir '03-graph'
        Compare-PanelBounds $closed $graph "[$tag closed->graph]" 'closed-vs-graph' $trackedHudPanels $runDir
        Compare-PanelBounds $open $graph "[$tag open->graph]" 'open-vs-graph' $trackedHudPanels $runDir
        Compare-PanelBounds $timeline $graph "[$tag timeline->graph]" 'timeline-vs-graph' $trackedHudPanels $runDir
        Shot $runDir '03-graph'

        $gmap = Get-PanelMap $graph
        if ($gmap.ContainsKey('graph-editor-panel') -and -not [bool](Get-ObjectProperty $gmap['graph-editor-panel'] 'missing' $false)) {
            $gp = $gmap['graph-editor-panel']
            $gpFinal = Get-ObjectProperty $gp 'final' $null
            $gpScale = Get-ObjectProperty $gp 'scale' $null
            Check "[$tag graph] graph uses bottom editor area" ((Get-RectField $gpFinal 'w') -gt 100 -and (Get-RectField $gpFinal 'h') -gt 100)
            Check "[$tag graph] graph is not viewport-scaled" ([math]::Abs((Get-RectField $gpScale 'x') - 1.0) -le 0.001 -and [math]::Abs((Get-RectField $gpScale 'y') - 1.0) -le 0.001)
        }
    } finally {
        if ($client) { $client.Close() }
    }
}

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
$fail = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ("Artifacts: {0}" -f $OutDir)
if ($fail -eq 0) {
    Write-Host "ALL EDITOR-VIEWPORT LIVE CHECKS PASSED" -ForegroundColor Green
    if ($script:transcriptStarted) { try { Stop-Transcript | Out-Null } catch {} }
    exit 0
}
Write-Host "$fail CHECK(S) FAILED" -ForegroundColor Red
if ($script:transcriptStarted) { try { Stop-Transcript | Out-Null } catch {} }
exit 1




