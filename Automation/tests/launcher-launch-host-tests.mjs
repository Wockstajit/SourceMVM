import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import { createRequire } from 'node:module';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';
const require = createRequire(import.meta.url);
const { runLaunchHost } = require('../../SourceMovieMakerLauncher/electron/launch-host.cjs');

function fixture(t, options = {}) {
  const dir = mkdtempSync(path.join(os.tmpdir(), 'source-mvm-launch-host-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  const config = { host: 'host.exe', args: ['--cs2', 'game path/cs2.exe'], cwd: dir,
    env: { SteamAppId: '730' }, logPath: path.join(dir, 'launch.log'), ...options };
  const start = (host, args, spawnOptions) => {
    assert.equal(host, config.host);
    assert.deepEqual(args, config.args);
    assert.equal(spawnOptions.windowsHide, true);
    assert.deepEqual(spawnOptions.stdio, ['ignore', 'pipe', 'pipe']);
    return child;
  };
  return { child, config, start, log: () => readFileSync(config.logPath, 'utf8') };
}
const resumed = 'SteamPath: Steam appid: 730\r\nCreated CS2 process 1234 suspended.\r\nHook injected; CS2 resumed.\r\n';

test('resumed output resolves at once; a later startup exit is reported separately', async t => {
  const exits = [];
  const f = fixture(t, { onStartupExit: (message, processId) => exits.push({ message, processId }) });
  const result = runLaunchHost(f.config, f.start);
  f.child.stdout.write(resumed);
  assert.equal((await result).processId, 1234);
  assert.equal(exits.length, 0);
  f.child.stderr.write('The game exited during startup (PID 1234, exit code 0x00000000).');
  f.child.emit('close', 1);
  assert.equal(exits.length, 1);
  assert.equal(exits[0].processId, 1234);
  assert.match(exits[0].message, /^The game exited during startup/);
  assert.match(exits[0].message, /Launch log:/);
  assert.doesNotMatch(exits[0].message, /SteamPath:/);
  assert.match(f.log(), /host exited 1/);
  assert.match(f.log(), /Hook injected; CS2 resumed/);
});

test('a game that survives the watch reports nothing after the launch', async t => {
  const exits = [];
  const f = fixture(t, { onStartupExit: message => exits.push(message) });
  const result = runLaunchHost(f.config, f.start);
  f.child.stdout.write(resumed);
  await result;
  f.child.stdout.write('Game survived initial startup.\n');
  f.child.emit('close', 0);
  assert.deepEqual(exits, []);
  assert.match(f.log(), /Game survived initial startup/);
});

test('access denied leads with the host error and logs the full launch context', async t => {
  const f = fixture(t);
  const result = runLaunchHost(f.config, f.start);
  f.child.stdout.write('SteamPath: Steam appid: 730\nGame: game path/cs2.exe\n');
  f.child.stderr.write('Windows could not start the game (CreateProcessW error 5: Access is denied).');
  f.child.emit('close', 1);
  await assert.rejects(result, /^Error: Windows could not start the game/);
  const log = f.log();
  assert.match(log, /SteamPath:/);
  assert.match(log, /Arguments:.*--cs2/);
  assert.match(log, /Working directory:/);
  assert.match(log, /CreateProcessW error 5/);
});

test('a successful host exit returns the exact PID and also logs success', async t => {
  const f = fixture(t);
  const result = runLaunchHost(f.config, f.start);
  f.child.stdout.write(resumed + 'Game survived initial startup.\n');
  f.child.emit('close', 0);
  assert.equal((await result).processId, 1234);
  assert.equal((await result).ok, true);
  assert.match(f.log(), /host exited 0/);
});

test('host spawn failure is logged once even when close follows error', async t => {
  const f = fixture(t);
  const result = runLaunchHost(f.config, f.start);
  f.child.emit('error', Object.assign(new Error('spawn EPERM'), { code: 'EPERM' }));
  f.child.emit('close', -1);
  await assert.rejects(result, /Windows blocked the launcher host/);
  assert.equal(f.log().match(/host exited/g).length, 1);
});

test('a logging failure preserves the actual startup error', async t => {
  const f = fixture(t);
  f.config.logPath = f.config.cwd; // A directory cannot be appended to.
  const result = runLaunchHost(f.config, f.start);
  f.child.stderr.write('Startup failed.');
  f.child.emit('close', 1);
  await assert.rejects(result, { message: 'Startup failed.' });
});

test('zero exit without a PID and termination by signal before resume both fail', async t => {
  for (const code of [0, null]) {
    const f = fixture(t);
    const result = runLaunchHost(f.config, f.start);
    if (code === null) f.child.stdout.write('Created CS2 process 1234 suspended.\r\n');
    f.child.emit('close', code);
    await assert.rejects(result, code === 0 ? /did not report a game process/ : /exited with code null/);
  }
});

test('UTF-8 error paths survive being split across stream chunks', async t => {
  const f = fixture(t);
  const result = runLaunchHost(f.config, f.start);
  const bytes = Buffer.from('Game: D:/Jeux/Ã©tÃ©/cs2.exe');
  const split = bytes.indexOf(Buffer.from('Ã©')) + 1;
  f.child.stderr.write(bytes.subarray(0, split));
  f.child.stderr.write(bytes.subarray(split));
  f.child.emit('close', 1);
  await assert.rejects(result, /Ã©tÃ©/);
});

function launchContext(f) {
  const source = readFileSync(new URL('../../SourceMovieMakerLauncher/electron/main.cjs', import.meta.url), 'utf8');
  const launch = source.slice(source.indexOf('async function launchWithSettings'), source.indexOf('\nfunction createWindow()'));
  const context = vm.createContext({
    fs: { existsSync: () => true }, path,
    app: { getPath: () => f.config.cwd },
    runtimeCandidate: name => name, hostPath: () => f.config.host,
    readSteamInstallPath: () => 'Steam', environmentFor: () => ({}), buildCommandLine: () => '-insecure',
    launchedCs2ProcessId: 0, startupWatchPending: false, STARTUP_WATCH_MS: 30000, hostArgs: null, exits: [],
    reportStartupExit: (message, processId) => context.exits.push({ message, processId }),
    runLaunchHost: config => { context.hostArgs = config.args; return runLaunchHost(config, () => f.child); },
  });
  vm.runInContext(launch, context);
  return context;
}

test('the app launch reports started on resume while the host keeps watching', async t => {
  const f = fixture(t);
  const context = launchContext(f);
  const result = context.launchWithSettings({ cs2Exe: 'game/cs2.exe' }, undefined, [], true);
  assert.deepEqual([...context.hostArgs.slice(-2)], ['--watch-startup-ms', '30000']);
  f.child.stdout.write(resumed);
  assert.equal((await result).processId, 1234);
  assert.equal(context.launchedCs2ProcessId, 1234);
  assert.equal(context.startupWatchPending, true);
  f.child.stderr.write('The game exited during startup (PID 1234, exit code 0xC0000005).');
  f.child.emit('close', 1);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(context.startupWatchPending, false);
  assert.equal(context.exits.length, 1);
  assert.equal(context.exits[0].processId, 1234);
  assert.match(context.exits[0].message, /0xC0000005/);
});

test('headless and automation-style launches do not ask the host to watch', async t => {
  const f = fixture(t);
  const context = launchContext(f);
  const result = context.launchWithSettings({ cs2Exe: 'game/cs2.exe' });
  assert.equal(context.hostArgs.includes('--watch-startup-ms'), false);
  f.child.stdout.write(resumed);
  f.child.emit('close', 0);
  const launched = await result;
  assert.equal(launched.processId, 1234);
  assert.equal('hostExited' in launched, false, 'IPC results must stay structured-cloneable');
});
