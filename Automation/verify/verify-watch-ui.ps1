#requires -Version 5
<#
============================================================
  Watch UI - automated isolation / layout verifier
============================================================
  Drives the live Filmmaker Watch UI over the CS2 netcon REPL
  (mirv_filmmaker ui_eval) and asserts the acceptance criteria
  WITHOUT pixel-clicking: it reads real Panorama panel state
  (tab visibility classes, child counts, whether tiles/detail
  loaded their native layouts) and captures a screenshot of the
  full primary monitor at each step.

  This is the automated equivalent of the manual test flow:
    open Watch -> Downloaded -> screenshot -> click Your Matches
    -> screenshot -> back to Downloaded -> toggle repeatedly,
    asserting the two tabs stay isolated and the Downloaded
    layout is the native (styled) one each time.

  PREREQ: CS2 must already be running under this HLAE build with
  the netcon console enabled (-netconport 29010). Use
  Automation\verify\test-filmmaker.ps1 to launch, then:

    pwsh Automation\verify\verify-watch-ui.ps1
============================================================
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$OutDir,
    [double]$ReadSeconds = 1.2
)
$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$here = $automationRoot
. (Join-Path $automationRoot 'lib\AutomationCommon.ps1')
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = New-AutomationRunFolder -Name 'verify-watch-ui'
} else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}
Save-AutomationRunMetadata -RunDirectory $OutDir -AutomationName 'verify-watch-ui' -Additional @{ port = $Port } | Out-Null

# --- netcon plumbing (mirror of cs2-netcon.ps1, but returns the text) --------
$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if (-not $cs2) { Write-Host '[FAIL] cs2.exe is not running. Launch it with test-filmmaker.ps1 first.' -ForegroundColor Red; exit 1 }

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] Could not connect to netcon on 127.0.0.1:$Port. Add -netconport $Port to CS2 launch options." -ForegroundColor Red; exit 1 }
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { [void]$sb.Append($enc.GetString($buf, 0, $n)) }
        } else { Start-Sleep -Milliseconds 40 }
    }
    return $sb.ToString()
}
function Send([string]$cmd, [double]$seconds = $ReadSeconds) {
    $stream.Write(($enc.GetBytes($cmd + "`n")), 0, ($cmd.Length + 1)); $stream.Flush()
    return Drain $seconds
}

# The netcon console truncates any single command at 256 bytes, so a long
# inline ui_eval payload gets cut off mid-script and silently does nothing.
# Instead we drive small helpers baked into the injected UI ($.Filmmaker.*),
# which run with no length limit (see FilmmakerGuiJs.h "automation hooks").
$reportJs = '$.Filmmaker.report()'

function ClickTabJs([string]$which) {
    # which = 'YourMatches' | 'Downloaded'.
    # gotoTab performs the REAL native-equivalent navigation (toggles the
    # WatchMenu--Hide class + runs our Downloaded takeover). A synthetic
    # 'Activated' on the navbar RadioButton only highlights it; it does NOT
    # trigger the native mainmenu_watch.NavigateToTab, so the tab body never
    # actually switched -- that was the "only the button highlights" bug.
    if ($which -eq 'YourMatches') { return "`$.Filmmaker.gotoTab('yourmatches')" }
    return "`$.Filmmaker.gotoTab('downloaded')"
}

function Eval([string]$js, [double]$seconds = $ReadSeconds) {
    return Send ('mirv_filmmaker ui_eval "' + $js + '"') $seconds
}

# Reads a "[TAG] k=v k=v" line out of the console dump.
#
# Matches any tag that actually CARRIES key=value pairs: [VERIFY], [BTN], [ORG], [STAB],
# [NEWFOLDER], [ORGTOGGLE]. A [VERIFY]-only regex silently returned an empty table for the
# others, so every check reading one compared $null against '1' and reported a failure that had
# nothing to do with the UI.
#
# [GOTO] and [SEL] are deliberately EXCLUDED, and the payload group requires a leading
# key=value. Those two emit bare status words ("[GOTO] JsDownloaded", "[SEL] done") with no
# pairs at all, and because this takes the LAST match, one of them arriving late in the drain
# buffer would shadow the real answer and hand back an empty table -- turning every check that
# read it into a phantom failure.
#
# Last match wins so demo-probe chatter or a stale reply from an earlier command cannot shadow
# the answer we just asked for.
function Parse([string]$text) {
    # Not $matches: that is a PowerShell automatic variable populated by -match.
    $hits = [regex]::Matches($text, '\[(?:VERIFY|BTN|ORG|STAB|NEWFOLDER|ORGTOGGLE)\]\s+(\S+=\S*(?:\s+\S+=\S*)*)')
    $h = @{}
    if ($hits.Count -gt 0) {
        $last = $hits[$hits.Count - 1]
        foreach ($kv in ($last.Groups[1].Value -split '\s+')) {
            $p = $kv -split '=', 2
            if ($p.Count -eq 2) { $h[$p[0]] = $p[1] }
        }
    }
    return $h
}

