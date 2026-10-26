// The three workspaces must draw their chrome from ONE definition each.
//
// CONFIG, the Camera Editor and Render each grew their own header chip, their own workspace-tab
// treatment and their own dock bar. They drifted into visibly different controls doing the same
// job: CONFIG's active MOUSE chip was a soft blue fill, the editor's a saturated gold one; the
// editor and Render put a filled box behind the ACTIVE workspace tab while CONFIG drew the strip
// flat; and CONFIG's dock bar was a dead LABEL where the editor had three working tabs.
//
// Docs/panorama/panorama_ui_guide.md maintains all three shared workspace contracts. The original
// designs gave headers identical markup and tabs the same `border-bottom:2px` rule with no fill,
// calling the dock "a single shared component imported by all three workspaces with an accent prop".
//
// These are static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');

const chrome = read('WorkspaceChromeJs.h');
const ribbon = read('TimelineDockRibbonJs.h');
const config = read('ConfigHudJs.h');
const editor = read('CameraEditorJs.h');
const editorWidgets = read('CameraEditorWidgetsJs.h');
const renderJs = read('CameraEditorRenderJs.h');

test('every header chip comes from the one shared builder', () => {
  assert.match(chrome, /function mvmChromeChip\(/);
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor],
                             ['CameraEditorRenderJs.h', renderJs]]) {
    assert.match(src, /mvmChromeChip\(/, name + ' must build its header chips from mvmChromeChip');
  }
  // Both hosts have to splice the fragment in, or the call is a ReferenceError at build time
  // and the whole panel script aborts half-built.
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /#include "WorkspaceChromeJs\.h"/, name + ' must include the fragment');
  }
});

