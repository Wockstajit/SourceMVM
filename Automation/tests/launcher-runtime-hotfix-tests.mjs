import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { test } from 'node:test';
const require = createRequire(import.meta.url);
const { pickHotfix, bundleRuntimeWanted, HOTFIX_SCHEMA } =
  require('../../SourceMovieMakerLauncher/electron/runtime-hotfix.cjs');

const sha = 'a'.repeat(64);
const manifest = (over = {}) => ({ schema: HOTFIX_SCHEMA, appVersion: '0.1.9', serial: 2,
  runtime: { asset: 'source-mvm-runtime-0.1.9-hotfix2.zip', url: 'https://github.com/x/y/z.zip', bytes: 5, sha256: sha }, ...over });

test('a newer serial for this app version is installed; the same or older one is not', () => {
  const picked = pickHotfix(manifest(), '0.1.9', 1);
  assert.equal(picked.serial, 2);
  assert.equal(picked.version, '0.1.9-hotfix2');
  assert.equal(pickHotfix(manifest(), '0.1.9', 2), null);
  assert.equal(pickHotfix(manifest(), '0.1.9', 3), null);
  assert.equal(pickHotfix(manifest(), '0.1.9', undefined).serial, 2);
});

test('a hotfix built for another app version is never applied', () => {
  assert.equal(pickHotfix(manifest(), '0.1.8', 0), null);
  assert.equal(pickHotfix(manifest({ appVersion: '0.1.10' }), '0.1.9', 0), null);
});

test('malformed manifests are refused instead of installing something unverified', () => {
  assert.throws(() => pickHotfix(null, '0.1.9', 0));
  assert.throws(() => pickHotfix(manifest({ schema: 99 }), '0.1.9', 0), /schema/);
  assert.throws(() => pickHotfix(manifest({ serial: 0 }), '0.1.9', 0), /serial/);
  assert.throws(() => pickHotfix(manifest({ runtime: { url: 'http://x/z.zip', sha256: sha } }), '0.1.9', 0), /https/);
  assert.throws(() => pickHotfix(manifest({ runtime: { url: 'https://x/z.zip', sha256: 'nope' } }), '0.1.9', 0), /sha256/);
});

test('a hotfixed runtime survives restarts; a newer bundle (app update) replaces it', () => {
  const hotfixed = { sha256: 'b'.repeat(64), bundleStamp: 'bundle:abc', hotfix: 2 };
  assert.equal(bundleRuntimeWanted(hotfixed, 'bundle:abc', true), false);
  assert.equal(bundleRuntimeWanted(hotfixed, 'bundle:def', true), true);
  assert.equal(bundleRuntimeWanted(hotfixed, 'bundle:abc', false), true);
  assert.equal(bundleRuntimeWanted(undefined, 'bundle:abc', true), true);
});
