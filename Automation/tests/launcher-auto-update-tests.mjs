// Automatic updates are opt-in and GO works without internet (electron/main.cjs, update-service.cjs,
// src/App.tsx). Runs the real settings and IPC code from main.cjs in a vm sandbox, with a temporary
// userData folder and an update service whose network boundary is stubbed, so nothing here needs
// Electron, CS2 or a connection.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { test } from 'node:test';
import vm from 'node:vm';

const require = createRequire(import.meta.url);
const { createUpdateService } = require('../../SourceMovieMakerLauncher/electron/update-service.cjs');

const launcher = new URL('../../SourceMovieMakerLauncher/', import.meta.url);
const mainSource = fs.readFileSync(new URL('electron/main.cjs', launcher), 'utf8');
const appSource = fs.readFileSync(new URL('src/App.tsx', launcher), 'utf8');
const typesSource = fs.readFileSync(new URL('src/vite-env.d.ts', launcher), 'utf8');

function slice(from, to) {
  const start = mainSource.indexOf(from);
  const end = mainSource.indexOf(to, start);
  assert.ok(start >= 0 && end > start, `${from} was not found in main.cjs`);
  return mainSource.slice(start, end);
}

// defaultSettings/loadSettings/saveSettings from main.cjs against a temporary userData folder.
function settingsContext(userData) {
  const context = vm.createContext({
    fs, path, settings: null, detectCs2Exe: () => '', app: { getPath: () => userData },
  });
  vm.runInContext(slice('function defaultSettings()', '// ---- Workshop Tools')
    + slice('function configPath()', 'function readSteamInstallPath()'), context);
  return context;
}

function tempUserData() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-auto-update-'));
}

function service(overrides = {}) {
  const calls = [], logs = [];
  const created = createUpdateService({
    packaged: true, portable: false, canInstall: () => true,
    automaticUpdates: () => false,
    loadReleases: async () => { calls.push('releases'); throw new Error('ENOTFOUND api.github.com'); },
    prepareRuntime: async (options) => { calls.push(['runtime', options]); },
    updater: {
      checkForUpdates: async () => { calls.push('updater'); return { isUpdateAvailable: false }; },
      downloadUpdate: async () => { calls.push('download'); return []; },
      quitAndInstall: () => { calls.push('restart'); },
    },
    log: line => logs.push(line),
    ...overrides,
  });
  return { service: created, calls, logs };
}

// registerIpc() from main.cjs with the collaborators it reaches for stubbed.
function ipcContext(updateService, settings) {
  const handlers = new Map();
  const context = vm.createContext({
    ipcMain: { on() {}, handle: (name, fn) => handlers.set(name, fn) },
    updateService, settings, launchInProgress: false, launches: 0,
    saveSettings: next => { context.settings = next; return next; },
    launchWithSettings: async () => { context.launches++; return { ok: true }; },
  });
  vm.runInContext(slice('function registerIpc()', '\nasync function runHeadless') + '\nregisterIpc();', context);
  return { handlers, context };
}

test('a fresh install and a config without the key both start with automatic updates off', () => {
  const userData = tempUserData();
  try {
    const context = settingsContext(userData);
    assert.equal(context.defaultSettings().autoUpdate, false);
    assert.equal(context.loadSettings().autoUpdate, false);
    // An existing launcher.json from before the setting existed.
    fs.mkdirSync(userData, { recursive: true });
    fs.writeFileSync(path.join(userData, 'launcher.json'), JSON.stringify({ devMode: true, customLaunchOptions: '-console' }));
    const loaded = context.loadSettings();
    assert.equal(loaded.autoUpdate, false);
    assert.equal(loaded.devMode, true);
  } finally {
    fs.rmSync(userData, { recursive: true, force: true });
  }
});

test('the setting persists across restarts with the other launcher settings', () => {
  const userData = tempUserData();
  try {
    const first = settingsContext(userData);
    const saved = first.saveSettings({ ...first.defaultSettings(), autoUpdate: true });
    assert.equal(saved.autoUpdate, true);
    const onDisk = JSON.parse(fs.readFileSync(path.join(userData, 'launcher.json'), 'utf8'));
    assert.equal(onDisk.autoUpdate, true);
    // A restart: a new context reads the same file.
    const restarted = settingsContext(userData);
    assert.equal(restarted.loadSettings().autoUpdate, true);
    restarted.saveSettings({ ...restarted.loadSettings(), autoUpdate: false });
    assert.equal(settingsContext(userData).loadSettings().autoUpdate, false);
  } finally {
    fs.rmSync(userData, { recursive: true, force: true });
  }
});

