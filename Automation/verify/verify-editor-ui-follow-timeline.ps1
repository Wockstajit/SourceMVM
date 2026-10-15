param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot

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

Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' 'var INSPECTOR_W = 430, BOTTOM_H = 176, BOTTOM_LIFT = 0;' 'editor uses widened/flush layout constants'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' "bottomTab\('CAMERA', 'mirv_filmmaker editor curveeditor timeline'\)" 'bottom tab bar has Camera switch'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' "bottomTab\('GRAPH', 'mirv_filmmaker editor curveeditor graph'\)" 'bottom tab bar has Graph switch'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' "bottomTab\('REGULAR TIMELINE', 'mirv_filmmaker editor curveeditor native'\)" 'bottom tab bar has Regular Timeline switch'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.h' 'BottomMode m_bottomMode = BottomMode::Native;' 'camera editor defaults to the Regular (native CS2) timeline'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorHud.cpp' 'GraphEditorExperiment_Set\(false\);' 'camera editor does not auto-open graph on entry'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorFollowInspectorJs.h' "targetDrop = customDrop" 'follow target dropdown exists'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraEditorJs.h' "nearestNameLbl\.text[\s\S]*nearestCand\.name" 'select nearest row includes closest target name'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'if \(nativeMode\) nativeDock\(barW\); else nativeUndock\(\);' 'native CS2 timeline docks left of the inspector in Regular Timeline mode'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'function applyGameHudViewport\(active, rect\)' 'camera editor maps native CS2 HUD into preview rect'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'HUD_INSET_X = 14, HUD_INSET_TOP = 12' 'game HUD viewport has top/side safe inset'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "scale3d\(" 'game HUD viewport transform scales into preview'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "applyGameHudViewport\(\(hosted \|\| cfgOpen\) && !!st\.hudScale" 'game HUD viewport adjustment follows editor scale state'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' "CamCursorBtn[\s\S]*oldMouse\) oldMouse\.visible = false" 'hosted editor suppresses injected mouse button'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\CameraTimelineJs.h' 'setNativeHidden\(\(!!st\.open\) \|\| \(hosted && !!st\.graphExp\) \|\| previewHidden\)' 'native timeline shown only in Regular Timeline mode (hidden behind camera/graph overlays)'
Require-Text 'AfxHookSource2\Filmmaker\Panorama\GraphEditorJs.h' 'var BOTTOM_LIFT = 0;' 'graph dock is flush with the bottom'
Require-Text 'AfxHookSource2\CampathDrawer.h' 'FollowCameraMarker_set' 'drawer exposes independent follow marker'
Require-Text 'AfxHookSource2\CampathDrawer.cpp' 'D3DCOLOR_RGBA\(190, 80, 255' 'follow marker draws purple glow'
Require-Text 'AfxHookSource2\Filmmaker\Movie\FollowCamera.cpp' 'CameraBridge_SetFollowCameraMarker\(true' 'follow camera publishes marker while editor active'
Require-Text 'AfxHookSource2\Filmmaker\Movie\CameraBridge.h' 'CameraBridge_SetFollowCameraMarker' 'bridge declares follow marker function'

"PASS editor UI / follow camera / native timeline static verification"




