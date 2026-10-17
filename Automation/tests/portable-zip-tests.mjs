import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

const read = path => readFileSync(new URL(`../../${path}`, import.meta.url), 'utf8');
const script = read('SourceMovieMakerLauncher/scripts/build-portable-zip.ps1');
const main = read('SourceMovieMakerLauncher/electron/main.cjs');
const bundled = read('SourceMovieMakerLauncher/electron-builder.bundled.cjs');
const updateService = read('SourceMovieMakerLauncher/electron/update-service.cjs');
const pkg = JSON.parse(read('SourceMovieMakerLauncher/package.json'));

test('the portable zip is built from the complete unpacked app and carries the portable flag', () => {
  assert.match(script, /win-unpacked/);
  assert.match(script, /resources\/bundle/);
  assert.match(script, /CreateEntry\('portable\.flag'\)/);
  assert.match(script, /Replace\('\\', '\/'\)/, 'entry names must use forward slashes');
  assert.doesNotMatch(script, /CreateFromDirectory\(/);
  assert.match(script, /SourceMovieMaker-Portable-\$version\.zip/);
  assert.match(pkg.scripts['package:portable-zip'], /build-portable-zip\.ps1/);
});
test('the launcher keeps its data beside the exe when portable.flag is present', () => {
  assert.match(main, /portable\.flag/);
  assert.match(main, /SourceMovieMaker-data/);
  assert.match(main, /mkdirSync\(dataDir, \{ recursive: true \}\);\s*app\.setPath\('userData', dataDir\)/, 'the data folder must exist before setPath');
});
test('the zip stays out of the updater feed: the complete build only emits the nsis installer', () => {
  assert.match(bundled, /target: \[\{ target: 'nsis', arch: \['x64'\] \}\]/);
});
test('a portable launcher is told to download a new zip or setup, and never self-updates', () => {
  assert.match(updateService, /if \(portable\) throw new Error\([^)]*portable zip/);
});
test('the portable zip is never a public release asset', () => {
  // It contains DMCA-restricted files. The installer is published; the zip is local or private only.
  const publish = read('Docs/setup/publishing-releases.md');
  const start = publish.indexOf('upload only these files');
  const end = publish.indexOf('Never upload the portable zip');
  assert.ok(start >= 0, 'the publish list must exist');
  assert.ok(end > start, 'the portable-zip rule must exist and follow the publish list');
  const uploads = publish.slice(start, end);
  assert.doesNotMatch(uploads, /Portable/);
  assert.doesNotMatch(script, /upload it as a normal release asset/);
  assert.match(script, /Do not upload it to a public release/);
});
