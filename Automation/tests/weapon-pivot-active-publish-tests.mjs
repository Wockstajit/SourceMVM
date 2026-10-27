// Who tells the pivot stores which weapon is in shot, and how often.
//
// The pivot strips in the CONFIG panel (Weapon Recoil and Weapon Bob) follow the gun the
// spectated player is holding. That reading is a property of the GAME, not of either effect, so
// it has to be published from somewhere that runs unconditionally.
//
// It used to be published from inside WeaponBob::Advance, and only to the BOB store. Three ways
// that went stale, all of them reported as "the pivot isn't updating fast enough":
//
//   * the RECOIL store was never told at all, so it sat on its Rifle default forever;
//   * Advance returns early when Weapon Bob is switched off, so nothing updated;
//   * Advance also returns early on a fresh sample -- which a SPECTATOR SWITCH sets, i.e.
//     exactly the moment the weapon in shot changes.
//
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Movie/' + name, import.meta.url), 'utf8');

const stack = read('CameraShakeStack.cpp');
const bob = read('WeaponBob.cpp');
const cs2Host = read('CameraShakeHostSource2.cpp');

test('exactly one place publishes the active weapon', () => {
  // Across the whole feature there are two NoteActive calls, and they are the pair below.
  const callers = [stack, bob, read('WeaponRecoil.cpp')]
    .join('\n')
    .split('\n')
    .filter((l) => /\bNoteActive\(/.test(l) && !/^\s*(\/\/|\*)/.test(l));
  assert.equal(callers.length, 2,
    `expected exactly 2 NoteActive calls (one per store, one publisher); found:\n${callers.join('\n')}`);

  // Weapon Bob must not be one of them -- it returns early when it is off and on a fresh sample.
  assert.doesNotMatch(bob, /NoteActive\(/,
    'WeaponBob must not publish the active weapon: Advance returns early when bob is off and on a spectator switch');
});

test('the publisher runs unconditionally, in RunFrame', () => {
  const fn = stack.slice(stack.indexOf('void CameraShake_RunFrame()'));
  const body = fn.slice(0, fn.indexOf('\n}\n') + 3);

  // Both stores, from the one resolved class.
  assert.match(body, /WeaponPivotRecoilRef\(\)\.NoteActive\(c, live\);/,
    'the RECOIL pivot store must be published -- it was the one that never updated at all');
  assert.match(body, /WeaponPivotBobRef\(\)\.NoteActive\(c, live\);/);

  // Both must get the SAME class: they follow one gun, and a split would let the two strips
  // disagree about what is on screen.
  assert.match(body, /const WeaponPivotClass c = WeaponPivotClassOf\(live\);/);

  // A failed read holds the last good value instead of writing a guess -- the spectated POV
  // flickers while the CONFIG panel is open, and blinking the strip is worse than holding it.
  assert.match(body, /if \(CameraShake_SpectatedWeapon\(live\) && !live\.empty\(\)\)/);

  // No early RETURN may sit above the publish, or the strip goes stale again the way it did
  // inside WeaponBob::Advance.
  const beforePublish = body.slice(0, body.indexOf('WHICH WEAPON IS IN SHOT'));
  assert.doesNotMatch(beforePublish, /\breturn\b/,
    'nothing may return before the publish, or the strip goes stale again');

  // It IS gated, though: the read walks the entity system (spectated pawn -> active weapon handle
  // -> class name), which is only meaningful on a loaded map. The gate was DemoIsLoaded() and was widened to EntityContextLive() so the offline live-match
  // harness (mvm_test, a loaded map with no demo) publishes too -- both pivot strips froze there.
  // The stack asks its game host (CameraShakeHost.h); CS2's host answers with EntityContextLive().
  assert.match(body, /if \(ShakeHost_EntitiesLive\(\)\) \{/,
    'the publish must be gated on a live entity context -- it walks the entity system');
  assert.match(cs2Host, /bool ShakeHost_EntitiesLive\(\) \{ return EntityContextLive\(\); \}/,
    "CS2's host must answer the gate with EntityContextLive(), which covers the offline harness");
});

test('WeaponBob still resolves its own class for its own profile', () => {
  // Removing the publish must not have removed the local resolve -- the bob profile and hinge
  // lookup both key off it.
  assert.match(bob, /m_activeClass = WeaponPivotClassOf\(live\);/);
  assert.match(bob, /m_profiles\[\(int\)m_activeClass\]/);
});
