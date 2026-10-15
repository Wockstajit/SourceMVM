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

test('resumed output cannot resolve launch before a later startup failure', async t => {
  const f = fixture(t);
  let settled = false;
  const result = runLaunchHost(f.config, f.start);
  result.then(() => { settled = true; }, () => { settled = true; });
  f.child.stdout.write(resumed);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(settled, false);
  f.child.stderr.write('The game exited during startup (PID 1234, exit code 0x00000000).');
  f.child.emit('close', 1);
  await assert.rejects(result, error => {
    assert.match(error.message, /^The game exited during startup/);
    assert.match(error.message, /Launch log:/);
    assert.doesNotMatch(error.message, /SteamPath:/);
    return true;
  });
  assert.match(f.log(), /host exited 1/);
  assert.match(f.log(), /Hook injected; CS2 resumed/);
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

test('zero exit without a PID and termination by signal both fail', async t => {
  for (const code of [0, null]) {
    const f = fixture(t);
    const result = runLaunchHost(f.config, f.start);
    if (code === null) f.child.stdout.write(resumed);
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

test('the real launch function records its PID only after the monitored host succeeds', async t => {
  const f = fixture(t);
  const source = readFileSync(new URL('../../SourceMovieMakerLauncher/electron/main.cjs', import.meta.url), 'utf8');
  const launch = source.slice(source.indexOf('async function launchWithSettings'), source.indexOf('\nfunction createWindow()'));
  const context = vm.createContext({
    fs: { existsSync: () => true }, path,
    app: { getPath: () => f.config.cwd },
    runtimeCandidate: name => name, hostPath: () => f.config.host,
    readSteamInstallPath: () => 'Steam', environmentFor: () => ({}), buildCommandLine: () => '-insecure',
    launchedCs2ProcessId: 0,
    runLaunchHost: config => runLaunchHost(config, () => f.child),
  });
  vm.runInContext(launch, context);
  const result = context.launchWithSettings({ cs2Exe: 'game/cs2.exe' });
  f.child.stdout.write(resumed);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(context.launchedCs2ProcessId, 0);
  f.child.emit('close', 0);
  assert.equal((await result).processId, 1234);
  assert.equal(context.launchedCs2ProcessId, 1234);
});
