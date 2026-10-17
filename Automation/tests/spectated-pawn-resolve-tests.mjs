// Who is being spectated, when the engine's observer target reads -1 (as it does in demos).
//
// Weapon recoil, footstep shake and the other weapon_fire consumers only act on the spectated
// player's own shots. The fallback used to demand that a pawn's render eye sit within 0.2 units of
// the game camera; while the player moved, the eye sat 4-5 units away, so recoil skipped about
// half of the shots ("not every time, super delayed sometimes"). The fallback now takes the
// nearest pawn eye within first-person range.
//
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const src = fs.readFileSync(
  new URL('../../AfxHookSource2/ClientEntitySystem.cpp', import.meta.url), 'utf8');

const body = (() => {
  const start = src.indexOf('int AfxGetSpectatedPawnIndex() {');
  assert.ok(start >= 0, 'AfxGetSpectatedPawnIndex is defined');
  return src.slice(start, src.indexOf('\nint AfxGetSpectatedPawnIndexHeld', start));
})();

test('the render-eye fallback takes the nearest pawn, not an exact match', () => {
  assert.match(body, /kFirstPersonEyeUnits\s*=\s*12\.0/);
  assert.match(body, /if \(d2 < best\) \{ best = d2; nearest = i; \}/);
});

test('no exact 0.2-unit eye match is left in the resolve', () => {
  assert.doesNotMatch(body, /0\.2\s*\*\s*0\.2|<\s*0\.2\b|0\.04\b/);
});
