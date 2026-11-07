// The MODULAR layout (Docs/panorama/modular-layout.md): a header bar along the top, CONFIG and the
// Camera Editor as movable, scalable windows over the full-screen game, both open at once.
//
// Static checks against the sources, so they run without CS2. The click and wheel rules
// themselves are pinned by the native UiClickMathTests.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (...p) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/' + p.join('/'), import.meta.url), 'utf8');

const win = read('Panorama', 'ModuleWindowJs.h');
const config = read('Panorama', 'ConfigHudJs.h');
const editor = read('Panorama', 'CameraEditorJs.h');
const header = read('Panorama', 'WorkspaceHeaderJs.h');
const headerHud = read('Panorama', 'WorkspaceHeaderHud.cpp');
const configHud = read('Panorama', 'ConfigHud.cpp');
const editorHud = read('Panorama', 'CameraEditorHud.cpp');
const filmmaker = read('Filmmaker.cpp');
const movie = read('Movie', 'MovieMode.cpp');

test('both columns are built in the window host, so one set of panels serves both layouts', () => {
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /#include "ModuleWindowJs\.h"/, name);
    assert.match(src, /var colHost = \w+Win\.host;/, name + ' takes its host from the window');
    for (const piece of ['mvmColumnGround', 'mvmActionRow'])
      assert.match(src, new RegExp(piece + '\\(colHost, '), name + ': ' + piece + ' goes in the host');
    assert.match(src, /var inspector = mk\('Panel', colHost\)/, name + ': the inspector goes in the host');
    assert.match(src, /root: colHost,/, name + ': the width edge goes in the host');
    // The scrollbar thumb and top fade are host children placed from a screen position.
    assert.match(src, /Win\.hostTop\(scrollView\)/, name + ' converts the scroll top into host px');
  }
});

