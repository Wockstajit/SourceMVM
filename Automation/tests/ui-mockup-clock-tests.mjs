// The mockup's fake demo clock, on its own.
//
//   node --test Automation/tests/ui-mockup-clock-tests.mjs
//
// MvmState.advance is the one piece of the browser mockup that is pure arithmetic, so it gets
// tested here rather than through a headless browser: no CS2, no Chrome, and the two bugs it
// encodes are both invisible in a single still frame.
//
//   1. `playing` is not one field in the engine. MovieHud publishes playing=true to mean a demo is
//      LOADED -- it sends paused=true in the same breath -- while CamTimeline publishes
//      playing=false to mean the clock is stopped. The mockup merges every root's feed into one
//      model, so MovieHud's meaning won and a demo captured paused ran anyway.
//   2. tickMin/tickMax are the CAMERA PATH's first and last key, not the demo's bounds. Wrapping
//      the clock on them yanked the playhead inside the path the moment the demo tick sat outside
//      it -- which is where it normally sits -- and the graph editor then read every channel value
//      at the wrong tick.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const SRC = path.join(ROOT, 'UiMockup', 'shim', 'state.js');

// state.js is a browser IIFE ending in `})(window)` -- it registers onto the window rather than
// exporting, so evaluate it with a stand-in window and take what it registers.
async function loadState() {
  const src = await readFile(SRC, 'utf8');
  const win = {};
  // state.js reads the scene-key model at load (MvmSceneCatalog / MvmSceneKeys), so those two
  // shims are evaluated first, in the order the page loads them.
  for (const dep of ['scene-catalog.js', 'scene-keys.js']) {
    const depSrc = await readFile(path.join(ROOT, 'UiMockup', 'shim', dep), 'utf8');
    new Function('window', depSrc)(win);
  }
  new Function('window', src)(win);
  assert.ok(win.MvmState, 'state.js did not register MvmState');
  return win.MvmState;
}

const DEMO = { demoStartTick: 0, demoEndTick: 138839, tickMin: 17660, tickMax: 17857 };

test('a paused demo does not advance, even when playing is true', async () => {
  const { advance } = await loadState();
  const st = { ...DEMO, tick: 42821, playing: true, paused: true, timescale: 1 };
  for (let i = 0; i < 60; i++) advance(st, 1 / 60);
  assert.equal(st.tick, 42821, 'the clock ran while the demo was paused');
});

test('a running demo advances at the tick rate', async () => {
  const { advance, TICK_RATE } = await loadState();
  const st = { ...DEMO, tick: 1000, playing: true, paused: false, timescale: 1 };
  advance(st, 1);
  assert.equal(st.tick, 1000 + TICK_RATE, 'one second should move the clock one tick rate');
});

test('playing:false stops the clock whatever paused says', async () => {
  const { advance } = await loadState();
  const st = { ...DEMO, tick: 5000, playing: false, timescale: 1 };
  advance(st, 1);
  assert.equal(st.tick, 5000);
});

test('the clock wraps on the demo bounds, not on the camera path', async () => {
  const { advance } = await loadState();
  // Sitting past the path's last key (17857) but well inside the demo. The playhead must stay
  // where it is; wrapping here is what dragged it back to 17660 and moved the graph readouts.
  const st = { ...DEMO, tick: 42821, playing: true, paused: false, timescale: 1 };
  advance(st, 1 / 60);
  assert.ok(st.tick > DEMO.tickMax,
    `clock wrapped into the camera path range (tick ${st.tick} <= tickMax ${DEMO.tickMax})`);

  // At the real end of the demo it does wrap, back to the demo's start.
  const end = { ...DEMO, tick: DEMO.demoEndTick + 5, playing: true, paused: false, timescale: 1 };
  advance(end, 1 / 60);
  assert.equal(end.tick, DEMO.demoStartTick, 'past the end of the demo it should wrap to the start');
});

test('the loop range still wraps when one is armed', async () => {
  const { advance } = await loadState();
  const st = { ...DEMO, tick: 43899, playing: true, paused: false, timescale: 1,
    loopOn: true, loopReady: true, loopIn: 41800, loopOut: 43900 };
  advance(st, 1 / 60);
  assert.equal(st.tick, 41800, 'an armed loop should send the clock back to its in point');
});
