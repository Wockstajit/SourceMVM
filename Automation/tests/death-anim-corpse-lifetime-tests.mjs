// Death Animations only touch a corpse's physics bodies while its ragdoll still exists, and decide
// that without touching the ragdoll (DeathAnimations.cpp, CarrierAlive).
//
// The settle watch (mvm_debug only) read the pelvis body every frame for 2.5 s after the clip let
// go, and checked the corpse only at the end. A ragdoll the game removed in between left a freed
// body whose vtable call ran whatever lived there: a crash at localize.dll from
// RagdollBodies::ReadBody, and heap corruption that crashed CS2 elsewhere a moment later
// (2026-10-24, ragdolls.dem). Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const deathAnims = read('AfxHookSource2/Filmmaker/Movie/DeathAnimations.cpp');

// One top-level function, from its signature to its closing brace (the file is CRLF or LF).
function body(name) {
  const start = deathAnims.search(new RegExp('\\n(?:bool|void) ' + name + '\\('));
  assert.ok(start >= 0, name + ' exists');
  const end = deathAnims.slice(start).search(/\r?\n\}\r?\n/);
  assert.ok(end > 0, name + ' ends');
  return deathAnims.slice(start, start + end);
}

test('a corpse is current only while its pawn and carrier are both still the ones it was taken from', () => {
  const alive = body('CarrierAlive');
  assert.match(alive, /EntityAt\(pawnIndex\) != pawn/);
  // The carrier is looked up by the slot it was acquired from, across the whole handle range (a
  // client-side ragdoll is a client-only entity above EntityAt's ceiling), never by reading it.
  assert.match(alive, /carrier == pawn \|\| \(carrierIndex >= 0 && EntityInSlot\(carrierIndex\) == carrier\)/);
  assert.match(read('AfxHookSource2/Filmmaker/Movie/RagdollBodies.cpp'), /out\.carrierIndex = EntityIndexOf\(carrier\);/);
  // The liveness check comes first, so a removed ragdoll is never read or called into.
  assert.match(body('Current'), /CarrierAlive\([^)]*\)\s*&& RagdollBodies::Current\(/);
});

test('the settle watch checks the corpse every frame, before it reads a body', () => {
  const pump = body('PumpSettles');
  const check = pump.indexOf('CarrierAlive(');
  const firstRead = pump.indexOf('ReadBody(');
  assert.ok(check > 0 && check < firstRead, 'CarrierAlive runs before the first ReadBody');
  assert.ok(check < pump.indexOf('kSettleSeconds'), 'and before the watch window branches');
});

test('the late-frame hand trace checks the whole corpse, not just the pawn', () => {
  const late = body('DeathAnimations_RunLateFrame');
  assert.match(late, /!Current\(c\)\) continue;/);
  assert.ok(late.indexOf('!Current(c)') < late.indexOf('ReadBody('));
});
