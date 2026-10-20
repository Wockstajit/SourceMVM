import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
const require = createRequire(import.meta.url);
const { createUpdateService } = require('../../SourceMovieMakerLauncher/electron/update-service.cjs');

const release = { draft: false, assets: [{ name: 'latest.yml' }] };
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
// Automatic updates are on here so the online path is what most tests exercise; the opt-in
// tests below turn them off.
function fixture(overrides = {}) {
  const calls = [], events = [], logs = [];
  const service = createUpdateService({ packaged: true, portable: false,
    automaticUpdates: () => true,
    loadReleases: async () => [release],
    prepareRuntime: async () => { calls.push('runtime'); },
    canInstall: () => true,
    updater: {
      checkForUpdates: async () => { calls.push('check'); return { isUpdateAvailable: false }; },
      downloadUpdate: async () => { calls.push('download'); return ['verified.exe']; },
      quitAndInstall: (...args) => calls.push(['restart', ...args]),
    }, ...overrides, report: status => events.push(status), log: line => logs.push(line) });
  return { service, calls, events, logs };
}
test('launch is denied before the first check and throughout runtime installation', async () => {
  const pending = deferred();
  const f = fixture({ prepareRuntime: () => pending.promise });
  assert.throws(f.service.assertCanLaunch, /Wait for/);
  const result = f.service.check();
  await new Promise(resolve => setImmediate(resolve));
  assert.throws(f.service.assertCanLaunch, /Wait for/);
  pending.resolve();
  assert.equal((await result).launchAllowed, true);
  assert.doesNotThrow(f.service.assertCanLaunch);
});
test('a found update is offered, not installed: the runtime readies and GO stays open', async () => {
  const f = fixture({ updater: {
    checkForUpdates: async () => { f.calls.push('check'); return { isUpdateAvailable: true, updateInfo: { version: '0.1.7' } }; },
    downloadUpdate: async () => { f.calls.push('download'); return ['verified.exe']; },
    quitAndInstall: () => f.calls.push('restart'),
  } });
  const result = await f.service.check();
  assert.equal(result.state, 'ready');
  assert.equal(result.message, 'UPDATE 0.1.7 AVAILABLE');
  assert.deepEqual(result.update, { version: '0.1.7', installable: true });
  assert.doesNotThrow(f.service.assertCanLaunch);
  assert.deepEqual(f.calls, ['check', 'runtime']);
});
test('saying yes downloads, restarts automatically, and never unlocks the older process', async () => {
  const calls = [], download = deferred();
  const f = fixture({ updater: {
    checkForUpdates: async () => ({ isUpdateAvailable: true, updateInfo: { version: '0.1.7' } }),
    downloadUpdate: () => download.promise,
    quitAndInstall: (...args) => calls.push(args),
  } });
  await f.service.check();
  const result = f.service.install();
  await new Promise(resolve => setImmediate(resolve));
  f.service.progress(37);
  assert.equal(f.service.getStatus().percent, 37);
  assert.throws(f.service.assertCanLaunch);
  download.resolve(['verified.exe']);
  assert.equal((await result).state, 'installing');
  assert.deepEqual(calls, [[false, true]]);
  assert.equal(f.events.filter(event => event.launchAllowed).length, 1); // the offer, before yes
  assert.throws(f.service.assertCanLaunch);
  assert.equal((await f.service.check()).state, 'installing');
  assert.equal((await f.service.install()).state, 'installing');
});
test('install with nothing pending, or a check that finds nothing, leaves no offer', async () => {
  const f = fixture();
  const result = await f.service.install();
  assert.equal(result.state, 'ready');
  assert.match(result.message, /No update is known/);
  assert.equal(result.update, undefined);
  assert.doesNotThrow(f.service.assertCanLaunch);
  assert.deepEqual(f.calls, []);
  assert.deepEqual(f.logs, ['Update not installed: No update is known. Press UPDATES to check.']);
  assert.equal((await f.service.check()).update, undefined);
});
test('startup and button checks share one in-flight operation', async () => {
  const pending = deferred();
  let checks = 0;
  const f = fixture({ loadReleases: () => { checks++; return pending.promise; } });
  const a = f.service.check(), b = f.service.check();
  assert.equal(a, b);
  pending.resolve([]);
  await a;
  assert.equal(checks, 1);
});
test('empty public repo and draft releases have no update; installed runtime can become ready', async () => {
  for (const releases of [[], [{ draft: true }]]) {
    const f = fixture({ loadReleases: async () => releases });
    assert.equal((await f.service.check()).message, 'NO UPDATES PUBLISHED');
    assert.deepEqual(f.calls, ['runtime']);
    assert.doesNotThrow(f.service.assertCanLaunch);
  }
});
test('an unreachable update server is logged once and the installed runtime still launches', async () => {
  let online = false;
  const runtimeCalls = [];
  const f = fixture({
    loadReleases: async () => { if (!online) throw new Error('Offline'); return [release]; },
    prepareRuntime: async (options) => { runtimeCalls.push(options); },
  });
  const result = await f.service.check();
  assert.equal(result.state, 'ready');
  assert.equal(result.skipped, true);
  assert.equal(result.message, 'OFFLINE / UPDATE CHECK SKIPPED');
  assert.doesNotThrow(f.service.assertCanLaunch);
  assert.deepEqual(f.logs, ['Update check skipped: Offline']);
  assert.deepEqual(f.calls, []);
  // The runtime side is told to stay offline too (no hotfix or payload manifest fetch).
  assert.deepEqual(runtimeCalls, [{ online: false }]);
  online = true;
  const next = await f.service.check();
  assert.equal(next.state, 'ready');
  assert.equal(next.message, 'UP TO DATE');
  assert.equal(next.skipped, undefined);
  assert.deepEqual(runtimeCalls, [{ online: false }, { online: true }]);
  assert.deepEqual(f.calls, ['check']);
  assert.equal(f.logs.length, 1);
});
test('automatic updates off: no release list, no updater, the runtime readies offline', async () => {
  const runtimeCalls = [];
  const f = fixture({
    automaticUpdates: () => false,
    loadReleases: () => { throw new Error('Unexpected network'); },
    prepareRuntime: async (options) => { runtimeCalls.push(options); },
  });
  const result = await f.service.check();
  assert.equal(result.state, 'ready');
  assert.equal(result.message, 'UPDATES OFF');
  assert.equal(result.launchAllowed, true);
  assert.doesNotThrow(f.service.assertCanLaunch);
  assert.deepEqual(f.calls, []);
  assert.deepEqual(f.logs, []);
  assert.deepEqual(runtimeCalls, [{ online: false }]);
});
test('the UPDATES button checks once even while automatic updates are off', async () => {
  const runtimeCalls = [];
  let fetched = 0;
  const f = fixture({
    automaticUpdates: () => false,
    loadReleases: async () => { fetched++; return [release]; },
    prepareRuntime: async (options) => { runtimeCalls.push(options); },
  });
  const result = await f.service.check({ manual: true });
  assert.equal(result.message, 'UP TO DATE');
  assert.equal(fetched, 1);
  assert.deepEqual(f.calls, ['check']);
  assert.deepEqual(runtimeCalls, [{ online: true }]);
  // The next automatic check is offline again.
  assert.equal((await f.service.check()).message, 'UPDATES OFF');
  assert.equal(fetched, 1);
});
test('automatic updates off still needs an installed runtime before GO unlocks', async () => {
  const f = fixture({
    automaticUpdates: () => false,
    prepareRuntime: async () => { throw new Error('No runtime is installed yet.'); },
  });
  assert.match((await f.service.check()).message, /No runtime is installed yet/);
  assert.throws(f.service.assertCanLaunch);
});
test('a malformed list or a published release with no update metadata never reports current', async () => {
  for (const releases of [{}, [{ draft: false, assets: [] }]]) {
    const f = fixture({ loadReleases: async () => releases });
    assert.equal((await f.service.check()).state, 'error');
    assert.throws(f.service.assertCanLaunch);
    assert.deepEqual(f.calls, []);
  }
});
test('runtime installation failure blocks GO even when the launcher is current', async () => {
  const f = fixture({ prepareRuntime: async () => { throw new Error('Runtime locked'); } });
  assert.match((await f.service.check()).message, /Runtime locked/);
  assert.throws(f.service.assertCanLaunch);
});
test('a failed metadata check blocks launch; a failed download or install keeps the current app launchable', async () => {
  const f = fixture({ updater: { checkForUpdates: async () => { throw new Error('Bad metadata'); } } });
  assert.equal((await f.service.check()).state, 'error');
  assert.throws(f.service.assertCanLaunch);
  for (const at of ['download', 'install', 'empty']) {
    const g = fixture({ updater: {
      checkForUpdates: async () => ({ isUpdateAvailable: true, updateInfo: { version: '0.1.7' } }),
      downloadUpdate: async () => {
        if (at === 'download') throw new Error('Checksum mismatch');
        return at === 'empty' ? [] : ['verified.exe'];
      },
      quitAndInstall: () => { if (at === 'install') throw new Error('Installer failed'); },
    } });
    await g.service.check();
    const result = await g.service.install();
    assert.equal(result.state, 'ready', at);
    assert.match(result.message, /^UPDATE NOT INSTALLED: .*PRESS UPDATES TO TRY AGAIN\.$/);
    assert.deepEqual(result.update, { version: '0.1.7', installable: true });
    assert.doesNotThrow(g.service.assertCanLaunch);
    assert.equal(g.logs.length, 1);
  }
});
test('a game running at discovery blocks readiness; one starting during download prevents restart', async () => {
  let running = true, restarts = 0;
  const f = fixture({ canInstall: () => !running, updater: {
    checkForUpdates: async () => ({ isUpdateAvailable: true }),
    downloadUpdate: async () => ['verified.exe'],
    quitAndInstall: () => restarts++,
  } });
  assert.match((await f.service.check()).message, /Close CS2/);
  assert.throws(f.service.assertCanLaunch);
  running = false;
  await f.service.check();
  const g = fixture({ canInstall: () => !running, updater: {
    checkForUpdates: async () => ({ isUpdateAvailable: true }),
    downloadUpdate: async () => { running = true; return ['verified.exe']; },
    quitAndInstall: () => restarts++,
  } });
  await g.service.check();
  const result = await g.service.install();
  assert.match(result.message, /Close CS2/);
  assert.equal(restarts, 0);
  // Nothing was installed, so this version still launches once CS2 is closed.
  assert.equal(result.state, 'ready');
});
test('a named blocker replaces the generic close-CS2 message and still blocks launch', async () => {
  const f = fixture({ canInstall: () => 'cs2.exe (PID 4321) is still running.' });
  const result = await f.service.check();
  assert.equal(result.state, 'error');
  assert.match(result.message, /PID 4321.*Retry UPDATES/);
  assert.deepEqual(f.calls, ['check']);
  assert.throws(f.service.assertCanLaunch);
});
test('unpackaged development never attempts app installation or network access', async () => {
  const f = fixture({ packaged: false, loadReleases: () => { throw new Error('Unexpected network'); } });
  assert.equal((await f.service.check()).state, 'ready');
  assert.deepEqual(f.calls, []);
});
test('portable app with a known update names the setup exe, cannot self-install, and still launches', async () => {
  const f = fixture({ portable: true, updater: { checkForUpdates: async () => ({ isUpdateAvailable: true, updateInfo: { version: '0.1.7' } }) } });
  const result = await f.service.check();
  assert.equal(result.message, 'UPDATE 0.1.7 AVAILABLE / INSTALL THE NEW SETUP EXE');
  assert.deepEqual(result.update, { version: '0.1.7', installable: false });
  assert.doesNotThrow(f.service.assertCanLaunch);
  const install = await f.service.install();
  assert.match(install.message, /Portable builds cannot replace themselves/);
  assert.equal(install.state, 'ready');
  assert.deepEqual(f.calls, ['runtime']);
});
test('new checks lock launch immediately and returned snapshots cannot bypass the gate', async () => {
  const f = fixture();
  await f.service.check();
  const snapshot = f.service.getStatus();
  snapshot.launchAllowed = false;
  assert.doesNotThrow(f.service.assertCanLaunch);
  const next = f.service.check();
  assert.throws(f.service.assertCanLaunch);
  await next;
});
test('asynchronous updater errors prevent a late current result from unlocking GO', async () => {
  const pending = deferred();
  const f = fixture({ prepareRuntime: () => pending.promise });
  const result = f.service.check();
  await new Promise(resolve => setImmediate(resolve));
  f.service.fail(new Error('Late update error'));
  pending.resolve();
  assert.equal((await result).state, 'error');
  assert.throws(f.service.assertCanLaunch);
});
test('the actual main-process launch IPC denies bypasses and launches only after readiness', async () => {
  const source = readFileSync(new URL('../../SourceMovieMakerLauncher/electron/main.cjs', import.meta.url), 'utf8');
  const register = source.slice(source.indexOf('function registerIpc()'), source.indexOf('\nasync function runHeadless'));
  const handlers = new Map(), f = fixture();
  let saves = 0, launches = 0;
  const context = vm.createContext({
    ipcMain: { on() {}, handle: (name, fn) => handlers.set(name, fn) },
    updateService: f.service, settings: {}, launchInProgress: false,
    saveSettings: next => { saves++; return next; },
    launchWithSettings: async () => { launches++; return { ok: true }; },
  });
  vm.runInContext(register + '\nregisterIpc();', context);
  await assert.rejects(handlers.get('cs2:launch')(null, {}), /Wait for/);
  assert.equal(saves, 0);
  assert.equal(launches, 0);
  await handlers.get('update:check')();
  const result = await handlers.get('cs2:launch')(null, {});
  assert.equal(result.ok, true);
  assert.equal(saves, 1);
  assert.equal(launches, 1);
  assert.equal(context.launchInProgress, false);
  f.service.fail(new Error('Known update failed'));
  await assert.rejects(handlers.get('cs2:launch')(null, {}), /Known update failed/);
  assert.equal(launches, 1);
});

