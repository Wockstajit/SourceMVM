import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { test } from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';

// CS2 writes its video, sound and bind settings into the mounted FX folder. Installing a new FX
// pack (complete installer or downloaded payload) must keep them, and a failed install must leave
// the old folder as it was.
const payloadSource = new URL('../../SourceMovieMakerLauncher/electron/runtime-payload.cjs', import.meta.url);
const launcherRequire = createRequire(payloadSource);
const { FX_SAVED_SETTINGS, isCs2Setting, replaceDirectory } = launcherRequire('./payload-swap.cjs');
const payloadScript = fs.readFileSync(payloadSource, 'utf8');
const releasePayloadPs1 = fs.readFileSync(
  new URL('../../SourceMovieMakerLauncher/scripts/build-release-payload.ps1', import.meta.url), 'utf8');

function writeTree(root, files) {
  for (const [relative, text] of Object.entries(files)) {
    const file = path.join(root, relative);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text);
  }
}

function readTree(root) {
  if (!fs.existsSync(root)) return null;
  const out = {};
  for (const relative of fs.readdirSync(root, { recursive: true })) {
    const file = path.join(root, relative);
    if (fs.statSync(file).isFile()) out[relative.split(path.sep).join('/')] = fs.readFileSync(file, 'utf8');
  }
  return out;
}

function tempRoot() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-payload-swap-'));
}

// What CS2 left in an installed FX folder, next to the pack's own files.
const PLAYER_SETTINGS = {
  'source_mvm_fx/cfg/cs2_video.txt': 'player video',
  'source_mvm_fx/cfg/cs2_video.txt.bak': 'player video backup',
  'source_mvm_fx/cfg/cs2_machine_convars.vcfg': 'player volume and snd_*',
  'source_mvm_fx/cfg/cs2_user_convars_0_slot0.vcfg': 'player convars',
  'source_mvm_fx/cfg/cs2_user_convars_0_slot0.vcfg_lastclouded': 'player convars cloud',
  'source_mvm_fx/cfg/cs2_user_keys_0_slot0.vcfg': 'player binds',
  'source_mvm_fx/ui_override/ServerConfig.vdf': 'player bot settings',
};
const OLD_PACK = {
  'source_mvm_fx/gameinfo.gi': 'old gameinfo',
  'source_mvm_fx/source_mvm_fx.vpk': 'old pack',
  'source_mvm_fx/old-only.txt': 'gone after the update',
  'source_mvm_fx/ui_override/panorama/videos/intro.webm': 'old intro',
};
const NEW_PACK = {
  'source_mvm_fx/gameinfo.gi': 'new gameinfo',
  'source_mvm_fx/source_mvm_fx.vpk': 'new pack',
  'source_mvm_fx/ui_override/panorama/videos/intro.webm': 'new intro',
};
const UPDATED = { ...NEW_PACK, ...PLAYER_SETTINGS };
const INSTALLED = { ...OLD_PACK, ...PLAYER_SETTINGS };

function moveIn(from) {
  return async (target) => { fs.renameSync(from, target); };
}

test('the kept paths are the folder CS2 writes its settings into', () => {
  assert.deepEqual(FX_SAVED_SETTINGS.map((relative) => relative.split(path.sep).join('/')),
    ['source_mvm_fx/cfg', 'source_mvm_fx/ui_override/ServerConfig.vdf']);
  for (const name of ['cs2_video.txt', 'cs2_video.txt.bak', 'cs2_machine_convars.vcfg',
    'cs2_user_convars_0_slot0.vcfg_lastclouded', 'cs2_user_keys_0_slot2.vcfg', 'ServerConfig.vdf']) {
    assert.equal(isCs2Setting(name), true, name);
  }
  assert.equal(isCs2Setting('source_mvm.cfg'), false);
  assert.equal(isCs2Setting('autoexec.cfg'), false);
});

