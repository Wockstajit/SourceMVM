// How the CONFIG panel's controls respond to the pointer: what the wheel does, where a tooltip
// lives, and which pivot scope you are allowed to edit.
//
// Four defects this guards, all reported from the game:
//
//   * THE WHEEL EDITED VALUES. A notch over a value tile changed that value ("wheel = 2% per
//     notch" in the handoff) instead of scrolling. You cannot move down a tab without crossing
//     tiles, so scrolling silently retuned whatever the pointer passed over. The wheel now only
//     ever scrolls; values change by dragging, which is deliberate and visible.
//   * TOOLTIPS ONLY ON THE NAME. The tip was attached to the label panel, so it appeared only
//     while the pointer was over the few characters of the name and not over the number, the
//     bar, or the rest of the tile.
//   * HOVER AND TIP FOUGHT OVER ONE EVENT. Panorama's SetPanelEvent REPLACES a handler, so
//     hoverable() and attachTip() on the same panel meant whichever ran last silently won.
//     Widening the tip to the whole tile is only safe once both go through one binding.
//   * PIVOT OFFERED "All" AND EVERY WEAPON CLASS. The All row was the default selection and was
//     invisible on any gun with an override, and the class cells let you edit a hinge for a
//     weapon that was not on screen. The strip now follows the gun in shot and locks the rest.
//
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');

const widgets = read('ConfigHudWidgetsJs.h');
const hud = read('ConfigHudJs.h');
const cameraTab = read('ConfigHudCameraTabJs.h');
const edCards = read('CameraEditorCardsJs.h');
const edHud = read('CameraEditorJs.h');

test('the wheel never edits a value -- on either surface', () => {
  // The nudge implementations are gone, not merely unreferenced: a dormant one would be
  // rewired by the next person who saw the call site.
  for (const [name, src] of [['ConfigHudWidgetsJs.h', widgets], ['CameraEditorCardsJs.h', edCards]]) {
    assert.doesNotMatch(src, /__nudge\s*=\s*function/, `${name}: a wheel-nudge implementation came back`);
  }
  assert.doesNotMatch(widgets, /function cfgWheelNudge/, 'cfgWheelNudge came back');
  assert.doesNotMatch(edCards, /function edWheelNudge/, 'edWheelNudge came back');

  // And nothing calls one before scrolling.
  assert.doesNotMatch(hud, /cfgWheelNudge\(/, 'the Config wheel still retargets to a tile');
  assert.doesNotMatch(edHud, /edWheelNudge\(/, 'the editor wheel still retargets to a tile');

  // The wheel still scrolls -- this must not have removed the scroll with the nudge.
  assert.match(hud, /cfgScrollY -= n \* 90;/);
});

test('hover paint and tooltip share one handler binding', () => {
  // Panorama's SetPanelEvent replaces rather than adds, so exactly one place may register the
  // mouseover/mouseout pair for a panel.
  assert.match(widgets, /function cfgBindHover\(p\)/);
  assert.match(widgets, /p\.__hoverBound/, 'the binding must be idempotent -- both callers reach it');
  assert.match(widgets, /if \(p\.__hoverPaint\) p\.__hoverPaint\(p\);/);
  assert.match(widgets, /if \(p\.__tipText\) tipShow\(p, p\.__tipText\);/);

  // Neither helper may register its own pair any more.
  const hoverable = widgets.slice(widgets.indexOf('function hoverable(p, paint)'),
    widgets.indexOf('function pressFlash'));
  assert.doesNotMatch(hoverable, /SetPanelEvent/,
    'hoverable must go through cfgBindHover, not register its own handlers');
  const attach = widgets.slice(widgets.indexOf('function attachTip(panel, text)'),
    widgets.indexOf('function attachTip(panel, text)') + 400);
  assert.doesNotMatch(attach, /SetPanelEvent/,
    'attachTip must go through cfgBindHover, not register its own handlers');
});

test('a tooltip covers its whole control, not just the name', () => {
  // Each builder attaches to the ROW/TILE it created, and keeps the label out of the hit path
  // so the row is always the hover target.
  const cases = [
    ['sliderRow', /nl\.hittest = false;\s*\n\s*if \(opts\.tip\) attachTip\(t, opts\.tip\);/],
    // A switch is a checkbox chip now: the tip hangs off the whole chip.
    ['togglePill', /tl\.hittest = false;[\s\S]{0,1500}if \(tipText\) attachTip\(c, tipText\);/],
    ['modeRow', /nl\.hittest = false;\s*\n\s*if \(tip\) attachTip\(r, tip\);/],
    ['optionGrid', /cap\.hittest = false;\s*\n\s*if \(tip\) attachTip\(r, tip\);/],
  ];
  for (const [what, re] of cases) {
    assert.match(widgets, re, `${what}: the tooltip must cover the whole control`);
  }
  // No builder may go back to hanging the tip on its label.
  assert.doesNotMatch(widgets, /attachTip\(nl,/, 'a tooltip is back on a name label');
  assert.doesNotMatch(widgets, /attachTip\(cap,/, 'a tooltip is back on a caption label');
  assert.doesNotMatch(widgets, /attachTip\(tl,/, 'a tooltip is back on a toggle label');
});

test('pivot has no All cell and follows the weapon in shot', () => {
  const scopes = cameraTab.match(/var PV_SCOPES = \[(.*?)\];/);
  assert.ok(scopes, 'PV_SCOPES not found');
  assert.doesNotMatch(scopes[1], /'all'/, "the pivot strip must not offer an All row");
  // The seven real classes are still there -- removing All must not have thinned the strip.
  for (const key of ['rifle', 'smg', 'pistol', 'shotgun', 'sniper', 'knife', 'utility']) {
    assert.match(scopes[1], new RegExp(`'${key}'`), `pivot scope ${key} went missing`);
  }

  // Only the live class is clickable, and the selection is derived from it rather than
  // remembered -- so switching weapon in the demo moves the strip.
  assert.match(cameraTab, /if \(key !== liveClass\) return;/,
    'the pivot strip must reject a scope that is not the weapon in shot');
  assert.match(cameraTab, /if \(liveClass\) scope = liveClass;/,
    'the pivot scope must follow the live weapon class');
  // Locked cells must read as unavailable.
  assert.match(cameraTab, /cell\.style\.opacity = \(PV_SCOPES\[j\]\[0\] === liveClass\) \? '1\.0' : '0\.35';/);

  // Both effects get this, because they share one builder.
  // The pivot is a drawer at the bottom of the card (rail handoff), not part of one page.
  assert.match(cameraTab, /pivotSection\(wrSec, 'recoil', 'pvr'/);
  assert.match(cameraTab, /pivotSection\([^)]*'bob', 'pvb'/);
});

test('a recoil preset can be saved without the keyboard', () => {
  // A field whose only commit is Enter reads as a field that does not work; the environment
  // preset field beside it has always had a Save button.
  assert.match(cameraTab, /function wrPresetSave\(\)/);
  assert.match(cameraTab, /button: \['Save', function \(\) \{\s*\n\s*if \(wrPresetFocus && wrPresetBuf\) wrPresetSave\(\); else wrPresetFocusOn\(\);/);
  // Enter and the button must commit through the SAME function, or they will drift.
  assert.match(cameraTab, /if \(code === 13\) \{\s*\n\s*wrPresetSave\(\);/);
  // Focusing blurs whatever else held the keyboard first.
  assert.match(cameraTab, /function wrPresetFocusOn\(\)[\s\S]{0,400}configWindowBlur\(\);/);
  // And the path is drivable from the console for testing.
  assert.match(hud, /api\.presetFocus = function \(\)/);
});
