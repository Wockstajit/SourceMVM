import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { test } from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
const require = createRequire(import.meta.url);
const { SAVE_EPOCH, EPOCH_FILE, parseEpoch, planReset, resetSavesIfNeeded } =
  require('../../SourceMovieMakerLauncher/electron/save-reset.cjs');

function hlaeFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-save-reset-'));
  for (const dir of ['source_mvm/props', 'source_mvm/environment/autosaves', 'bugreports', 'debuglogs', 'osu'])
    fs.mkdirSync(path.join(root, dir), { recursive: true });
  fs.writeFileSync(path.join(root, 'source_mvm/props/de_inferno.json'), '{}');
  fs.writeFileSync(path.join(root, 'filmmaker_settings.json'), '{}');
  fs.writeFileSync(path.join(root, 'bugreports/SourceMVM-bug-x.txt'), 'kept');
  return root;
}

test('epoch parsing tolerates missing or junk files', () => {
  assert.equal(parseEpoch(''), 0);
  assert.equal(parseEpoch('junk'), 0);
  assert.equal(parseEpoch('1\n'), 1);
  assert.deepEqual(planReset(SAVE_EPOCH), []);
  assert.ok(planReset(0).includes('source_mvm'));
});

test('an older machine is wiped once: saves go, diagnostics and content stay', async () => {
  const root = hlaeFixture();
  const first = await resetSavesIfNeeded(root);
  assert.equal(first.reset, true);
  assert.ok(first.removed.includes('source_mvm'));
  assert.ok(first.removed.includes('filmmaker_settings.json'));
  assert.equal(fs.existsSync(path.join(root, 'source_mvm')), false);
  assert.equal(fs.existsSync(path.join(root, 'filmmaker_settings.json')), false);
  assert.equal(fs.readFileSync(path.join(root, 'bugreports/SourceMVM-bug-x.txt'), 'utf8'), 'kept');
  assert.equal(fs.existsSync(path.join(root, 'debuglogs')), true);
  assert.equal(fs.existsSync(path.join(root, 'osu')), true);
  assert.equal(parseEpoch(fs.readFileSync(path.join(root, EPOCH_FILE), 'utf8')), SAVE_EPOCH);

  // New saves made after the reset survive every later start.
  fs.mkdirSync(path.join(root, 'source_mvm/props'), { recursive: true });
  fs.writeFileSync(path.join(root, 'source_mvm/props/de_dust2.json'), '{}');
  const second = await resetSavesIfNeeded(root);
  assert.equal(second.reset, false);
  assert.equal(fs.existsSync(path.join(root, 'source_mvm/props/de_dust2.json')), true);
  fs.rmSync(root, { recursive: true, force: true });
});

test('a fresh machine just records the epoch', async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-save-reset-'));
  const result = await resetSavesIfNeeded(root);
  assert.equal(result.reset, true);
  assert.deepEqual(result.removed, []);
  assert.equal(parseEpoch(fs.readFileSync(path.join(root, EPOCH_FILE), 'utf8')), SAVE_EPOCH);
  fs.rmSync(root, { recursive: true, force: true });
});

function archivedSidecar(root, { native = null, mapped = true } = {}) {
  const demo = path.join(root, 'match.dem');
  const folder = path.join(root, 'source_mvm', 'demos', 'cs2', 'match.dem');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(demo, 'demo unchanged');
  fs.writeFileSync(path.join(folder, 'demo.info'), Buffer.from([16, 123, 0, 255]));
  fs.writeFileSync(path.join(root, 'source_mvm', 'demos', 'index.json'), JSON.stringify({entries:
    mapped ? [{game: 'cs2', demoPath: demo, folder: 'match.dem'}] : []}));
  if (native !== null) fs.writeFileSync(demo + '.info', native);
  return demo;
}

test('reset restores relocated Valve metadata before deleting settings', async () => {
  const root = hlaeFixture();
  try {
    const demo = archivedSidecar(root);
    await resetSavesIfNeeded(root);
    assert.deepEqual(fs.readFileSync(demo + '.info'), Buffer.from([16, 123, 0, 255]));
    assert.equal(fs.readFileSync(demo, 'utf8'), 'demo unchanged');
    assert.equal(fs.existsSync(path.join(root, 'source_mvm')), false);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('reset preserves newer native metadata over an archived copy', async () => {
  const root = hlaeFixture();
  try {
    const demo = archivedSidecar(root, {native: 'new CS2 metadata'});
    await resetSavesIfNeeded(root);
    assert.equal(fs.readFileSync(demo + '.info', 'utf8'), 'new CS2 metadata');
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('reset keeps all saves when relocated metadata cannot be mapped', async () => {
  const root = hlaeFixture();
  try {
    archivedSidecar(root, {mapped: false});
    await assert.rejects(resetSavesIfNeeded(root), /saves have been kept/);
    assert.equal(fs.existsSync(path.join(root, 'filmmaker_settings.json')), true);
    assert.equal(fs.existsSync(path.join(root, EPOCH_FILE)), false);
    assert.equal(fs.existsSync(path.join(root, 'source_mvm/demos/cs2/match.dem/demo.info')), true);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});

test('reset keeps all saves on a native sidecar directory collision', async () => {
  const root = hlaeFixture();
  try {
    const demo = archivedSidecar(root);
    fs.mkdirSync(demo + '.info');
    await assert.rejects(resetSavesIfNeeded(root), /saves have been kept/);
    assert.equal(fs.existsSync(path.join(root, 'filmmaker_settings.json')), true);
    assert.equal(fs.existsSync(path.join(root, EPOCH_FILE)), false);
  } finally { fs.rmSync(root, {recursive: true, force: true}); }
});
