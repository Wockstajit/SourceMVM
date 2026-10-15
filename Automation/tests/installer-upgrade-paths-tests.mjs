import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

const repo = fileURLToPath(new URL('../../', import.meta.url));
const read = file => readFileSync(path.join(repo, file), 'utf8');
const launcher = path.join(repo, 'SourceMovieMakerLauncher');
const pkg = JSON.parse(read('SourceMovieMakerLauncher/package.json'));
const nsh = read('SourceMovieMakerLauncher/build-resources/installer.nsh');
const bundleScript = read('SourceMovieMakerLauncher/scripts/build-bundle.ps1');
const long = p => `\\\\?\\${p}`;

test('the installer includes the upgrade pre-clean', () => {
  assert.equal(pkg.build.nsis.include, 'build-resources/installer.nsh');
});

test('the pre-clean runs after the app is closed and only inside a real install folder', () => {
  const body = nsh.slice(nsh.indexOf('!macro customCheckAppRunning'));
  const check = body.indexOf('!insertmacro _CHECK_APP_RUNNING');
  const clean = body.indexOf('!insertmacro removeLongBundledPaths');
  assert.ok(check > 0 && clean > check, 'electron-builder\'s app check must run before files are removed');
  assert.match(body, /IS_POWERSHELL_AVAILABLE/);
  assert.match(nsh, /Var pid/);
  assert.match(nsh, /FileExists\} "\$0\\\$\{APP_EXECUTABLE_FILENAME\}"/);
  assert.match(nsh, /rd \/s \/q "\\\\\?\\\$0\\resources\\bundle\\runtime\\osu"/);
});

test('an elevated all-users install cleans up too, and a failed delete is reported', () => {
  // electron-builder skips its app check in the elevated inner instance of an all-users install.
  const init = nsh.slice(nsh.indexOf('!macro customInit'));
  assert.match(init, /\$\{if\} \$\{UAC_IsInnerInstance\}\s+!insertmacro removeLongBundledPathsFromInstalls/);
  const remove = nsh.slice(nsh.indexOf('!macro removeLongBundledPaths DIR'), nsh.indexOf('!macro removeLongBundledPathsFromInstalls'));
  assert.ok(remove.indexOf('MessageBox') > remove.indexOf('rd /s /q'), 'a folder still present after rd is reported');
  assert.match(remove, /\/SD IDOK/, 'silent installs must not block on the message');
});

test('the complete bundle build shortens paths before stamping', () => {
  const shorten = bundleScript.indexOf('shorten-bundle-paths.ps1');
  assert.ok(shorten > 0 && shorten < bundleScript.indexOf('# ---- stamp'));
});

const windows = process.platform === 'win32';
function shorten(root) {
  return execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
    path.join(launcher, 'scripts', 'shorten-bundle-paths.ps1'), '-Root', root], { encoding: 'utf8', stdio: 'pipe' });
}

test('long set folders and definition files get short stable names; referenced files keep theirs', { skip: !windows }, () => {
  const root = mkdtempSync(path.join(tmpdir(), 'bundle-paths-'));
  try {
    const name = 'An Extremely Long Artist Name - A Very Long Title Indeed (Extended Cut Ver.) (Mapper)';
    const set = path.join(root, 'runtime', 'osu', 'Songs', name);
    mkdirSync(long(set), { recursive: true });
    writeFileSync(long(path.join(set, `${name} [Some Very Long Difficulty Name].osu`)), 'AudioFilename: audio.mp3\n');
    writeFileSync(long(path.join(set, 'audio.mp3')), 'x');
    writeFileSync(path.join(root, 'bundle.json'), '{}');

    shorten(root);
    const sets = readdirSync(path.join(root, 'runtime', 'osu', 'Songs'));
    assert.equal(sets.length, 1);
    assert.ok(sets[0].length <= 42 && /^An Extremely Long Artist Name ~[0-9a-f]{8}$/.test(sets[0]), sets[0]);
    const files = readdirSync(path.join(root, 'runtime', 'osu', 'Songs', sets[0])).sort();
    assert.equal(files.length, 2);
    assert.ok(files.includes('audio.mp3'), 'files referenced from a definition keep their names');
    const definition = files.find(f => f.endsWith('.osu'));
    assert.ok(definition.length <= 44, definition);

    shorten(root); // a second pass changes nothing
    assert.deepEqual(readdirSync(path.join(root, 'runtime', 'osu', 'Songs')), sets);
  } finally {
    rmSync(long(root), { recursive: true, force: true });
  }
});

test('a path that cannot be shortened fails the build', { skip: !windows }, () => {
  const root = mkdtempSync(path.join(tmpdir(), 'bundle-paths-'));
  try {
    const dir = path.join(root, 'runtime', 'x64', 'a'.repeat(60), 'b'.repeat(60));
    mkdirSync(dir, { recursive: true });
    writeFileSync(path.join(dir, 'file.dll'), 'x');
    assert.throws(() => shorten(root), /longer than 140 characters/);
    assert.ok(existsSync(path.join(dir, 'file.dll')), 'nothing outside the media sets is renamed');
  } finally {
    rmSync(long(root), { recursive: true, force: true });
  }
});
