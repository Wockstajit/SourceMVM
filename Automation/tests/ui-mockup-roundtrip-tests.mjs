// The browser mockup's round trip: header -> generated JS -> header must be byte-exact for every
// surface, and an edit made in the generated JS must land in the right literal of the right header
// and nowhere else. This is what makes "change it in the HTML, then port it to the mod" a tool
// rather than a hope -- if this fails, apply-ui.mjs is rewriting bytes nobody edited.
//
//   node --test Automation/tests/ui-mockup-roundtrip-tests.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { flatten, unflatten, tokenize } from '../../UiMockup/lib/panorama-headers.mjs';
import { SURFACES } from '../../UiMockup/extract-ui.mjs';
import { applyToHeader } from '../../UiMockup/apply-ui.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const PANO = path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'Panorama');
const scripted = SURFACES.filter((s) => !s.variantOf);

function collectHeaders(file, seen = new Set(), sourceDir = PANO) {
  if (seen.has(file)) return seen;
  seen.add(file);
  const src = fs.readFileSync(path.join(sourceDir, file), 'utf8');
  for (const t of tokenize(src, file)) if (t.type === 'include') collectHeaders(t.name, seen, sourceDir);
  return seen;
}

test('every surface survives extract -> apply byte for byte', () => {
  for (const s of scripted) {
    const sourceDir = s.sourceDir ? path.resolve(PANO, s.sourceDir) : PANO;
    const bodies = unflatten((s.files || [s.file]).map(file => flatten(sourceDir, file)).join(''));
    const headers = new Set();
    for (const file of s.files || [s.file]) collectHeaders(file, headers, sourceDir);
    for (const file of headers) {
      const before = fs.readFileSync(path.join(sourceDir, file), 'utf8');
      assert.ok(bodies[file], `${s.id}: ${file} produced no segments`);
      const r = applyToHeader(path.join(sourceDir, file), bodies[file], file);
      assert.equal(r.changed, 0, `${s.id}: ${file} reports ${r.changed} changed literal(s) with no edit`);
      assert.equal(r.after, before, `${s.id}: ${file} would be rewritten with no edit`);
    }
  }
});

test('an edit in the generated JS lands in exactly one literal, with the header\'s own line endings', () => {
  const s = scripted.find((x) => x.id === 'cameraeditor');
  const js = flatten(PANO, s.file);
  const marker = '/* @mvm:seg CameraEditorJs.h#0 */\n';
  assert.ok(js.includes(marker), 'first literal is marked');
  const edited = js.replace(marker, marker + '/* mockup-roundtrip-test */\n');
  const bodies = unflatten(edited);

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-rt-'));
  const copy = path.join(tmp, 'CameraEditorJs.h');
  fs.copyFileSync(path.join(PANO, 'CameraEditorJs.h'), copy);
  const r = applyToHeader(copy, bodies['CameraEditorJs.h'], 'CameraEditorJs.h');
  assert.equal(r.changed, 1);
  assert.ok(r.after.includes('/* mockup-roundtrip-test */'));
  // Everything outside literal 0 is untouched: strip the inserted line and the file is identical.
  assert.equal(r.after.replace(/\/\* mockup-roundtrip-test \*\/\r?\n/, ''), r.before);
  fs.rmSync(tmp, { recursive: true, force: true });
});

test('a header whose literal count changed underneath the mockup is refused, not clobbered', () => {
  const bodies = unflatten(flatten(PANO, 'MovieHudJs.h'));
  const file = 'MovieHudJs.h';
  const tooFew = bodies[file].slice(0, -1);
  assert.throws(() => applyToHeader(path.join(PANO, file), tooFew, file), /Re-run extract-ui\.mjs/);
});

test('a body that would terminate the C++ literal early is refused', () => {
  const file = 'MovieHudJs.h';
  const src = fs.readFileSync(path.join(PANO, file), 'utf8');
  const tag = tokenize(src, file).find((t) => t.type === 'raw').tag;
  const bodies = unflatten(flatten(PANO, file));
  bodies[file][0] += `\n// )${tag}" sneaks out of the literal\n`;
  assert.throws(() => applyToHeader(path.join(PANO, file), bodies[file], file), /end the C\+\+ literal early/);
});
