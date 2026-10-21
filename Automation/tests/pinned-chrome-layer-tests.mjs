// The pinned chrome in both inspectors must keep its own stacking context.
//
// THE BUG THIS GUARDS. Scroll the CONFIG panel or the Camera Editor and the header chips and
// tab captions stopped painting: the layout boxes stayed exactly where they were (ESC EXIT kept
// its 96px box and lost only its label) while the panel background showed through where the
// text had been. It was measured, not guessed:
//
//   * not the wheel      -- it reproduced from JS with no mouse input at all;
//   * not a race         -- parking the column in the failing window left the chrome gone;
//   * not the scroll     -- `position`, `marginTop` and `transform` failed identically;
//   * not layout         -- Panorama reported every chrome panel visible, same x/y/w/h, opacity 1;
//   * not an overlay     -- hiding topFade, bottomFade, footerBg, scrollThumb, actionRow and
//                           pinnedFooter in turn at a failing offset changed nothing, but hiding
//                           the SCROLLED SUBTREE restored the chrome instantly.
//
// The failing offsets formed one contiguous window about the height of one card, i.e. exactly
// while a card straddled the viewport's top clip edge. The tell was what never broke: every
// panel carrying an explicit zIndex painted correctly in every bad frame, and only the chrome,
// which had none, was dropped. An explicit zIndex takes the chrome out of the clipped column's
// compositing context. It is a paint-order change only -- both rows sit above the scroll
// viewport in flow and never overlap it -- so removing it looks harmless and is not.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');

const config = read('ConfigHudJs.h');
const editor = read('CameraEditorJs.h');

// The overlays that already sat above everything, and must keep doing so.
const ABOVE = { topFade: 110, bottomFade: 110, footerBg: 100, scrollThumb: 105, actionRow: 120 };
const CHROME_Z = 80;

function zIndexOf(source, panel) {
  const m = new RegExp(panel + "\\.style\\.zIndex\\s*=\\s*'(\\d+)'").exec(source);
  return m ? Number(m[1]) : null;
}

test('the CONFIG panel pins its header and tab strip on their own layer', () => {
  assert.equal(zIndexOf(config, 'head'), CHROME_Z,
    'ConfigHudJs.h `head` needs an explicit zIndex or its chips stop painting while scrolling');
  assert.equal(zIndexOf(config, 'tabStrip'), CHROME_Z,
    'ConfigHudJs.h `tabStrip` needs an explicit zIndex or its tab captions stop painting');
});

test('the Camera Editor pins its header and tab strip the same way', () => {
  assert.equal(zIndexOf(editor, 'head'), CHROME_Z,
    'CameraEditorJs.h `head` needs an explicit zIndex; this inspector had the identical dropout');
  assert.equal(zIndexOf(editor, 'modeRow'), CHROME_Z,
    'CameraEditorJs.h `modeRow` needs an explicit zIndex');
});

test('the chrome layer stays under the floating overlays', () => {
  // The action row moved into the shared builder (WorkspaceChromeJs.h mvmActionRow), where the
  // panel is simply `row`; every other overlay is still declared in ConfigHudJs.h by name.
  const chrome = read('WorkspaceChromeJs.h');
  for (const [panel, z] of Object.entries(ABOVE)) {
    const found = panel === 'actionRow' ? zIndexOf(chrome, 'row') : zIndexOf(config, panel);
    assert.equal(found, z, panel + ' changed z-order; it must stay above the chrome');
    assert.ok(found > CHROME_Z, panel + ' must sit above the pinned chrome, not under it');
  }
});

test('neither inspector scrolls by a property that needs its own layer', () => {
  // Not the cause of the dropout, but the scrolled subtree owns clipped content next to pinned
  // text, so it stays on plain layout rather than being promoted every notch.
  assert.match(config, /scrollWrap\.style\.marginTop = \(-cfgScrollPx\) \+ 'px'/);
  assert.match(editor, /scrollWrap\.style\.marginTop = \(-edScrollPx\) \+ 'px'/);
  for (const [name, source] of [['ConfigHudJs.h', config], ['CameraEditorJs.h', editor]]) {
    assert.doesNotMatch(source, /scrollWrap\.style\.transform/,
      name + ' must not put the scrolled column on its own compositing layer');
  }
});

test('the diagnostic verbs used to find this are not left in the shipping panel', () => {
  for (const verb of ['dbgPaint', 'dbgHide', 'scrollMode', 'dbgChrome']) {
    assert.doesNotMatch(config, new RegExp('api\\.' + verb),
      'api.' + verb + ' was a temporary probe and must not ship');
  }
});

// THE OTHER LAYER BUG. Panorama compares z-index between siblings only, so a CONFIG tooltip or
// dropdown parented to ConfigHudRoot (z52) drew BEHIND the docked native demo bar (z58), however
// high its own zIndex was. Popovers live in a top-level ctx child above every HUD root instead.
test('CONFIG popovers draw above the docked demo bar', () => {
  const widgets = read('ConfigHudWidgetsJs.h');
  const timeline = read('CameraTimelineJs.h');
  const cpp = read('ConfigHud.cpp');
  const m = /\$\.CreatePanel\('Panel', ctx, 'ConfigHudOverlayRoot'[\s\S]*?o\.style\.zIndex = '(\d+)'/.exec(widgets);
  assert.ok(m, 'the overlay root must be a direct child of ctx');
  assert.ok(Number(m[1]) > 65, 'the overlay root must outrank every HUD root, the docked bar (58) included');
  assert.match(widgets, /tipPanel = mk\('Panel', cfgOverlay\(\)\)/, 'tooltips must live in the overlay root');
  assert.match(widgets, /var l = mk\('Panel', cfgOverlay\(\)\)/, 'the dropdown layer must live in the overlay root');
  assert.doesNotMatch(widgets, /tipPanel = mk\('Panel', root\)/);
  assert.match(config, /cfgOverlayPanel\.visible = on/, 'the overlay must hide with ConfigHudRoot');
  assert.match(config, /\$\('#ConfigHudOverlayRoot'\)/, 'a rebuild must drop the old overlay');
  assert.match(cpp, /\$\('#ConfigHudOverlayRoot'\)/, 'teardown must drop the overlay');
  assert.match(timeline, /'ConfigHudOverlayRoot': 1/, 'Hide All must not hide the overlay');
});