test('turning the setting on runs the usual update check once; saving it unchanged or off does not', async () => {
  let checks = 0;
  const { handlers } = ipcContext({ check: () => { checks++; return Promise.resolve({ state: 'ready' }); } },
    { autoUpdate: false });
  await handlers.get('settings:save')(null, { autoUpdate: false });
  assert.equal(checks, 0);
  await handlers.get('settings:save')(null, { autoUpdate: true });
  assert.equal(checks, 1);
  await handlers.get('settings:save')(null, { autoUpdate: true, devMode: true });
  assert.equal(checks, 1);
  await handlers.get('settings:save')(null, { autoUpdate: false });
  assert.equal(checks, 1);
  await handlers.get('settings:save')(null, { autoUpdate: true });
  assert.equal(checks, 2);
});

test('GO works with no internet: automatic updates off never touches the network', async () => {
  const s = service();
  const { handlers, context } = ipcContext(s.service, { autoUpdate: false });
  // What app.whenReady() does at startup.
  const startup = await s.service.check();
  assert.equal(startup.state, 'ready');
  assert.equal(startup.message, 'UPDATES OFF');
  const result = await handlers.get('cs2:launch')(null, { autoUpdate: false });
  assert.equal(result.ok, true);
  assert.equal(context.launches, 1);
  assert.deepEqual(s.calls, [['runtime', { online: false }]]);
  assert.deepEqual(s.logs, []);
});

test('GO works with no internet: automatic updates on logs the failed check once and launches', async () => {
  const s = service({ automaticUpdates: () => true });
  const { handlers, context } = ipcContext(s.service, { autoUpdate: true });
  const startup = await s.service.check();
  assert.equal(startup.state, 'ready');
  assert.equal(startup.skipped, true);
  const result = await handlers.get('cs2:launch')(null, { autoUpdate: true });
  assert.equal(result.ok, true);
  assert.equal(context.launches, 1);
  assert.deepEqual(s.calls, ['releases', ['runtime', { online: false }]]);
  assert.deepEqual(s.logs, ['Update check skipped: ENOTFOUND api.github.com']);
});

test('the UPDATES button is a manual check and the startup check is automatic', async () => {
  const s = service({ loadReleases: async () => { s.calls.push('releases'); return [{ draft: false, assets: [{ name: 'latest.yml' }] }]; } });
  const { handlers } = ipcContext(s.service, { autoUpdate: false });
  assert.equal((await handlers.get('update:check')()).message, 'UP TO DATE');
  assert.deepEqual(s.calls, ['releases', 'updater', ['runtime', { online: true }]]);
  assert.match(mainSource, /^\s*void updateService\.check\(\);$/m);
  assert.match(mainSource, /ipcMain\.handle\('update:check', \(\) => updateService\.check\(\{ manual: true \}\)\)/);
});

test('the launch path itself reads nothing from the network', () => {
  const launch = slice('async function launchWithSettings', '\nfunction createWindow()');
  for (const forbidden of ['net.fetch', 'https://', 'checkForPayload', 'applyRuntimeHotfix', 'checkForUpdates', 'installPayload(']) {
    assert.ok(!launch.includes(forbidden), `launchWithSettings must not use ${forbidden}`);
  }
  // Only the hotfix and payload checks may use the network while readying the runtime, and only online.
  const runtime = slice('prepareRuntime: async ({ online })', 'async function launchWithSettings');
  assert.match(runtime, /if \(online\) \{\s*try \{\s*await runtimePayload\.applyRuntimeHotfix/);
  assert.match(runtime, /\} else if \(online\) \{[\s\S]*?checkForPayload\(\)/);
  assert.match(runtime, /catch \(error\) \{\s*console\.warn\('runtime payload check failed:'/);
});

test('the renderer carries the setting, the Updates tab and its toggle', () => {
  assert.match(typesSource, /autoUpdate: boolean;/);
  assert.match(typesSource, /skipped\?: boolean;/);
  assert.match(appSource, /const emptySettings: LauncherSettings = \{[\s\S]*?autoUpdate: false,[\s\S]*?\};/);
  assert.match(appSource, /type AdvancedTab = 'game' \| 'runtime' \| 'locations' \| 'dlss' \| 'updates'/);
  assert.match(appSource, /id="advanced-tab-updates"[\s\S]*?>UPDATES<\/button>/);
  assert.match(appSource, /advancedTab === 'updates' && <div id="advanced-panel-updates"/);
  assert.match(appSource, /checked=\{settings\.autoUpdate\}/);
  assert.match(appSource, /patch\(\{ autoUpdate: event\.target\.checked \}\)/);
  assert.match(appSource, /Check for updates automatically/);
  // Ready-but-skipped is not shown as a green "up to date".
  assert.match(appSource, /status\.state === 'ready' && !status\.skipped \? 'good' : 'normal'/);
  assert.match(mainSource, /^\s*autoUpdate: false,$/m);
});
