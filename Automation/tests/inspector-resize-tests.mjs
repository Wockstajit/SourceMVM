// The right-hand inspector column is resizable, and everything that lays out against it follows.
//
// THE BUG CLASS THIS EXISTS FOR. Three separate Panorama panels need the column's width and they
// are not in one script scope: CONFIG draws the column, the CAMERA EDITOR draws its own and sizes
// the scaled preview to what is left, and the docked TIMELINE (regular / camera / graph) sizes its
// bar to what is left. Each used to carry its own `520` under a "keep in sync with ..." comment --
// a promise nothing checks, and the timeline's copy is the one that decides how wide the docked
// bar is. So the number now comes from C++ (InspectorWidth.h) and these tests assert that no
// panel has quietly grown a private copy again.
//
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const pan = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');

const widthH = pan('InspectorWidth.h');
const widthCpp = pan('InspectorWidth.cpp');
const grip = pan('InspectorResizeJs.h');
const config = pan('ConfigHudJs.h');
const editor = pan('CameraEditorJs.h');
const timeline = pan('CameraTimelineJs.h');

test('one C++ owner publishes the width to all three panels', () => {
  assert.match(widthH, /double InspectorWidth_Get\(\);/);
  assert.match(widthCpp, /FilmmakerSettings_UiString\("inspectorW"/,
    'the width must live in the persistent UI-preference store, not a private global');
  // A malformed stored value must not throw out of a per-frame state build.
  assert.match(widthCpp, /std::strtod/);
  assert.doesNotMatch(widthCpp, /std::stod\b/, 'stod throws; a corrupt stored width must fall back');

  for (const [name, src] of [
    ['ConfigHud.cpp', pan('ConfigHud.cpp')],
    ['CameraEditorHud.cpp', pan('CameraEditorHud.cpp')],
    ['CameraTimelineHud.cpp', pan('CameraTimelineHud.cpp')],
  ]) {
    assert.match(src, /InspectorWidth_AppendJson\(o\);/, `${name} must publish the width`);
  }
});

test('no panel keeps a private hard-coded column width', () => {
  // Each may still name a build-time fallback, but it must be used only until state arrives --
  // never as the value the layout is computed from.
  const cases = [
    ['ConfigHudJs.h', config, /st\.inspW/],
    ['CameraEditorJs.h', editor, /st\.inspW/],
    ['CameraTimelineJs.h', timeline, /st\.inspW/],
  ];
  for (const [name, src, re] of cases) {
    assert.match(src, re, `${name} must read the published width`);
  }
  // The "keep in sync by hand" comments are gone with the thing they were guarding.
  assert.doesNotMatch(timeline, /keep in sync with CameraEditorJs INSPECTOR_W/,
    'the timeline must not be hand-synced to the editor any more');
});

test('the three surfaces share one floor and one screen cap', () => {
  // C++ and both JS copies must agree, or the column, the preview and the docked bar disagree
  // about where the edge is on a screen too narrow for the stored width.
  assert.match(widthCpp, /kMin = 600\.0/);
  assert.match(widthCpp, /kMaxScreenFraction = 0\.62/);
  for (const [name, src] of [['CameraEditorJs.h', editor], ['CameraTimelineJs.h', timeline]]) {
    assert.match(src, /screenW \* 0\.62/, `${name}: screen cap must match InspectorWidth.h`);
    assert.match(src, /Math\.max\(600,/, `${name}: floor must match InspectorWidth.h`);
  }
  assert.match(grip, /MIN_W = 600/);
  assert.match(grip, /SCREEN_FRAC = 0\.62/);
});

test('the grip persists on release, not per frame', () => {
  // This HUD writes console output synchronously; a command per frame during a drag is the
  // console-spam stall the value tiles' feedback-loop guards exist to prevent.
  assert.match(grip, /if \(moved && opts\.commit\) opts\.commit\(liveW\);/);
  // The commit sits in the button-RELEASE branch, so it cannot fire while the drag is live.
  const heldBlock = grip.slice(grip.indexOf('if (held) {'), grip.indexOf('lastLmb = lmb;'));
  const releaseBlock = heldBlock.slice(heldBlock.indexOf('if (!lmb) {'));
  assert.match(releaseBlock, /opts\.commit\(liveW\)/, 'commit must happen on release');

  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /commit: function \(w\) \{ cmd\('mirv_filmmaker settings ui inspectorW ' \+ w\); \}/,
      `${name} must persist through the existing UI-preference command`);
  }
});

