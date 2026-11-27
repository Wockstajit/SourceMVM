import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';

// The EVENT FX card (ConfigHudEventFxJs.h) and the Scene Objects Play box (ConfigHudOutlinerJs.h).
const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const card = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudEventFxJs.h');
const outliner = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudOutlinerJs.h');
const math = read('AfxHookSource2/Filmmaker/EventFx/EventFxMath.h');
const fx = read('AfxHookSource2/Filmmaker/EventFx/EventFx.cpp');
const props = read('AfxHookSource2/Filmmaker/Props/Props.cpp');

test('the card offers exactly the C++ effect catalog, in the same order', () => {
  const cpp = [...math.matchAll(/^\s*\{"([a-z_0-9]+)",\s+"[^"]+",\s+"[a-z]+",\s+\{"particles\//gm)].map((m) => m[1]);
  const block = /var EV_EFFECTS = \[([\s\S]*?)\];/.exec(card)[1];
  const js = [...block.matchAll(/\['([a-z_0-9]+)', T\('[^']+'\)\]/g)].map((m) => m[1]);
  assert.ok(cpp.length >= 25);
  assert.deepEqual(js, cpp);
});

test('the screen pulse is gone: effects spawn in the world instead', () => {
  assert.doesNotMatch(card, /exposureEv|Bake Keys|chromatic|vignette/);
  assert.doesNotMatch(fx, /RenderPost|BuildBakedExposureKeys|kPostShader/);
  assert.match(fx, /Props_SpawnEffect\(s\.plan\.system\.c_str\(\), s\.plan\.pos\)/);
  // Placed effects play on their event even with the presets off.
  assert.match(fx, /Props_FireTrigger\(EventFx::TriggerToken\(kind\)\);\n\s*if \(!g_masterEnabled\) return;/);
  // The precache rule: systems are loaded before they can fire.
  assert.match(fx, /Props_PrecacheEffect\(s\.c_str\(\)\)/);
});

test('a placed effect can wait for an event or a tick', () => {
  assert.match(outliner, /var olFxPlay = dropdownRow\(olFxBox, T\('Plays'\), OL_PLAYS/);
  assert.match(outliner, /olFxSet\('playtick', 'here'\)/);
  assert.match(outliner, /propsCommand\('fire ' \+ it\.id\)/);
  assert.match(props, /if \(Triggered\(p\) && !p\.playing\) \{ Props::PrecacheModel\(p\.model\.c_str\(\)\); return; \}/);
  assert.match(props, /\|\| \(Triggered\(p\) && !p\.playing\)\) continue;/);
  assert.match(props, /j\.StringField\("play", p\.play\);/);
});