test('a window scales as a whole with ui-scale, proportions locked', () => {
  // All three axes: one percentage alone is accepted by CS2 but lays the panel out with no height.
  assert.match(win, /win\.style\.uiScale = pct \+ ' ' \+ pct \+ ' 100%'/, 'Panorama ui-scale, so hit tests see the drawn size');
  // One factor for both axes, from any corner (sx / sy are -1 on the left / top) ...
  assert.match(win, /var k = drag\.k \+ \(c\.sx \* dx \/ designW \+ c\.sy \* dy \/ designH\) \/ 2;/, 'one factor for both axes');
  // ... and the opposite corner stays put.
  assert.match(win, /x = \(c\.sx < 0\) \? drag\.x \+ designW \* \(drag\.k - k\) : drag\.x;/);
  assert.match(win, /y = \(c\.sy < 0\) \? drag\.y \+ designH \* \(drag\.k - k\) : drag\.y;/);
  assert.match(win, /mkCorner\('left', 'top'\); mkCorner\('right', 'top'\); mkCorner\('left', 'bottom'\);/);
  // The stored rect is ignored until the console write hands our own value back (no snap on release).
  assert.match(win, /if \(pendingStore !== null\) \{/);
  assert.match(win, /pendingStore = v; pendingFrames = 0;/);
  assert.match(win, /'mirv_filmmaker settings ui win\.' \+ key \+ ' ' \+ v/, 'position and scale are saved');
  // Inside a window the column's width edge is hidden: the corner scale is the only resize.
  assert.match(config, /cfgGrip\.panel\.visible = !modularNow;/);
  assert.match(editor, /edGrip\.panel\.visible = !renderWorkspaceOpen && !st\.modular;/);
});

test('in the modular layout the panels publish no preview and neither closes the other', () => {
  assert.match(config, /if \(modularNow\) layoutModular\(hostRect\);/);
  assert.match(editor, /if \(modularNow\) \{\s*\/\/[^\n]*\n\s*hidePreviewLayout\(\);/);
  assert.match(configHud, /if \(m_enabled && CameraEditor_Active\(\) && !UiLayout_Modular\(\)\)/);
  assert.match(configHud, /if \(ConfigHudRef\(\)\.Enabled\(\) && CameraEditor_Active\(\) && !UiLayout_Modular\(\)\)/);
  assert.match(filmmaker, /if \(enabled && ConfigHud_Enabled\(\) && !UiLayout_Modular\(\)\)/);
  // No preview by design, so the "preview rect missing" warning stays quiet.
  assert.match(configHud, /if \(demo && m_enabled && !UiLayout_Modular\(\)\) \{/);
});

test('clicks and the wheel go to the window under the pointer', () => {
  for (const src of [config, editor]) assert.match(src, /Win\.publish\(root\);/);
  assert.match(configHud, /UiLayout_SetPanelRectString\(UiPanel::Config, /);
  assert.match(editorHud, /UiLayout_SetPanelRectString\(UiPanel::Editor, /);
  assert.match(movie, /in\.onPanel = UiLayout_PanelAt\(x, y\) != UiPanel::None/);
  assert.match(movie, /static void PushWorkspaceWheel\(int notches, int x, int y\)/);
  assert.match(filmmaker, /if \(!preview && UiLayout_Modular\(\) && \(ConfigHud_Enabled\(\) \|\| CameraEditor_Active\(\)\)\) \{/);
});

test('the close button reads the cursor pipe, and a title bar stays above the dock', () => {
  assert.match(win, /drag = \{ kind: 'close'/);
  assert.match(win, /if \(was\.kind === 'close'\) \{ if \(inRect\(rectOf\(close\), mx, my\) && opts\.onClose\) opts\.onClose\(\); \}/);
  assert.match(win, /var floorY = rh - Math\.max\(M, bottomClear \|\| 0\) - TITLE_H \* scale;/);
  assert.match(config, /bottomClear: mvmWinDockClear\(st\.dockTab, st\.dockH\)/);
  assert.match(editor, /bottomClear: mvmWinDockClear\(st\.bottomMode, st\.dockH\)/);
});

test('the TIMELINE dock shortens from its top edge, never past its design height', () => {
  const graph = read('Panorama', 'GraphEditorJs.h');
  const dockCpp = read('Movie', 'TimelineDock.cpp');
  const dockH = read('Movie', 'TimelineDock.h');
  assert.match(graph, /var DOCK_H_MAX = 440, DOCK_H_MIN = 220;/);
  assert.match(graph, /if \(dockGripPump\(\)\) prevLmb = !!st\.lmb;\s*else processInput\(\);/, "the strip outranks the dock's own pointer engine");
  assert.match(graph, /mirv_filmmaker settings ui timeline\.dockH /);
  assert.match(dockH, /constexpr int kTimelineDockBodyMax = 440;/);
  assert.match(dockCpp, /return tab == DockTab::Timeline \? kRibbonHeight \+ TimelineDock_BodyHeight\(\) : kHeightNative;/);
  assert.match(filmmaker, /TimelineDock_MainThreadFrame\(\);/);
});

test('scene objects: a left click on the game picks, with the Camera Editor window open too', () => {
  // The docked editor covers CONFIG; MODULAR windows sit side by side (owner's report, 2026-11-07).
  assert.match(movie, /in\.viewEdit = ConfigHud_Enabled\(\) && \(UiLayout_Modular\(\) \|\| !CameraEditor_Active\(\)\) && ConfigHud_ViewEditing\(\);/);
  assert.match(configHud, /"onPanel\\":" << \(\(UiLayout_Modular\(\) && MovieModeRef\(\)\.PointOnPanel\(cmx, cmy\)\)/);
  for (const name of ['ConfigHudPropsGizmoJs.h', 'ConfigHudEnvironmentGizmoJs.h']) {
    const g = read('Panorama', name);
    // The whole screen is the game's in MODULAR layout, and the picker steps aside over panels.
    assert.match(g, /clipW = winMode \? rw : rw - INSPECTOR_W;/, name);
    assert.match(g, /if \(rect\.length === 4 && !winMode\) \{/, name);
    assert.match(g, /Overlay\.hittest = editing && .*!\(winMode && s\.onPanel\);/, name);
  }
});

test('the TIMELINE dock hosted by the header bar owns the mouse and its own wheel', () => {
  // Review on #137: closing the last window cleared the hosted cursor flag, and wheel notches went
  // to a closed Camera Editor that never drained them.
  const body = filmmaker.slice(filmmaker.indexOf('bool GraphEditorExperiment_WantsCursor()'),
                               filmmaker.indexOf('// --- Fisheye lens (barrel'));
  assert.match(body, /if \(TimelineDock_Host\(\) == DockHost::Header\)\s*return true;/);
  assert.match(filmmaker, /void GraphEditorExperiment_PushWheel\(int notches\) \{ GraphEditorExperimentHudRef\(\)\.PushWheel\(notches\); \}/);
  const graphHud = read('Panorama', 'GraphEditorExperimentHud.cpp');
  assert.match(graphHud, /\$\.GraphExp && \$\.GraphExp\.wheel && \$\.GraphExp\.wheel\(%d\);/);
  for (const [name, src] of [['ConfigHud.cpp', configHud], ['CameraEditorHud.cpp', editorHud]])
    assert.match(src, /m_pendingWheel = 0; \} \/\/ nothing from before it opened/, name + ' drops notches from before it opened');
});

test('the header bar is torn down with its settings card and hosts the dock only when no window does', () => {
  assert.match(headerHud, /\$\('#MvmHeaderRoot'\)[\s\S]{0,80}\$\('#MvmHeaderSettingsRoot'\)[\s\S]{0,60}\$\.MvmHeader=null/);
  assert.match(header, /'MvmHeaderRoot'/);
  assert.match(header, /id: 'MvmHeaderSettingsRoot'/);
  assert.match(headerHud, /!ConfigHud_Enabled\(\) && !CameraEditor_Active\(\)\s*&& TimelineDock_Tab\(\) == DockTab::Timeline/);
  assert.match(filmmaker, /CameraEditorHudRef\(\)\.RunFrame\(\); \}[\s\S]{0,300}WorkspaceHeaderHudRef\(\)\.RunFrame\(\);/,
    'pumped after both windows, so it knows what they hold this frame');
});

test('the director card follows a layout switch made with CONFIG or the Camera Editor open', () => {
  for (const [name, src, cls] of [['ConfigHud.cpp', configHud, 'ConfigHud'], ['CameraEditorHud.cpp', editorHud, 'CameraEditorHud']]) {
    assert.match(src, new RegExp('void ' + cls + '::ReconcileMovieHud\\(\\) \\{[\\s\\S]{0,120}UiLayout_Modular\\(\\)'),
      name + ': the card is reconciled against the current layout');
    assert.match(src, /if \(m_enabled\) ReconcileMovieHud\(\);/, name + ': every frame while the panel is open');
    assert.doesNotMatch(src, /m_hidMovieHud = !UiLayout_Modular\(\);/, name + ': not latched once on enter');
  }
  assert.match(configHud, /const bool want = IsLive\(\) \|\| !UiLayout_Modular\(\);/, 'the live sidebar still hides the card');
  assert.match(editorHud, /const bool want = !UiLayout_Modular\(\) \|\| Render_IsModalOpen\(\);/,
    'the Render workspace frames a preview in either layout');
});

test('modular dock hit tests use the dock body, with no ribbon above it', () => {
  assert.match(movie, /static int DockBandHeight\(\) \{\s*if \(UiLayout_Modular\(\) && TimelineDock_Tab\(\) == DockTab::Timeline\) return TimelineDock_BodyHeight\(\);\s*return TimelineDock_Height\(\);/);
  assert.match(movie, /in\.onPanel = [\s\S]{0,300}DockBandHeight\(\) \/ 1080\.f/, 'a click just above the dock reaches the game');
  assert.match(movie, /bool MovieMode::PointerOverDock\(\) \{[\s\S]{0,500}DockBandHeight\(\) \/ 1080\.f/, 'and so does the cursor');
});
