import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import vm from 'node:vm';

// Exercise shared packing with intervening content and different column counts, without a game.
function harness() {
  function panel(parent) {
    const p = { style: {}, children: [], GetChildCount() { return this.children.length; },
      GetChild(i) { return this.children[i]; } };
    if (parent) parent.children.push(p);
    return p;
  }
  const ctx = vm.createContext({ $: { CreatePanel: (_, parent) => panel(parent) } });
  for (const name of ['PanoramaUiMetricsJs.h', 'InspectorLayoutJs.h']) {
    const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');
    const match = /R"(\w+)\(([\s\S]*?)\)\1"/.exec(source);
    vm.runInContext(match[2], ctx);
  }
  return { ctx, panel, root: panel(null) };
}

test('odd tile counts keep equal reserved columns and consistent vertical alignment', () => {
  const { ctx, root } = harness();
  const slots = Array.from({ length: 5 }, () => ctx.mvmTileSlot(root));
  assert.equal(root.children.length, 3);
  for (const row of root.children) {
    assert.equal(row.children.length, 2);
    assert.equal(row.children[0].style.width, row.children[1].style.width);
    assert.equal(row.children[0].style.verticalAlign, 'top');
    assert.equal(row.children[1].style.marginRight, undefined);
  }
  assert.equal(slots[4], root.children[2].children[0]);
  assert.equal(root.children[2].children[1].children.length, 0);
});

test('a note or an action group ends an unfinished tile row', () => {
  const { ctx, root, panel } = harness();
  ctx.mvmTileSlot(root);
  const note = panel(root);
  ctx.mvmTileSlot(root);
  assert.equal(root.children.length, 3);
  assert.equal(root.children[1], note);
  const group = ctx.mvmActionGroup(root);
  ctx.mvmTileSlot(root);
  assert.equal(root.children.length, 5);
  assert.equal(root.children[3], group);
});

test('packing uses the configured column count and tile height', () => {
  const { ctx, root } = harness();
  ctx.UI.tile.perRow = 3;
  for (let i = 0; i < 4; i++) {
    const slot = ctx.mvmTileSlot(root);
    const tile = ctx.mvmTileBox(slot);
    assert.equal(slot.style.height, ctx.UI.tile.h + 'px');
    assert.equal(tile.style.height, '100%');
  }
  assert.deepEqual(root.children.map(p => p.children.length), [3, 3]);
});

test('shared color math round-trips picker colors and preserves hue for grays', () => {
  const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ColorMathJs.h', import.meta.url), 'utf8');
  const match = /R"(\w+)\(([\s\S]*?)\)\1"/.exec(source);
  const ctx = vm.createContext({ clamp01: value => Math.max(0, Math.min(1, value)), Math });
  vm.runInContext(match[2], ctx);
  for (const rgb of [[255, 0, 0], [64, 128, 192], [255, 200, 150], [0, 0, 0]]) {
    const hsv = ctx.mvmRgbToHsv(rgb, 0.73);
    assert.deepEqual(Array.from(ctx.mvmHsvToRgb(...hsv)), rgb);
  }
  assert.equal(ctx.mvmRgbToHsv([128, 128, 128], 0.73)[0], 0.73);
});

test('the colour picker opens inline and is driven off the cursor pipe', () => {
  const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ConfigColorPickerJs.h', import.meta.url), 'utf8');
  // Inline, under its own control: the host is created in the caller's flow, not on the root.
  assert.match(source, /function colorPicker\(parent, titleText, rgbRef, onChange, scale\) \{\s*\n\s*endTileLine\(parent\);\s*\n\s*var host = mk\('Panel', parent\);/);
  assert.doesNotMatch(source, /\$\.CreatePanel\('Panel', root,/, 'the picker must not float on the root any more');
  // The wheel is native panels (no conic gradient in Panorama): rotated spokes, darkened by V.
  assert.match(source, /sp\.style\.transform = 'rotateZ\('/);
  assert.match(source, /u\.dark\.style\.opacity = \(1 - v\)/);
  // Dragging reads the OS-cursor pipe and maps the wheel to hue (angle) and saturation (radius).
  assert.match(source, /function cfgColorPump\(st\)/);
  assert.match(source, /Math\.atan2\(dx, -dy\)/);
  // One open at a time; Reset goes back to the colour it opened with.
  assert.match(source, /cfgCloseColor\(\);\s*\n\s*if \(!owner\.ui\) owner\.ui = cfgColorBuild\(owner, host\);/);
  assert.match(source, /owner\.hsv = mvmRgbToHsv\(owner\.orig, owner\.hsv\[0\]\);/);
});

test('object color picker survives its first render identity sync', () => {
  const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/ConfigColorPickerJs.h', import.meta.url), 'utf8');
  assert.match(source, /if \(!hasIdentity\) \{ identity = data\.id; hasIdentity = true; \}/);
  assert.match(source, /else if \(identity !== data\.id\) \{ picker\.hide\(\); identity = data\.id; \}/);
});
