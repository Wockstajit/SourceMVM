// Gun only: the gun is the subject wherever it is, and it has a live view.
//
// The gun pass was the colour difference between Main and NoGun alone, so every part of the gun as
// bright as what sat behind it (grey sleeve and barrel highlights over grey smoke) fell under the
// tolerance and came out key green; and the live view refused Gun only altogether (2026-10-17).
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const shader = read('Shaders/afx_render_pass_ps_5_0.fxc');

test('the gun pass keeps the viewmodel depth band whatever its colour', () => {
  assert.match(shader, /bool ViewmodelBand\(float raw\)/);
  const gun = shader.slice(shader.indexOf('} else if (mask == 2) {'), shader.indexOf('} else if (mask == 4) {'));
  assert.match(gun, /subject = smoothstep\(diff\.x, diff\.y/);               // the muzzle flash still comes from the difference
  assert.match(gun, /if \(ViewmodelBand\(LoadRaw\(refDepthTex, px\)\)\) subject = 1\.0;/);
});

test('the live view shows Gun only from its band', () => {
  assert.match(shader, /else if \(mask == 4\) \{[\s\S]*?subject = ViewmodelBand\(LoadRaw\(depthTex, px\)\) \? 1\.0 : 0\.0;/);
  assert.match(read('AfxHookSource2/Filmmaker/Render/RenderPassPlan.h'), /GunBand = 4/);
  assert.match(read('AfxHookSource2/RenderPassCompositor.cpp'), /p\.mask == 1 \|\| p\.mask == 4/);
  assert.match(read('AfxHookSource2/RenderSystemDX11Hooks.cpp'),
    /OnlyWeapon\) params\.mask = \(int\)Filmmaker::PassMask::GunBand;/);
  const command = read('AfxHookSource2/Filmmaker/Render/RenderCommand.cpp');
  assert.doesNotMatch(command, /Gun only has no live view/);
  const panel = read('AfxHookSource2/Filmmaker/Panorama/CameraEditorRenderJs.h');
  assert.doesNotMatch(panel, /Gun only has no live view/);
  assert.doesNotMatch(panel, /if \(RP_ELEMENTS\[e\]\[0\] === 'gun'\) continue;/);
});