function Shot([string]$name) {
    $out = Join-Path $OutDir $name
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'capture\capture-main-monitor.ps1') -Out $out 2>$null | Out-Null
    if (Test-Path $out) { Write-Host "      shot -> $out" -ForegroundColor DarkGray }
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$label, [bool]$cond) {
    if ($cond) { Write-Host "[ PASS ] $label" -ForegroundColor Green; $results.Add("PASS $label") }
    else { Write-Host "[ FAIL ] $label" -ForegroundColor Red; $results.Add("FAIL $label") }
}

Write-Host '=== Watch UI verifier ===' -ForegroundColor Cyan
Drain 0.5 | Out-Null

# 1) Open Watch -> Downloaded
Write-Host "`n--- Step 1: open Watch -> Downloaded ---" -ForegroundColor Cyan
Send 'mirv_filmmaker scan' 2.0 | Out-Null
Send 'mirv_filmmaker ui' 1.5 | Out-Null
Start-Sleep -Milliseconds 600
$dl1 = Parse (Eval $reportJs)
Shot '1_downloaded.png'
Check 'Downloaded tab is visible (not WatchMenu--Hide)' ($dl1['dlHide'] -eq '0')
Check 'Our Downloaded body exists' ($dl1['bodyExists'] -eq '1')
Check 'Downloaded has match tiles' ([int]($dl1['tiles']) -gt 0)
Check 'Tiles loaded native player.xml layout (styled, no overlap)' ($dl1['styledTile'] -eq '1')
Check 'Detail loaded native matchinfo.xml scoreboard' ($dl1['scoreboard'] -eq '1')

# 2) Click Your Matches -> must isolate
Write-Host "`n--- Step 2: click Your Matches (isolation) ---" -ForegroundColor Cyan
Eval (ClickTabJs 'YourMatches') 0.8 | Out-Null
Start-Sleep -Milliseconds 500
$ym = Parse (Eval $reportJs)
Shot '2_yourmatches.png'
Check 'Your Matches is visible' ($ym['ymHide'] -eq '0')
Check 'Downloaded is HIDDEN under Your Matches (no bleed)' ($ym['dlHide'] -eq '1')

# 3) Back to Downloaded -> layout intact again
Write-Host "`n--- Step 3: back to Downloaded ---" -ForegroundColor Cyan
Eval (ClickTabJs 'Downloaded') 0.8 | Out-Null
Start-Sleep -Milliseconds 500
$dl2 = Parse (Eval $reportJs)
Shot '3_downloaded_again.png'
Check 'Downloaded visible again' ($dl2['dlHide'] -eq '0')
Check 'Tiles still styled (no overlap on re-entry)' ($dl2['styledTile'] -eq '1')

# 4) Toggle several times -> must stay isolated every time
Write-Host "`n--- Step 4: rapid toggling (x4) ---" -ForegroundColor Cyan
$toggleOk = $true
for ($i = 1; $i -le 4; $i++) {
    Eval (ClickTabJs 'YourMatches') 0.5 | Out-Null; Start-Sleep -Milliseconds 300
    $a = Parse (Eval $reportJs 0.8)
    if ($a['dlHide'] -ne '1' -or $a['ymHide'] -ne '0') { $toggleOk = $false }
    Eval (ClickTabJs 'Downloaded') 0.5 | Out-Null; Start-Sleep -Milliseconds 300
    $b = Parse (Eval $reportJs 0.8)
    if ($b['dlHide'] -ne '0') { $toggleOk = $false }
}
Check 'Tabs stayed isolated across repeated toggling' $toggleOk

