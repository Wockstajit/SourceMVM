import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

const source = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/CameraEditorCustomizeJs.h', import.meta.url), 'utf8');

function functionSource(name) {
  const start = source.indexOf('function ' + name + '(');
  assert.ok(start >= 0, name + ' must exist');
  let index = source.indexOf('{', start), depth = 0;
  for (; index < source.length; index++) {
    if (source[index] === '{') depth++;
    if (source[index] === '}' && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error('Incomplete function ' + name);
}

const script = ['slotPickup', 'slotOwnerSteam', 'effectiveSlotDef',
  'shouldRoutePickupToOwner', 'pickupOwnerSteam', 'resetToDefault']
  .map(functionSource).join('\n') + '\nresetToDefault();';
const holder = '76561198000000001';
const ownerA = '76561198000000002';
const ownerB = '76561198000000003';

function weapon(defIndex, ownerSteamId = '', pickup = false) {
  return { defIndex, ownerSteamId, pickup };
}

function runReset(extra = {}) {
  const commands = [], uiCalls = [];
  const context = {
    custTargetKey: 'steam:' + holder,
    custLoadout: { primary: null, secondary: null },
    custActiveWeaponPickup: false,
    custActiveWeaponOwnerSteam: '',
    custActiveWeaponDef: 0,
    custActiveWeaponSlot: '',
    custTouched: { agent: true, primary: true, secondary: true, knife: true, gloves: true },
    custDirty: true,
    cmd: command => commands.push(command),
    closeAllDrops() { uiCalls.push('close'); },
    populateCustomize() { uiCalls.push('populate'); },
    updatePreview() { uiCalls.push('preview'); },
    updateActionBar() { uiCalls.push('actions'); },
    ...extra,
  };
  context.resetSelectionsFromLoadout = () => {
    // Reseeding can replace slot/active-weapon data. Reset must capture its
    // command targets before this UI work, rather than relying on those mirrors.
    context.custLoadout = { primary: null, secondary: null };
    context.custActiveWeaponOwnerSteam = '';
    uiCalls.push('reseed');
  };
  vm.runInNewContext(script, context);
  assert.equal(context.custDirty, false);
  assert.deepEqual(Object.keys(context.custTouched).sort(), ['agent', 'gloves', 'knife', 'primary', 'secondary']);
  assert.ok(Object.values(context.custTouched).every(value => value === false));
  assert.deepEqual(uiCalls, ['close', 'reseed', 'populate', 'preview', 'actions']);
  assert.ok(commands.every(command => !command.startsWith('mirv_filmmaker cosmetics player ')),
    'reset must discard staged picks without applying them');
  return commands;
}

function profiles() {
  const profile = label => ({
    weapons: { 1: label + '-deagle', 7: label + '-ak', 9: label + '-awp', 16: label + '-m4' },
    knife: { defIndex: 500, paintKit: 12 },
    gloves: { defIndex: 5030, paintKit: 10006 },
    agent: label + '-agent',
  });
  return { [holder]: profile('holder'), [ownerA]: profile('owner-a'), [ownerB]: profile('owner-b') };
}

function replay(initial, commands) {
  // Model the backend store operations independently of the UI routing code.
  // A weapon clear removes one definition; a player clear removes that profile.
  const result = structuredClone(initial);
  for (const command of commands) {
    const [prefix, category, action, target, defIndex, ...rest] = command.split(' ');
    assert.equal(prefix, 'mirv_filmmaker');
    assert.equal(category, 'cosmetics');
    assert.equal(rest.length, 0);
    if (action === 'enabled') {
      assert.equal(target, '1');
      assert.equal(defIndex, undefined);
      continue;
    }
    const owner = target === 'current' ? holder : target;
    if (action === 'clearPlayer') {
      assert.equal(defIndex, undefined);
      delete result[owner];
    } else {
      assert.equal(action, 'clearWeapon');
      assert.ok(Number(defIndex) > 0);
      if (result[owner]) delete result[owner].weapons[defIndex];
    }
  }
  return result;
}

function assertClears(commands, clears, target = holder) {
  assert.deepEqual(commands, [
    'mirv_filmmaker cosmetics enabled 1',
    ...clears.map(([owner, def]) => 'mirv_filmmaker cosmetics clearWeapon ' + owner + ' ' + def),
    'mirv_filmmaker cosmetics clearPlayer ' + target,
  ]);
  const initial = profiles(), expected = structuredClone(initial);
  delete expected[holder];
  for (const [owner, def] of clears) delete expected[owner].weapons[def];
  assert.deepEqual(replay(initial, commands), expected,
    'reset must preserve every unrelated original-owner weapon, knife, glove, and agent override');
}

test('reset clears both pickup definitions for one owner without wiping their profile', () => {
  assertClears(runReset({ custLoadout: {
    primary: weapon(7, ownerA, true), secondary: weapon(1, ownerA, true),
  } }), [[ownerA, 7], [ownerA, 1]]);
});

test('reset clears pickups belonging to different owners', () => {
  assertClears(runReset({ custLoadout: {
    primary: weapon(7, ownerA, true), secondary: weapon(1, ownerB, true),
  } }), [[ownerA, 7], [ownerB, 1]]);
});

test('reset deduplicates an identical owner and definition', () => {
  assertClears(runReset({ custLoadout: {
    primary: weapon(7, ownerA, true), secondary: weapon(7, ownerA, true),
  } }), [[ownerA, 7]]);
});

test('the same definition at different owners is not deduplicated', () => {
  assertClears(runReset({ custLoadout: {
    primary: weapon(7, ownerA, true), secondary: weapon(7, ownerB, true),
  } }), [[ownerA, 7], [ownerB, 7]]);
});

test('slot pickup routing persists while a knife is active', () => {
  assertClears(runReset({
    custLoadout: { primary: weapon(7, ownerA, true), secondary: weapon(1, holder) },
    custActiveWeaponDef: 42, custActiveWeaponSlot: 'knife',
  }), [[ownerA, 7]]);
});

test('legacy active pickup ownership routes a matching held definition', () => {
  assertClears(runReset({
    custLoadout: { primary: weapon(7), secondary: null },
    custActiveWeaponPickup: true, custActiveWeaponOwnerSteam: ownerA,
    custActiveWeaponDef: 7, custActiveWeaponSlot: 'primary',
  }), [[ownerA, 7]]);
});

test('legacy active ownership does not route a different definition or slot', () => {
  for (const active of [
    { custActiveWeaponDef: 16, custActiveWeaponSlot: 'primary' },
    { custActiveWeaponDef: 7, custActiveWeaponSlot: 'secondary' },
  ]) {
    assertClears(runReset({
      custLoadout: { primary: weapon(7), secondary: null },
      custActiveWeaponPickup: true, custActiveWeaponOwnerSteam: ownerA,
      ...active,
    }), []);
  }
});

test('reset clears only the holder for owned or missing slots', () => {
  for (const custLoadout of [
    { primary: null, secondary: null },
    { primary: weapon(7, holder), secondary: weapon(1, holder) },
    { primary: weapon(7, holder, true), secondary: null },
  ]) assertClears(runReset({ custLoadout }), []);
});

test('reset never invents an owner or definition for incomplete pickup data', () => {
  for (const primary of [weapon(7, '', true), weapon(0, ownerA, true),
    weapon(-1, ownerA, true), { pickup: true, ownerSteamId: ownerA }]) {
    assertClears(runReset({ custLoadout: { primary, secondary: null } }), []);
  }
  assertClears(runReset({
    custLoadout: { primary: weapon(7), secondary: null },
    custActiveWeaponPickup: true, custActiveWeaponDef: 7, custActiveWeaponSlot: 'primary',
  }), []);
});

test('a missing slot owner can use valid matching active ownership', () => {
  assertClears(runReset({
    custLoadout: { primary: weapon(7, '', true), secondary: null },
    custActiveWeaponPickup: true, custActiveWeaponOwnerSteam: ownerB,
    custActiveWeaponDef: 7, custActiveWeaponSlot: 'primary',
  }), [[ownerB, 7]]);
});

test('an unkeyed target keeps the current-player fallback and explicit pickup owner', () => {
  assertClears(runReset({
    custTargetKey: 'pawn:12',
    custLoadout: { primary: weapon(7, ownerA, true), secondary: null },
  }), [[ownerA, 7]], 'current');
});
