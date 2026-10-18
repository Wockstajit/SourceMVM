// The follow camera's target box ("which one did I just pick?") shows for a few seconds after a
// pick and then goes. It used to stay around the subject for the rest of the session once a target
// was chosen (user report 2026-10-17). Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Movie/' + name, import.meta.url), 'utf8');

test('the target box times out five seconds after the target changes', () => {
  const cam = read('FollowCamera.cpp');
  const body = cam.slice(cam.indexOf('void FollowCamera::PublishTargetHighlight()'), cam.indexOf('void FollowCamera::RunFrame('));
  assert.match(body, /constexpr unsigned long long kTargetHighlightMs = 5000;/);
  assert.match(body, /m_highlightSinceMs = now;/);
  assert.match(body, /now - m_highlightSinceMs >= kTargetHighlightMs\) \{\s*m_wantTargetHighlight = false;\s*CameraBridge_SetFollowTargetBox\(false/);
});

test('every pick shows the box again, even of the same target', () => {
  const discovery = read('FollowTargetDiscovery.cpp');
  const pick = discovery.slice(discovery.indexOf('bool FollowCamera::SelectCandidate('), discovery.indexOf('bool FollowCamera::SelectNearest('));
  assert.match(pick, /m_highlightTarget = -2;/);
});