function headlessContext(service) {
  const source = readFileSync(new URL('../../SourceMovieMakerLauncher/electron/main.cjs', import.meta.url), 'utf8');
  const run = source.slice(source.indexOf('async function runHeadless'), source.indexOf('\napp.whenReady()'));
  const calls = [];
  const context = vm.createContext({
    updateService: service,
    cliSettings: () => ({ current: {}, commandLine: '', envValues: [] }),
    runtimePayload: { isBundled: () => true, installBundled: () => { calls.push('unguarded bundle write'); } },
    launchWithSettings: async () => { calls.push('launch'); },
    process: { exit: code => calls.push(['exit', code]), stderr: { write: value => calls.push(['stderr', value]) } },
  });
  vm.runInContext(run, context);
  return { run: () => context.runHeadless([]), calls };
}

test('headless startup waits for guarded runtime readiness and never writes a bundle while CS2 is open', async () => {
  const blocked = fixture({ canInstall: () => false });
  const cli = headlessContext(blocked.service);
  await cli.run();
  assert.deepEqual(blocked.calls, ['check']);
  assert.match(cli.calls[0][1], /Close CS2/);
  assert.deepEqual(cli.calls[1], ['exit', 1]);
  const ready = fixture(), current = headlessContext(ready.service);
  await current.run();
  assert.deepEqual(ready.calls, ['check', 'runtime']);
  assert.deepEqual(current.calls, ['launch', ['exit', 0]]);
});

