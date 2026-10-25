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
  assert.match(system, /if \(drawn && !meshState\.drawn\) meshState\.viewmodelMirrorFrames = kViewmodelMirrorFrames;/);
  assert.match(system, /MirrorWeaponMeshToViewmodel\(ownerPawn, w, mask, i\)/);
  // Only the viewed player's gun: HUD copies are matched by model path.
  assert.match(system, /drawn = ph\.IsValid\(\) && ph\.GetEntryIndex\(\) == viewedPawnIndex;/);
});

test('a repeated mirror never re-invalidates a copy that already shows the group', () => {
  assert.match(swap, /return RefreshViewmodelWeapons\(pawn, nullptr, meshMask, worldWeapon, entityIndex, \/\*releaseComposite=\*\/false,\s*\/\*onlyIfDiffers=\*\/true\);/);
  const writes = swap.match(/if \(meshMask && !\(onlyIfDiffers && ReadEntityMeshGroupMask\(.*?\) == meshMask\)\)/g) || [];
  assert.equal(writes.length, 2, 'both the scene-node walk and the HUD-entity fallback skip a copy already right');
});
