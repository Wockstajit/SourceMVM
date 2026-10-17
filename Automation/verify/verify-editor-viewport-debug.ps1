param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot

# Static source verification for the camera-editor VIEWPORT/HUD work:
#   * opening the editor no longer hijacks the camera (no free-cam / no jump),
#   * the HUD scales into the world-blit rect (no insets, per-panel screen origin),
#   * the viewport/HUD debug overlay + its render-layer instrumentation exist end to end.
# Mirrors verify-editor-ui-follow-timeline.ps1 (text-pattern assertions, no CS2 launch).

function Require-Text {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Name
    )
    $full = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $full)) {
        throw "Missing file for $Name`: $Path"
    }
    $text = Get-Content -LiteralPath $full -Raw
    if ($text -notmatch $Pattern) {
        throw "FAILED $Name`: pattern not found: $Pattern"
    }
    "OK $Name"
}

function Require-Absent {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Pattern,
        [Parameter(Mandatory=$true)][string]$Name
    )
    $full = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $full)) {
        throw "Missing file for $Name`: $Path"
    }
    $text = Get-Content -LiteralPath $full -Raw
    if ($text -match $Pattern) {
        throw "FAILED $Name`: pattern SHOULD be absent but was found: $Pattern"
    }
    "OK $Name"
}

# --- Opening behavior: no auto free-cam, no auto-jump on open --------------------------------
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'cp\.SelectIndex\(0, /\*teleport\*/ false\)' 'editor open pre-selects a key WITHOUT teleport'
Require-Absent 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'cp\.SelectForEditor' 'editor open does NOT call SelectForEditor (no seek/jump)'
Require-Absent 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'CameraBridge_SetFreeCamEnabled' 'editor open does NOT force free cam on'
Require-Text   'AfxHookSource2\Filmmaker\FilmmakerCommand.cpp' 'bool FocusEditorCameraIfAny\(\)' 'timeline/graph focus uses a camera-only helper'
Require-Text   'AfxHookSource2\Filmmaker\FilmmakerCommand.cpp' 'tl\.SetVisible\(true\);\s*FocusEditorCameraIfAny\(\);' 'camera timeline open focuses an existing camera only'
Require-Absent 'AfxHookSource2\Filmmaker\FilmmakerCommand.cpp' 'CameraBridge_SetFreeCamEnabled' 'timeline/graph commands do NOT force free cam without a camera'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\GraphEditorExperimentHud.cpp' '!m_enabled \|\| !m_drive \|\| m_model\.TotalKeys\(\) == 0' 'graph editor drive is gated on real graph keys'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\GraphEditorExperimentHud.cpp' 'Do not force free cam here' 'graph editor enter documents no-freecam empty graph behavior'

# --- HUD scales with the world blit (allowlisted native HUD only) ----------------------------
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'var sx = x1 - x0, sy = y1 - y0;' 'HUD viewport uses the exact blit rect (no insets)'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'var px = \(c\.actualxoffset \|\| 0\) / rsx' 'HUD viewport reads each panel offset for screen-origin scaling'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'VIEWPORT_HUD_ROOTS' 'HUD viewport uses an explicit native-HUD allowlist'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "'HudWinPanel': 1" 'round win panel is included in HUD viewport scaling'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' '!isViewportHudRoot\(c\)' 'HUD viewport skips non-allowlisted roots'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "'GraphExpRoot': 1" 'graph editor root is explicitly excluded from HUD scaling'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "transformOrigin = '0% 0%'" 'HUD viewport uses Panorama-safe top-left transform origin'
Require-Text   'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "style\.transition = 'none'" 'HUD viewport snaps (no animated transition)'
Require-Absent 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'x0 \+ \(HUD_INSET_X / rw\)' 'old asymmetric HUD insets removed from the transform'