test('headless launches without the network when automatic updates are off or the server is down', async () => {
  for (const automatic of [false, true]) {
    const f = fixture({ automaticUpdates: () => automatic,
      loadReleases: async () => { throw new Error('ENOTFOUND api.github.com'); } });
    const cli = headlessContext(f.service);
    await cli.run();
    assert.deepEqual(f.calls, ['runtime']);
    assert.deepEqual(cli.calls, ['launch', ['exit', 0]]);
    assert.deepEqual(f.logs, automatic ? ['Update check skipped: ENOTFOUND api.github.com'] : []);
  }
});

test('headless launches the installed app when an update is found; nobody is there to say yes', async () => {
  let restarts = 0;
  const next = fixture({ updater: {
    checkForUpdates: async () => { next.calls.push('check'); return { isUpdateAvailable: true, updateInfo: { version: '0.1.7' } }; },
    downloadUpdate: async () => ['verified.exe'],
    quitAndInstall: () => { restarts++; },
  } });
  const cli = headlessContext(next.service);
  await cli.run();
  assert.equal(restarts, 0);
  assert.deepEqual(cli.calls, ['launch', ['exit', 0]]);
  assert.deepEqual(next.calls, ['check', 'runtime']);
  assert.equal(next.service.getStatus().update.version, '0.1.7');
});