# 5) List stability: select a demo, tag the first row, induce a scan, wait 30s.
#    A stable list keeps the same tile count, the SAME first-row object (its tag
#    survives -> rows were not recreated -> scroll is preserved) and the same
#    checked row.
Write-Host "`n--- Step 5: left-list stability (30s under a scan) ---" -ForegroundColor Cyan
Eval (ClickTabJs 'Downloaded') 0.8 | Out-Null; Start-Sleep -Milliseconds 400
$selJs = "`$.Filmmaker.tagFirst(77)"
Eval $selJs 0.8 | Out-Null; Start-Sleep -Milliseconds 400
$stabJs = "`$.Filmmaker.stab()"
function StabParse([string]$txt) { $m=[regex]::Match($txt,'\[STAB\]\s+(.+)'); $h=@{}; if($m.Success){foreach($kv in ($m.Groups[1].Value -split '\s+')){$p=$kv -split '=',2; if($p.Count -eq 2){$h[$p[0]]=$p[1]}}}; return $h }
$base = StabParse (Eval $stabJs 0.8)
Shot '4_selected.png'
Send 'mirv_filmmaker scan' 1.0 | Out-Null   # induce churn (phase-2 data fill -> repeated render())
$stable = $true; $deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $now = StabParse (Eval $stabJs 0.8)
    # Compares selKey (the selected demo's stable folder|fileName identity), NOT the checked
    # panel's DOM index.
    #
    # DOM index was only ever a proxy for "which demo is selected", and it stopped being a
    # faithful one once the organiser began rendering tiles in SORT order while tilePanels
    # stayed indexed by position in `demos`. tagFirst() checks DOM row 1 with a synthetic
    # 'Activated', which (like the navbar case noted above) only highlights and does not run our
    # onactivate -- so the selection stays where it was, and the next render's setCheckedTile
    # correctly moves the checkmark onto the truly selected demo, which under a name sort is
    # some other row. That relocation used to be invisible because DOM order equalled `demos`
    # order. selKey asserts what the step actually cares about.
    if ($now['tiles'] -ne $base['tiles'] -or $now['tag'] -ne '77' -or $now['selKey'] -ne $base['selKey']) {
        $stable = $false
        Write-Host "      drift: $($now['tiles'])/$($now['tag'])/$($now['selKey']) vs $($base['tiles'])/77/$($base['selKey'])" -ForegroundColor Yellow
    }
}
Shot '5_after_30s.png'
Check 'Tile count unchanged over 30s' ($stable)
Check 'First row was NOT recreated (tag survived -> scroll preserved)' ($stable)
Check 'Selected demo stayed selected over 30s' ($stable)

# 6) Buttons: all present, visible, in the same flow row, with images passing
# hover/click through to the full 40x40 Button hitbox.
Write-Host "`n--- Step 6: demo toolbar buttons ---" -ForegroundColor Cyan
$btnJs = "`$.Filmmaker.btnReport()"
$btn = Parse (Eval $btnJs 0.8)
Shot '6_buttons.png'
Check 'Search button present + visible' ($btn['search'] -eq '1' -and $btn['searchVis'] -eq '1')
Check 'Refresh button present + visible' ($btn['ref'] -eq '1' -and $btn['refVis'] -eq '1')
Check 'Add button present + visible (list rail)' ($btn['add'] -eq '1' -and $btn['addVis'] -eq '1')
Check 'Navbar buttons share the flow row (cannot overlap)' ($btn['sameRow'] -eq '1')
Check 'Toolbar icons do not steal hover/click hit testing' ($btn['searchImgNoHit'] -eq '1' -and $btn['refImgNoHit'] -eq '1' -and $btn['addImgNoHit'] -eq '1')

# 7) Demo organiser rail: toolbar, sort control and folder headers.
Write-Host "`n--- Step 7: demo organiser rail ---" -ForegroundColor Cyan
$org = Parse (Eval "`$.Filmmaker.orgReport()" 0.8)
Shot '7_organizer.png'
Check 'Rail toolbar present' ($org['toolbar'] -eq '1')
Check 'Sort control present' ($org['sortBtn'] -eq '1')
Check 'Add control present in the rail' ($org['addBtn'] -eq '1')
Check 'A sort mode is active' (-not [string]::IsNullOrWhiteSpace($org['sort']))
Check 'Folder headers match the folder count' ($org['groups'] -eq '0' -or [int]$org['headers'] -eq ([int]$org['groups'] + 1))

$client.Close()

# --- Summary ---------------------------------------------------------------
# @(...) so a zero-match pipeline is an empty ARRAY, not $null. Under Set-StrictMode,
# $null.Count throws PropertyNotFoundException -- which meant a fully passing run exited 1
# with a confusing error, a path only reachable once every check actually passed.
$fail = @($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
Write-Host "Screenshots in: $OutDir"
if ($fail -eq 0) { Write-Host "`nALL CHECKS PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`n$fail CHECK(S) FAILED" -ForegroundColor Red; exit 1 }




