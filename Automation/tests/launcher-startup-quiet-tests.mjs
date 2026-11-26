// Launcher audit batch 3: startup cost, single instance, and idle animations.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';

const root = new URL('../../SourceMovieMakerLauncher/', import.meta.url);
const read = (rel) => fs.readFileSync(new URL(rel, root), 'utf8');
const mainSource = read('electron/main.cjs');

function slice(from, to) {
  const start = mainSource.indexOf(from);
  const end = mainSource.indexOf(to, start);
  assert.ok(start >= 0 && end > start, `${from} .. ${to} not found in main.cjs`);
  return mainSource.slice(start, end);
}

function settingsContext(userData, detections) {
  const context = vm.createContext({
    fs, path, settings: null, app: { getPath: () => userData },
    detectCs2Exe: () => { detections.count += 1; return 'C:\\cs2.exe'; },
  });
  vm.runInContext(slice('function defaultSettings()', '// ---- Workshop Tools')
    + slice('function configPath()', 'function readSteamInstallPath()'), context);
  return context;
}

test('saving settings does not search for CS2 again when the path is already known', () => {
  const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-save-'));
  try {
    const detections = { count: 0 };
    const context = settingsContext(userData, detections);
    const saved = context.saveSettings({ cs2Exe: 'D:\\Games\\cs2.exe', devMode: true });
    assert.equal(detections.count, 0, 'reg.exe and the Steam libraries are not read on a save');
    assert.equal(saved.cs2Exe, 'D:\\Games\\cs2.exe');
    assert.equal(saved.devMode, true);
    assert.equal(saved.autoUpdate, false, 'missing keys still fall back to their defaults');
    context.saveSettings({ devMode: false });
    assert.equal(detections.count, 1, 'a save without a CS2 path still finds one');
    context.loadSettings();
    assert.equal(detections.count, 2, 'loading still detects');
  } finally {
    fs.rmSync(userData, { recursive: true, force: true });
  }
});

test('a second GUI launch focuses the open launcher; headless runs are not locked out', () => {
  assert.match(mainSource, /const ownsInstance = startsHeadless \|\| app\.requestSingleInstanceLock\(\);/);
  assert.match(mainSource, /if \(!ownsInstance\) \{\s*app\.quit\(\);/);
  assert.match(mainSource, /app\.on\('second-instance'[\s\S]{0,240}mainWindow\.focus\(\);/);
  assert.match(mainSource, /app\.whenReady\(\)\.then\(async \(\) => \{\s*if \(!ownsInstance\) return;/);
});

test('only an unpackaged launcher can be pointed at another renderer page', () => {
  assert.match(mainSource, /const rendererUrl = app\.isPackaged \? '' : process\.env\.SMVM_RENDERER_URL;/);
});

test('the launcher holds its looping video and title sweep while CS2 runs or it is in the background', () => {
  const app = read('src/App.tsx');
  const css = read('src/styles.css');
  assert.match(app, /const quiet = started \|\| advanced \|\| !windowActive;/);
  assert.match(app, /if \(quiet\) video\.pause\(\);/);
  assert.match(app, /ref=\{linemapRef\}/);
  assert.match(app, /className=\{`launcher \$\{introDone \? 'is-ready' : ''\} \$\{quiet \? 'is-quiet' : ''\}`\}/);
  assert.match(css, /\.launcher\.is-quiet \.wm-title,\s*\.launcher\.is-quiet \.wm-trail,\s*\.launcher\.is-quiet \.wm-ghost \{\s*animation-play-state: paused;/);
  // The intro video is gone once it has played, and Space / Escape / a click skip it.
  assert.match(app, /\{!introDone && <video\s+className="intro-video"/);
  assert.match(app, /if \(event\.key === ' ' \|\| event\.key === 'Escape'\) finishIntro\(\);/);
});

test('the launch host resumes CS2 right after injection unless a debug helper needs time', () => {
  const host = read('host/Program.cs');
  assert.match(host, /public int SettleMilliseconds \{ get; private set; \}\r?\n/);
  assert.doesNotMatch(host, /SettleMilliseconds \{ get; private set; \} = 2000;/);
  assert.match(host, /else if \(!string\.IsNullOrWhiteSpace\(options\.DebugHelperPath\)\)\s*options\.SettleMilliseconds = 2000;/);
});
