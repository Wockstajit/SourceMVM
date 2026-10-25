// The -1 / +1 buttons under IN and LAST FRAME (CameraEditorRenderJs.h, rsSetSpan) send the whole span
// as a Custom range. A step must never leave the demo: a last frame past its end has the job wait on
// frames the demo's end hold never plays (review on #103). Runs the page's own function on stand-ins,
// so it needs no CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const js = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/CameraEditorRenderJs.h', import.meta.url), 'utf8');

function nudger() {
  const start = js.indexOf('var rsSpanStart = 0');
  const fn = js.indexOf('function rsSetSpan(', start);
  const src = js.slice(start, js.indexOf('\n    }\n', fn) + 6);
  const sent = [];
  const api = new Function('cmd', src + `
    return {
      set: rsSetSpan,
      state: function (s, e, ds, de) { rsSpanStart = s; rsSpanEnd = e; rsDemoStart = ds; rsDemoEnd = de; }
    };`)((line) => sent.push(line));
  return { api, sent };
}

test('the state publishes the demo bounds the nudges clamp to', () => {
  assert.match(js, /rsDemoStart = typeof r\.demoStart === 'number' \? r\.demoStart : -1;/);
  assert.match(js, /rsDemoEnd = typeof r\.demoEnd === 'number' \? r\.demoEnd : -1;/);
});

test('a nudge at a demo edge does nothing, and inside the demo it moves one tick', () => {
  const { api, sent } = nudger();
  api.state(100, 5001, 100, 5000);   // the whole demo: In on its first tick, last frame on its last
  api.set(99, 5001);                 // IN -1
  api.set(100, 5002);                // LAST FRAME +1
  assert.deepEqual(sent, []);
  api.set(101, 5001);                // IN +1
  assert.deepEqual(sent, ['mirv_filmmaker render range custom 101 5001']);
  api.state(100, 4000, 100, 5000);
  api.set(100, 6000);                // past the end: stops on the demo's last tick
  assert.equal(sent.at(-1), 'mirv_filmmaker render range custom 100 5001');
});

test('without published bounds In still never goes below tick 0', () => {
  const { api, sent } = nudger();
  api.state(0, 64, -1, -1);
  api.set(-1, 64);
  assert.deepEqual(sent, []);
  api.set(0, 65);
  assert.deepEqual(sent, ['mirv_filmmaker render range custom 0 65']);
});