test('the grip only drags while the panel owns the cursor', () => {
  // In game-mouse mode the button is aiming the free cam, not dragging a column edge.
  assert.match(grip, /var lmb = !!st\.lmb && !!st\.cursor;/);
});

test('a resize re-applies every width-dependent style write', () => {
  // CONFIG bakes the width into four panels at build time; all four must be rewritten together,
  // or a resize leaves one of them at the old edge.
  const applyFn = config.slice(config.indexOf('function cfgApplyInspectorW(w)'));
  const body = applyFn.slice(0, applyFn.indexOf('\n    }'));
  for (const panel of ['inspector', 'footerBg', 'topFade', 'bottomFade']) {
    assert.match(body, new RegExp(`${panel}\\.style\\.width = w \\+ 'px';`),
      `${panel} must follow the column width`);
  }
  // The scroll viewport is a function of the column, so the offset clamp has to re-run.
  assert.match(body, /cfgApplyScroll\(\)/);
  // And the settings popup rises off the gear on that edge.
  assert.match(body, /settingsPopup\.setMarginRight/);
  assert.match(pan('EditorSettingsPopupJs.h'), /setMarginRight: function \(px\)/);
});

// ---------------------------------------------------------------------------------------------
// Reported after the first cut, in the game: "the render and the customize player button don't
// scale, and everything else only scales after you let go ... the preview window does, but not
// the timeline, camera, and the graph."
//
// Two distinct causes:
//   * the RENDER / CUSTOMIZE PLAYER row is built once from UI.col.w and nothing re-sized it;
//   * the docked timeline / camera / graph are SEPARATE Panorama panels reading their own state,
//     and the width only reaches state when it is persisted -- which happens on release. The
//     preview scaled live because it is drawn by the same panel that owns the drag.

test('the action row follows the column on both surfaces', () => {
  assert.match(config, /actionRow\.style\.width = Math\.floor\(w - UI\.action\.inset \* 2\) \+ 'px';/,
    'CONFIG: RENDER / CUSTOMIZE PLAYER must resize with the column');
  assert.match(editor, /actionRow\.style\.width = Math\.floor\(inspectorW - UI\.action\.inset \* 2\) \+ 'px';/,
    'CAMERA EDITOR: RENDER / CUSTOMIZE PLAYER must resize with the column');
});

test('the other panels are told the width DURING the drag, not on release', () => {
  // One publisher, in the fragment both draggable surfaces already include.
  assert.match(grip, /function mvmPublishInspW\(w\)/);
  // The real registered globals -- $.GraphEditor does not exist; the graph panel is $.GraphExp.
  assert.match(grip, /\$\.CamTimeline && \$\.CamTimeline\.setInspW/);
  assert.match(grip, /\$\.GraphExp && \$\.GraphExp\.setInspW/);
  assert.doesNotMatch(grip, /\$\.GraphEditor\b/, 'the graph panel registers as $.GraphExp');

  // Both grips call it from apply() -- the live path -- not only from commit().
  for (const [name, src] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.match(src, /mvmPublishInspW\(w\);/, `${name} must publish the live width`);
  }

  // And both receivers accept it.
  for (const [name, src] of [['CameraTimelineJs.h', timeline], ['GraphEditorJs.h', pan('GraphEditorJs.h')]]) {
    assert.match(src, /api\.setInspW = function \(w\)/, `${name} must accept a live width`);
  }
});

test('a live width outranks state, and hands back without a snap', () => {
  // Clearing the override when the command is SENT would show one frame of the old stored width.
  // It clears only once the published value has caught up.
  for (const [name, src, v] of [
    ['CameraTimelineJs.h', timeline, 'dockInspWOverride'],
    ['GraphEditorJs.h', pan('GraphEditorJs.h'), 'graphInspWOverride'],
  ]) {
    assert.match(src, new RegExp(`if \\(${v} > 0\\) \\{`), `${name}: the override must win`);
    assert.match(src, new RegExp(`Math\\.abs\\(st\\.inspW - ${v}\\) < 1\\) ${v} = 0;`),
      `${name}: the override must clear only once state has caught up`);
  }
});

test('the graph dock sizes off the shared width', () => {
  const graph = pan('GraphEditorJs.h');
  assert.match(graph, /scrW - graphInspectorW\(scrW\)/,
    'the graph dock must use the shared width, not a private INSPECTOR_W');
  assert.match(pan('GraphEditorExperimentHud.cpp'), /InspectorWidth_AppendJson\(o\);/);
  // Same floor and cap as everyone else.
  assert.match(graph, /screenW \* 0\.62/);
  assert.match(graph, /Math\.max\(600,/);
});
