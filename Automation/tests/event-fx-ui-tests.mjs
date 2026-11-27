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
  assert.match(outliner, /var olFxPlay = dropdownRow\(olFxCellA, T\('Plays'\), OL_PLAYS/);
  assert.match(outliner, /olFxSet\('playtick', 'here'\)/);
  assert.match(outliner, /propsCommand\('fire ' \+ it\.id\)/);
  assert.match(props, /if \(Triggered\(p\) && !p\.playing\) \{ Props::PrecacheModel\(p\.model\.c_str\(\)\); return; \}/);
  assert.match(props, /\|\| \(Triggered\(p\) && !p\.playing\)\) continue;/);
  assert.match(props, /j\.StringField\("play", p\.play\);/);
});

// The card lives on ENVIRONMENT beside Scene Objects, its presets are tiles two to a row, and every
// placed effect has an icon in the view that picks it, live or waiting for its event.
test('Event FX sits beside Scene Objects, and placed effects show an icon', () => {
  const widgets = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudWidgetsJs.h');
  const gizmo = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudPropsGizmoJs.h');
  assert.match(card, /section\(pageEnvironment, T\('EVENT FX'\), true, false, 'env\.eventfx'/);
  assert.doesNotMatch(card, /pageEffects/);
  assert.match(widgets, /\[T\('Scene'\), \['env\.scene', 'env\.eventfx', 'env\.heat'\]\]/);
  assert.match(card, /var cells = evCells\(evGrid, 2\);/);
  assert.match(props, /j\.StringField\("kind", "effect"\);/);
  assert.match(props, /if \(Props::IsEffect\(q\.model\)\) \{\n\s*\/\/ An effect is picked by its icon/);
  assert.match(props, /return p && \(Alive\(\*p\) \|\| Props::IsEffect\(p->model\)\) \? p : nullptr;/);
  assert.match(gizmo, /\} else if \(kind === 'effect'\) \{/);
});

// The shake jogs the frame only: a free camera reads the last view back every frame, so the shake's
// offset is taken out of that read, or each frame's shake became the next frame's pose.
test('the event shake never accumulates into a free camera', () => {
  const main = read('AfxHookSource2/main.cpp');
  const runtime = read('AfxHookSource2/Filmmaker/Movie/CameraRuntimeInternal.h');
  assert.match(main, /g_MirvInputEx\.ShakeAngles\[0\] = shook \? \(double\)Rx - preRx : 0\.0;/);
  assert.match(runtime, /rX = LastCameraAngles\[0\] - ShakeAngles\[0\];/);
});

// A paused demo freezes particles, so Test and Play now play the demo for the effect's length and
// go back to the tick they started from; Go to moves the camera to a preset's last place or an effect.
test('Test plays through a pause, and Go to finds the effect', () => {
  const header = read('AfxHookSource2/Filmmaker/EventFx/EventFx.h');
  assert.match(header, /void EventFx_PreviewWhilePaused\(double seconds\);/);
  assert.match(fx, /if \(g_preview\.active \|\| !fx::DemoIsPaused\(\)\) return;/);
  assert.match(fx, /DemoScrub_Release\(\(double\)g_preview\.tick, \/\*resume\*\/ false\);/);
  assert.match(fx, /EventFx_PreviewWhilePaused\(longest \+ p->delay\);/);
  assert.match(props, /EventFx_PreviewWhilePaused\(p->playLife > 0 \? p->playLife : 6\.0\);/);
  assert.match(card, /evCmd\('goto ' \+ p\.id\)/);
  assert.match(outliner, /propsCommand\('goto ' \+ it\.id\)/);
});

// Nothing plays until a template is picked from the dropdown; a pick places it at the crosshair
// (Where::Spot), with an icon, and Move here puts it where the crosshair points.
test('Event FX starts empty, and a picked template is placed where you aim', () => {
  assert.match(fx, /std::vector<EventFx::EventPreset> g_presets;\n/);
  assert.match(fx, /if \(0 == _stricmp\(sub, "addfrom"\) && argc >= 4\)/);
  assert.match(fx, /if \(aimed\) \{ p\.where = EventFx::Where::Spot; std::memcpy\(p\.spot, at, sizeof\(at\)\); \}/);
  assert.match(fx, /if \(p\.where == EventFx::Where::Spot\) std::memcpy\(at, p\.spot, sizeof\(at\)\);/);
  assert.doesNotMatch(fx, /sub, "place"\)/);
  assert.match(card, /var evAdd = dropdownRow\(evSec, T\('Add effect'\), \[\]/);
  assert.match(card, /evCmd\('set ' \+ p\.id \+ ' spot here'\)/);
  assert.match(props, /j\.IntField\("eventfx", s\.id\);/);
});