# --- Debug overlay: C++ state plumbing ------------------------------------------------------
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.h'  'void ToggleDebugOverlay\(\)' 'editor exposes debug-overlay toggle'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.h'  'bool m_debugOverlay' 'editor stores debug-overlay flag'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' '\\"debug\\":' 'editor state JSON publishes the debug flag'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'AfxViewportScaler::GetLastBlit' 'editor state JSON pulls the render-layer blit numbers'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' '\\"panels\\":' 'editor state JSON publishes measured panel rects'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'm_debugOverlay\)' 'debug overlay can build while editor is closed'
Require-Text 'AfxHookSource2\Filmmaker\FilmmakerCommand.cpp'         '0 == _stricmp\(arg, "debug"\)' 'console command: mirv_filmmaker editor debug'

# --- Debug overlay: render-layer instrumentation --------------------------------------------
Require-Text 'AfxHookSource2\ViewportScaler.h'           'bool GetLastBlit\(UINT& bbW' 'CViewportScaler exposes last-blit numbers'
Require-Text 'AfxHookSource2\ViewportScaler.h'           'bool m_LastBlitRan' 'CViewportScaler stores last-blit state'
Require-Text 'AfxHookSource2\ViewportScaler.cpp'         'm_LastBlitRan = true;' 'Blit records its actual viewport rect + backbuffer size'
Require-Text 'AfxHookSource2\RenderSystemDX11Hooks.cpp'  'bool GetLastBlit\(int& bbW' 'AfxViewportScaler bridge exposes GetLastBlit'

# --- Debug overlay: Panorama readout --------------------------------------------------------
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'function updateDebug\(st\)' 'editor JS builds the debug overlay'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'CamEditorDebugRoot' 'debug overlay uses a separate high-z root'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' '\$\.CreatePanel\(''Panel'', ctx, ''CamEditorDebugRoot''' 'debug overlay is a graph-proof sibling panel'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' "dbgPanel\.style\.zIndex = '150'" 'debug overlay sits above the graph editor root'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'CamEditorDebugRoot' 'debug overlay root is deleted during editor teardown'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'var dbgDescriptors = \[' 'debug overlay uses explicit HUD/panel descriptors'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'debugpanels' 'debug overlay publishes machine-readable panel rects'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'finalArea' 'debug overlay publishes per-panel pixel areas'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'cropPct' 'debug overlay reports per-panel crop percentage'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'editorOverlapPct' 'debug overlay reports per-panel editor overlap percentage'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'round-win-panel' 'debug overlay tracks round win panel bounds'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'round-win-container' 'debug overlay tracks visible round win child bounds'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'INACTIVE' 'debug overlay identifies zero-area inactive panels'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'trueview-active' 'debug overlay tracks TrueView / Active indicator'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'graph-editor-panel' 'debug overlay tracks graph editor panel'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'blit rect match' 'debug overlay labels expected-vs-actual as world-blit only'

# --- Live automation: standard resolution / mode / panel checks ------------------------------
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'LaunchEachResolution' 'live verifier can launch the standard automation resolution'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'w = 1600; h = 1200' 'live verifier covers standard 1600x1200 windowed automation'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'Test-PanelSet' 'live verifier checks every published HUD/editor panel rect'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'crop near zero' 'live verifier asserts HUD crop is near zero'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'editor overlap zero' 'live verifier asserts HUD/editor overlap is zero'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'round-win-panel' 'live verifier includes round win panel in HUD checks'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'round-win-container' 'live verifier includes visible round win child panels in HUD checks'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'inactive zero-area is ignored' 'live verifier treats inactive zero-area panels as inactive, not cropped'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'live HUD pixel bounds' 'live verifier prints per-HUD pixel bounds'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'Compare-PanelBounds \$closed \$open' 'live verifier compares closed/open HUD positions'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'viewport-normalized position' 'live verifier checks viewport-normalized HUD stability'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'Quote-ConsoleArg' 'live verifier quotes demo paths with spaces'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'dock timeline' 'live verifier switches to the TIMELINE dock'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'editor.timeline.view graph' 'live verifier switches the TIMELINE dock to its GRAPH view'
Require-Text 'Automation\verify\verify-editor-viewport-live.ps1' 'graph is not viewport-scaled' 'live verifier asserts graph remains independent from viewport scaling'

"PASS editor viewport / HUD 1:1 / debug-overlay static verification"