test('a new pack replaces the old one and keeps the player\'s CS2 settings', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'payload', 'fx');
    const staged = path.join(root, 'staged');
    writeTree(target, INSTALLED);
    writeTree(staged, NEW_PACK);
    const result = await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS);
    assert.deepEqual(readTree(target), UPDATED);
    assert.equal(result.kept, Object.keys(PLAYER_SETTINGS).length);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('a cfg the package ships stays, except where the player has a CS2 settings file', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const staged = path.join(root, 'staged');
    writeTree(target, { ...INSTALLED,
      'source_mvm_fx/cfg/source_mvm.cfg': 'old package cfg',
      'source_mvm_fx/cfg/autoexec.cfg': 'player autoexec' });
    writeTree(staged, { ...NEW_PACK,
      'source_mvm_fx/cfg/source_mvm.cfg': 'new package cfg',
      'source_mvm_fx/cfg/cs2_video.txt': 'package video defaults' });
    await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS);
    const after = readTree(target);
    assert.equal(after['source_mvm_fx/cfg/cs2_video.txt'], 'player video');
    assert.equal(after['source_mvm_fx/cfg/source_mvm.cfg'], 'new package cfg');
    assert.equal(after['source_mvm_fx/cfg/autoexec.cfg'], 'player autoexec');
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('a fresh install has nothing to keep', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'payload', 'fx');
    const staged = path.join(root, 'staged');
    writeTree(staged, NEW_PACK);
    assert.deepEqual(await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS), { kept: 0 });
    assert.deepEqual(readTree(target), NEW_PACK);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('an install that fails part way puts the old folder back, settings included', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    writeTree(target, INSTALLED);
    await assert.rejects(replaceDirectory(target, async (into) => {
      writeTree(into, { 'source_mvm_fx/gameinfo.gi': 'half copied' });
      throw new Error('ENOSPC: no space left on device');
    }, FX_SAVED_SETTINGS), /previous install was kept[\s\S]*ENOSPC/);
    assert.deepEqual(readTree(target), INSTALLED);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('a carry-over that fails gives a moved bundle back and restores the old folder', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const bundle = path.join(root, 'bundle');
    writeTree(target, INSTALLED);
    // A package with a FILE named cfg cannot take the player's cfg folder.
    writeTree(bundle, { ...NEW_PACK, 'source_mvm_fx/cfg': 'not a folder' });
    await assert.rejects(replaceDirectory(target, async (into) => {
      fs.renameSync(bundle, into);
      return () => fs.renameSync(into, bundle);
    }, FX_SAVED_SETTINGS), /previous install was kept/);
    assert.deepEqual(readTree(target), INSTALLED);
    assert.deepEqual(readTree(bundle), { ...NEW_PACK, 'source_mvm_fx/cfg': 'not a folder' });
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

// Windows refuses to move a folder while any file in it is open, as it is while CS2 runs.
test('a folder that is still in use is left exactly as it was', { skip: process.platform !== 'win32' }, async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const staged = path.join(root, 'staged');
    writeTree(target, INSTALLED);
    writeTree(staged, NEW_PACK);
    const handle = fs.openSync(path.join(target, 'source_mvm_fx', 'source_mvm_fx.vpk'), 'r');
    try {
      await assert.rejects(replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS),
        /Close Counter-Strike 2 and try again/);
    } finally { fs.closeSync(handle); }
    assert.deepEqual(readTree(target), INSTALLED);
    assert.deepEqual(readTree(staged), NEW_PACK);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('an install cut short between the two moves is recovered on the next one', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const staged = path.join(root, 'staged');
    // Only the old folder, moved aside: the launcher closed before the new one arrived.
    writeTree(`${target}.previous`, INSTALLED);
    writeTree(staged, NEW_PACK);
    await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS);
    assert.deepEqual(readTree(target), UPDATED);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('a half-copied new folder is thrown away and the last good install survives a failed retry', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    // The launcher closed while the new pack was still being copied in.
    writeTree(`${target}.previous`, INSTALLED);
    writeTree(`${target}.incoming`, { 'source_mvm_fx/gameinfo.gi': 'half copied' });
    await assert.rejects(replaceDirectory(target, async (into) => {
      writeTree(into, { 'source_mvm_fx/gameinfo.gi': 'half copied again' });
      throw new Error('ENOSPC: no space left on device');
    }, FX_SAVED_SETTINGS), /previous install was kept[\s\S]*ENOSPC/);
    assert.deepEqual(readTree(target), INSTALLED);
    assert.equal(fs.existsSync(`${target}.incoming`), false);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('an install cut short before the carry-over never lets package defaults beat saved settings', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const staged = path.join(root, 'staged');
    // The new pack was in, with its default video settings, but the player's had not been copied.
    const packWithDefaults = { ...NEW_PACK, 'source_mvm_fx/cfg/cs2_video.txt': 'package video defaults' };
    writeTree(`${target}.previous`, INSTALLED);
    writeTree(`${target}.incoming`, packWithDefaults);
    writeTree(staged, packWithDefaults);
    await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS);
    assert.deepEqual(readTree(target), UPDATED);
    assert.equal(fs.existsSync(`${target}.incoming`), false);
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

test('a swap that finished before the old folder was deleted keeps the new folder', async () => {
  const root = tempRoot();
  try {
    const target = path.join(root, 'fx');
    const staged = path.join(root, 'staged');
    writeTree(`${target}.previous`, INSTALLED);
    writeTree(target, { ...UPDATED, 'source_mvm_fx/cfg/cs2_video.txt': 'newer video' });
    writeTree(staged, NEW_PACK);
    await replaceDirectory(target, moveIn(staged), FX_SAVED_SETTINGS);
    assert.deepEqual(readTree(target), { ...UPDATED, 'source_mvm_fx/cfg/cs2_video.txt': 'newer video' });
    assert.equal(fs.existsSync(`${target}.previous`), false);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

// ---- runtime-payload.cjs itself, with a stand-in for Electron -----------------------------------

function loadPayload({ userData, resources = '', packaged = false, fetch, extract, fsp = {} }) {
  const realFsp = launcherRequire('node:fs/promises');
  const context = vm.createContext({
    module: { exports: {} },
    Buffer,
    process: { resourcesPath: resources, env: { SystemRoot: process.env.SystemRoot } },
    require: (name) => {
      if (name === 'electron') {
        return { app: { isPackaged: packaged, getPath: () => userData, getVersion: () => '0.3.2' },
          net: { fetch: fetch || (() => { throw new Error('No download should run'); }) } };
      }
      if (name === 'node:child_process') return { execFile: extract || (() => { throw new Error('No unpack should run'); }) };
      if (name === 'node:fs/promises') return { ...realFsp, ...fsp };
      return launcherRequire(name);
    },
  });
  vm.runInContext(payloadScript, context);
  return context.module.exports;
}

function installedState(userData) {
  return JSON.parse(fs.readFileSync(path.join(userData, 'payload', 'installed.json'), 'utf8'));
}

function bundledFixture() {
  const root = tempRoot();
  const userData = path.join(root, 'user');
  const resources = path.join(root, 'resources');
  const bundleFx = path.join(resources, 'bundle', 'fx');
  writeTree(path.join(resources, 'bundle'), {
    'bundle.json': JSON.stringify({ version: '0.3.2', stamp: 'new', components: { runtime: 'r1', fx: 'f2' } }),
  });
  writeTree(bundleFx, NEW_PACK);
  writeTree(path.join(userData, 'payload', 'fx'), INSTALLED);
  writeTree(path.join(userData, 'payload', 'runtime'), { 'x64/AfxHookSource2.dll': 'hook' });
  writeTree(path.join(userData, 'payload'), { 'installed.json': JSON.stringify({
    runtime: { version: '0.3.1', sha256: 'bundle:r1', bundleStamp: 'bundle:r1', hotfix: 0, bytes: 0 },
    fx: { version: '0.3.1', sha256: 'bundle:f1', bundleStamp: 'bundle:f1', hotfix: 0, bytes: 0 },
  }) });
  return { root, userData, resources, bundleFx, fxDir: path.join(userData, 'payload', 'fx') };
}

test('the complete installer\'s FX update keeps the player\'s CS2 settings', async () => {
  const f = bundledFixture();
  try {
    const payload = loadPayload({ userData: f.userData, resources: f.resources, packaged: true });
    const result = await payload.installBundled();
    assert.equal(result.state, 'installed');
    assert.deepEqual(readTree(f.fxDir), UPDATED);
    assert.equal(fs.existsSync(`${f.fxDir}.previous`), false);
    assert.equal(installedState(f.userData).fx.bundleStamp, 'bundle:f2');
    assert.equal(payload.installedFxDir(), path.join(f.fxDir, 'source_mvm_fx'));
  } finally { fs.rmSync(f.root, { recursive: true, force: true }); }
});

test('a complete installer copy that fails leaves the installed FX pack and settings alone', async () => {
  const f = bundledFixture();
  try {
    const realFsp = launcherRequire('node:fs/promises');
    let copies = 0;
    const payload = loadPayload({ userData: f.userData, resources: f.resources, packaged: true, fsp: {
      // Another drive: no move, so the bundle is copied file by file, and the disk fills up.
      rename: async (from, to) => {
        if (from === f.bundleFx) throw Object.assign(new Error('EXDEV: cross-device link'), { code: 'EXDEV' });
        return realFsp.rename(from, to);
      },
      copyFile: async (from, to) => {
        copies += 1;
        if (copies > 1) throw Object.assign(new Error('ENOSPC: no space left on device'), { code: 'ENOSPC' });
        return realFsp.copyFile(from, to);
      },
    } });
    await assert.rejects(payload.installBundled(), /previous install was kept[\s\S]*ENOSPC/);
    assert.deepEqual(readTree(f.fxDir), INSTALLED);
    assert.equal(fs.existsSync(`${f.fxDir}.previous`), false);
    assert.equal(installedState(f.userData).fx.bundleStamp, 'bundle:f1');
    assert.deepEqual(readTree(f.bundleFx), NEW_PACK);
  } finally { fs.rmSync(f.root, { recursive: true, force: true }); }
});

function downloadFixture() {
  const root = tempRoot();
  const userData = path.join(root, 'user');
  const packageDir = path.join(root, 'package');
  writeTree(packageDir, { ...NEW_PACK, 'source_mvm_fx/particles/new.vpcf_c': 'new particle' });
  writeTree(path.join(userData, 'payload', 'fx'), INSTALLED);
  writeTree(path.join(userData, 'payload'), { 'installed.json': JSON.stringify({
    fx: { version: '0.3.1', sha256: 'f'.repeat(64), bytes: 1 } }) });
  const archive = Buffer.from('fx zip bytes');
  const sha256 = crypto.createHash('sha256').update(archive).digest('hex');
  const json = (data) => ({ ok: true, json: async () => data });
  const fetch = async (url) => {
    if (url.includes('/releases?')) {
      return json([{ draft: false, prerelease: true, tag_name: 'v0.3.2',
        assets: [{ name: 'payload-manifest.json', browser_download_url: 'https://example.invalid/payload-manifest.json' }] }]);
    }
    if (url.endsWith('payload-manifest.json')) {
      return json({ schema: 1, version: '0.3.2',
        components: { fx: { version: '0.3.2', url: 'https://example.invalid/fx.zip', sha256 } } });
    }
    if (url.endsWith('fx.zip')) {
      return { ok: true, headers: { get: () => String(archive.length) }, body: [archive] };
    }
    throw new Error(`unexpected fetch ${url}`);
  };
  // Stands in for tar.exe: "unpacks" the package folder into the -C destination.
  const extract = (_file, args, _options, callback) => {
    fs.cpSync(packageDir, args[args.indexOf('-C') + 1], { recursive: true });
    callback(null, '', '');
  };
  return { root, userData, fetch, extract, sha256, fxDir: path.join(userData, 'payload', 'fx') };
}

test('a downloaded FX update keeps the player\'s CS2 settings', async () => {
  const f = downloadFixture();
  try {
    const payload = loadPayload({ userData: f.userData, fetch: f.fetch, extract: f.extract });
    const result = await payload.installPayload(() => {});
    assert.equal(result.state, 'installed');
    assert.deepEqual(readTree(f.fxDir), { ...UPDATED, 'source_mvm_fx/particles/new.vpcf_c': 'new particle' });
    assert.equal(fs.existsSync(`${f.fxDir}.previous`), false);
    assert.equal(installedState(f.userData).fx.sha256, f.sha256);
  } finally { fs.rmSync(f.root, { recursive: true, force: true }); }
});

test('a downloaded FX update that cannot be moved in leaves the installed pack alone', async () => {
  const f = downloadFixture();
  try {
    const realFsp = launcherRequire('node:fs/promises');
    const payload = loadPayload({ userData: f.userData, fetch: f.fetch, extract: f.extract, fsp: {
      rename: async (from, to) => {
        if (path.basename(from) === 'unpacked') throw Object.assign(new Error('EPERM: operation not permitted'), { code: 'EPERM' });
        return realFsp.rename(from, to);
      },
    } });
    await assert.rejects(payload.installPayload(() => {}), /previous install was kept[\s\S]*EPERM/);
    assert.deepEqual(readTree(f.fxDir), INSTALLED);
    assert.equal(fs.existsSync(`${f.fxDir}.previous`), false);
    assert.equal(installedState(f.userData).fx.sha256, 'f'.repeat(64));
  } finally { fs.rmSync(f.root, { recursive: true, force: true }); }
});

test('the release payload never packages the build machine\'s CS2 settings', () => {
  const fxBlock = releasePayloadPs1.slice(releasePayloadPs1.indexOf("$fxStage = Join-Path $stagingDir 'fx'"),
    releasePayloadPs1.indexOf("New-PayloadZip -Name 'source-mvm-fx'"));
  assert.match(fxBlock, /Copy-Item -Path \(Join-Path \$FxRoot '\*'\)/);
  assert.match(fxBlock, /foreach \(\$cs2Settings in @\('cfg', 'ui_override\\ServerConfig\.vdf'\)\)/);
  assert.match(fxBlock, /Remove-Item -LiteralPath \$stagedSettings -Recurse -Force/);
  assert.ok(fxBlock.indexOf('Copy-Item') < fxBlock.indexOf('Remove-Item'), 'settings are removed after the copy');
});
