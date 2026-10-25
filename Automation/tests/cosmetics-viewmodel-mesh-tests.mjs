// A legacy finish (AWP Dragon Lore, Gungnir) needs the gun's legacy mesh group on its first-person
// copy too. CS2 builds that copy anew on every draw, on the demo's own group, and only the one native
// switch mirrored ours onto it, so a gun drawn after a seek or loop landing showed the finish on the
// CS2 mesh in first person (measured 2026-10-24: world mask 2, HUD mask 1). Static checks against
// the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const system = read('AfxHookSource2/Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp');
const swap = read('AfxHookSource2/Filmmaker/Cosmetics/CosmeticModelSwap.cpp');

test('the first-person copy is mirrored each time the gun comes into first-person view', () => {
  assert.match(system, /if \(drawn && \(!meshState\.drawn \|\| viewModeChanged == 1\)\) \{\s*meshState\.viewmodelMirrorFrames = kViewmodelMirrorFrames;/);
  assert.match(system, /MirrorWeaponMeshToViewmodel\(ownerPawn, w, mask, i, meshState\.viewmodelWrites\)/);
  // Only the viewed player's gun: HUD copies are matched by model path.
  assert.match(system, /drawn = ph\.IsValid\(\) && ph\.GetEntryIndex\(\) == viewedPawnIndex;/);
});

// Chase or roaming to in-eye on the same player builds a new copy without a new draw, and the viewed
// pawn stays the same outside the eye, so a window must neither be spent before a copy exists nor
// miss the switch into the eye (review on #101).
test('a change of view mode re-arms the mirror, and only frames that found a copy count', () => {
  assert.match(system, /viewModeChanged = observerMode != m_lastMirrorObserverMode \? 1 : 0;/);
  assert.match(system, /if \(copies > 0 \|\| --meshState\.viewmodelWaitFrames <= 0\)\s*--meshState\.viewmodelMirrorFrames;/);
  assert.match(system, /meshState\.viewmodelWrites = ViewmodelMirrorWrites\{\};/);
});

// HUD weapons are pooled, so a recycled copy can read back the target mask while it still draws its
// previous group (ForceSetViewmodelMeshMask). Each copy gets one forced write per window; only after
// that may a matching read skip the write (review on #101).
test('every copy gets one forced write per window before its readback is trusted', () => {
  assert.match(swap, /return RefreshViewmodelWeapons\(pawn, nullptr, meshMask, worldWeapon, entityIndex, \/\*releaseComposite=\*\/false,\s*&written\);/);
  const skips = swap.match(/if \(meshMask && !\(written && written->Has\(\(uintptr_t\)\w+\)\s*&& ReadEntityMeshGroupMask\(.*?\) == meshMask\)\)/g) || [];
  assert.equal(skips.length, 2, 'both the scene-node walk and the HUD-entity fallback trust a readback only after a forced write');
  const records = swap.match(/if \(written\) written->Add\(\(uintptr_t\)\w+\);/g) || [];
  assert.equal(records.length, 2, 'both paths record the copy they wrote');
});
