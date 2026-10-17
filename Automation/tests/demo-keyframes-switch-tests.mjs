// The demo is never reloaded onto a scrub keyframe copy by itself.
//
// Copies finish on every demo open and 5 s after every camera marker change. Switching onto each one
// automatically reloaded the demo under the user at the next pause, again and again ("why does the
// demo keep restarting", 2026-10-17). The switch is now only `mirv_filmmaker keyframes switch`.
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/' + rel, import.meta.url), 'utf8');
const filmmaker = read('Filmmaker.cpp');

test('a finished copy does not arm the switch', () => {
  const at = filmmaker.indexOf('DemoKeyframes_RunFrame(demo, ResolvePlayingDemoPath()');
  assert.ok(at > 0);
  const around = filmmaker.slice(at - 200, at + 400);
  assert.doesNotMatch(around, /g_copySwitch/);
  assert.doesNotMatch(filmmaker, /switching to them when the demo is paused/);
});

test('only the keyframes switch command arms it', () => {
  const arms = filmmaker.match(/g_copySwitch\.state = CopySwitch::State::Waiting;/g) || [];
  assert.equal(arms.length, 1);
  const fn = filmmaker.slice(filmmaker.indexOf('bool DemoKeyframes_SwitchNow()'), filmmaker.indexOf('void PumpCopySwitch()'));
  assert.match(fn, /g_copySwitch\.state = CopySwitch::State::Waiting;/);
  assert.match(read('FilmmakerCommand.cpp'), /_stricmp\(args->ArgV\(3\), "switch"\)\) Filmmaker::DemoKeyframes_SwitchNow\(\);/);
});