test('a header chip carries no accent fill in any workspace', () => {
  // The label states the mode ("MOUSE: UI · G" / "MOUSE: GAME · G"), so a fill is a second
  // encoding of the same thing -- and it was the thing that made the two headers diverge.
  assert.doesNotMatch(config, /setBtnOn\(mouseBtn/, 'CONFIG must not tint its MOUSE chip');
  assert.doesNotMatch(editor, /btnSel\(mouseBtn/, 'the editor must not tint its MOUSE chip');
});

test('font-weight is a named weight, never a CSS number', () => {
  // Panorama throws "Invalid value for property 'font-weight'" on a number and the throw aborts
  // the panel build, leaving a half-built panel on screen rather than an error.
  for (const [name, src] of [['WorkspaceChromeJs.h', chrome], ['TimelineDockRibbonJs.h', ribbon]]) {
    assert.doesNotMatch(src, /fontWeight\s*=\s*'\d+'/, name + ' uses a numeric font-weight');
  }
});

test('workspace tabs are text on a rule, never a filled pill', () => {
  // Docs/panorama/panorama_ui_guide.md: workspace tabs are text on a rule, not filled pill buttons.
  assert.match(editorWidgets, /function tabify\(/);
  // Assert on the CODE, not on a string that also occurs in the prose explaining the fix.
  assert.match(editorWidgets, /b\.__bgSel = S\.clear; b\.__bgSelHover = S\.clear;/,
    'a selected workspace tab must not take a raised fill (this is what boxed the active tab)');
  // #afb9c6, not the original designs' #8b95a3: that measured APCA Lc 44 on #0c0e12 for 12px bold text.
  assert.match(editorWidgets, /b\.__fgBase = '#afb9c6ff'/,
    'an inactive tab is #afb9c6');
});

test('the dock ribbon is one component, driven by one console verb', () => {
  assert.match(ribbon, /function mvmTimelineDockRibbon\(/);
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /mvmTimelineDockRibbon\(/, name + ' must build the shared ribbon');
    assert.match(src, /#include "TimelineDockRibbonJs\.h"/, name + ' must include the fragment');
  }
  // One verb, so a tab clicked in either workspace moves the same shared state.
  assert.match(ribbon, /cmd\('mirv_filmmaker dock ' \+ key\)/);
  // The old per-panel bar is gone rather than merely unused.
  assert.doesNotMatch(editor, /function bottomTab\(/, "the editor's own tab-bar builder must be gone");
  assert.doesNotMatch(config, /REGULAR TIMELINE'/, 'CONFIG must not hard-code a dock label any more');
  // The gear belongs to the ribbon. A second hand-built gear once survived the swap and showed
  // up as a clipped half-glyph glued to the real one; only the ribbon may put a gear on the band.
  assert.doesNotMatch(config, /btn\(timelineBand, '⚙'/, 'CONFIG must not append its own gear to the ribbon');
  assert.match(config, /var settingsBtn = dockRibbon\.gear;/, "CONFIG's settings button must be the ribbon's gear");
  assert.equal((config.match(/var settingsBtn\b/g) || []).length, 1, 'settingsBtn must be declared exactly once');
});

test('the RENDER / CUSTOMIZE PLAYER row is one builder with CONFIG geometry', () => {
  const customize = read('CameraEditorCustomizeJs.h');
  assert.match(chrome, /function mvmActionRow\(/);
  // The row spans the column and the second action fills it -- CONFIG's geometry, the spec.
  assert.match(chrome, /row\.style\.width = \(UI\.col\.w - UI\.action\.inset \* 2\) \+ 'px'/);
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /mvmActionRow\(root, /, name + ' must build its action row from mvmActionRow');
    assert.match(src, /width: 'fill-parent-flow\(1\.0\)'/, name + ': CUSTOMIZE PLAYER must fill the row');
  }
  // The two hand-built copies are gone, not just bypassed.
  assert.doesNotMatch(config, /function workspaceAction\(/, "CONFIG's own row builder must be gone");
  assert.doesNotMatch(editor, /var renderBtnEl = mk\(/, "the editor's hand-built RENDER must be gone");
  assert.doesNotMatch(customize, /var customizeBtn = mk\('Panel', actionRow\)/,
    'CUSTOMIZE PLAYER must not be built separately in the Customize fragment any more');
});

test('both workspaces measure the docked editor through one helper', () => {
  // This is the regression guard: CONFIG assumed only the native bar sat under it, so once it
  // could dock the camera timeline its preview stopped shrinking and the ribbon sat under the bar.
  assert.match(ribbon, /function mvmDockMeasure\(/);
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /mvmDockMeasure\(ctx, rh, /, name + ' must size its bottom from mvmDockMeasure');
  }
  // CONFIG feeds it the SHARED tab, never a local assumption.
  assert.match(config, /mvmDockMeasure\(ctx, rh, sy, st && st\.dockTab/);
  // The editor's private copy of the measurement is gone.
  assert.doesNotMatch(editor, /var editorTop;\n\s+if \(bm === 'graph'\)/);
});

test('the workspace shell is one builder, and each host only supplies colour and wordmark', () => {
  const shell = read('WorkspaceShellJs.h');
  assert.match(shell, /function mvmWorkspaceShell\(/);
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /#include "WorkspaceShellJs\.h"/, name + ' must include the shell');
    assert.match(src, /var shell = mvmWorkspaceShell\(root, \{/, name + ' must build the shell');
    assert.match(src, /shell\.layout\(\{/, name + ' must place the shell through shell.layout');
    // The per-panel copies are gone, not bypassed: no host may build its own masks, catchers,
    // frame edges or decor any more.
    assert.doesNotMatch(src, /function (previewPart|workspaceMask|mkCatcher)\(/,
      name + ' still has its own shell builder');
    assert.doesNotMatch(src, /linemap\.webm/, name + ' paints its own line-map wash');
    assert.doesNotMatch(src, /var decorGhost = lbl\(/, name + ' builds its own decor');
  }
  // Only the shell knows the line-map; every host gets it washed in ITS accent.
  assert.match(shell, /linemap\.webm/);
  assert.match(shell, /washColor = a;/);
});

test('the dock mode is shared state, not a panel-private copy', () => {
  const header = fs.readFileSync(
    new URL('../../AfxHookSource2/Filmmaker/Movie/TimelineDock.h', import.meta.url), 'utf8');
  assert.match(header, /enum class DockTab/);
  assert.match(header, /enum class DockHost/);
  // The shared per-tab dock heights (Docs/features/camera-editor-feature.md) have to live with the mode that selects them.
  const impl = fs.readFileSync(
    new URL('../../AfxHookSource2/Filmmaker/Movie/TimelineDock.cpp', import.meta.url), 'utf8');
  // The numbers are public in the header (the CS:S hook lays out with them); the module uses them.
  for (const [name, h] of [['kTimelineDockHeightNative', '126'], ['kTimelineDockHeightTimeline', '476']]) {
    assert.match(header, new RegExp(name + ' = ' + h + ';'), 'dock height ' + h + ' is missing');
    assert.match(impl, new RegExp('= ' + name + ';'), name + ' is not what TimelineDock.cpp uses');
  }
  const editorHud = fs.readFileSync(
    new URL('../../AfxHookSource2/Filmmaker/Panorama/CameraEditorHud.h', import.meta.url), 'utf8');
  assert.doesNotMatch(editorHud, /BottomMode m_bottomMode/,
    'the Camera Editor must not keep its own copy of the dock mode');
});

test('the Render job has one Start/Cancel button and its refresh only reads live locals', () => {
  assert.equal((renderJs.match(/btn\w*\(jobActionRow/g) || []).length, 1);
  assert.doesNotMatch(renderJs, /btnDanger\(jobActionRow/, 'Cancel must be the same button, not a second one');
  assert.match(renderJs, /render cancel' : 'mirv_filmmaker render start'/);
  // curW/curH went with custom resolution; a stale read threw and stopped the whole job refresh.
  assert.doesNotMatch(renderJs, /\bcurW\b|\bcurH\b/);
});
