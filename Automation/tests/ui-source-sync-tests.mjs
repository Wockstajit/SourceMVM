import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import assert from 'node:assert/strict';
import { publishExtraction, validateApply, commitApplications } from '../lib/ui-source-sync.mjs';
import { createPreviewServer, watchSources } from '../ui/preview.mjs';
import { extractAll } from '../../UiMockup/extract-ui.mjs';
import { applySurface } from '../../UiMockup/apply-ui.mjs';
import { extract } from '../../UiMockupDeadlock/extract-deadlock-ui.mjs';
import { apply } from '../../UiMockupDeadlock/apply-deadlock-ui.mjs';
import { tokenize as cs2Tokens } from '../../UiMockup/lib/panorama-headers.mjs';
import { tokenize as deadlockTokens } from '../../UiMockupDeadlock/lib/deadlock-macros.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const quiet = () => {};
function temp(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-ui-sync-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

for (const game of ['cs2', 'deadlock']) {
  function fixture(t) {
    const root = temp(t), pano = path.join(root, 'pano'), gen = path.join(root, 'generated');
    fs.mkdirSync(pano);
    const source = path.join(ROOT, 'AfxHookSource2', 'Filmmaker', game === 'cs2' ? 'Panorama' : 'Deadlock');
    for (const file of fs.readdirSync(source)) if (/\.(h|inc|cpp)$/.test(file)) fs.copyFileSync(path.join(source, file), path.join(pano, file));
    // CS2 extraction includes the sibling PlayMenu headers as well as Panorama.
    if (game === 'cs2') fs.cpSync(path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'PlayMenu'),
      path.join(root, 'PlayMenu'), { recursive: true });
    const refresh = () => game === 'cs2' ? extractAll(pano, gen, quiet)
      : extract(pano, path.join(pano, 'DeadlockWorkspaces.cpp'), gen, quiet);
    const generated = path.join(gen, game === 'cs2' ? 'cameraeditor.js' : 'workspace.js');
    const file = game === 'cs2' ? 'CameraEditorJs.h' : 'DeadlockStyleJs.h';
    const marker = game === 'cs2' ? `/* @mvm:seg ${file}#0 */\n` : `/* @mvm:seg ${file}#DEADLOCK_STYLE_JS#0 */\n`;
    const applyDraft = () => game === 'cs2' ? applySurface('cameraeditor', { panoDir: pano, genDir: gen, log: quiet }) : apply(pano, generated);
    refresh();
    const code = fs.readFileSync(generated, 'utf8');
    return { pano, gen, file, generated, code, refresh, applyDraft,
      edit: () => fs.writeFileSync(generated, code.replace(marker, marker + '// reviewed draft\n')) };
  }

  test(`${game}: a preview draft applies once to the source and survives another extraction`, (t) => {
    const f = fixture(t);
    f.edit(); f.applyDraft();
    assert.match(fs.readFileSync(path.join(f.pano, f.file), 'utf8'), /reviewed draft/);
    f.refresh(); f.applyDraft();
    assert.equal((fs.readFileSync(f.generated, 'utf8').match(/reviewed draft/g) || []).length, 1);
  });

  test(`${game}: reopening/refreshing cannot overwrite an unapplied draft`, (t) => {
    const f = fixture(t); f.edit();
    const draft = fs.readFileSync(f.generated, 'utf8');
    assert.throws(f.refresh, /unapplied mockup edits/);
    assert.equal(fs.readFileSync(f.generated, 'utf8'), draft);
  });

  test(`${game}: same-count header edits reject a stale draft without writing any header`, (t) => {
    const f = fixture(t); f.edit();
    fs.appendFileSync(path.join(f.pano, f.file), '\n// newer source change\n');
    const before = new Map(fs.readdirSync(f.pano).map((p) => [p, fs.readFileSync(path.join(f.pano, p))]));
    assert.throws(f.applyDraft, /changed since extraction/);
    for (const [p, bytes] of before) assert.deepEqual(fs.readFileSync(path.join(f.pano, p)), bytes, p);
  });

  test(`${game}: source-only edits refresh the preview; removing markers cannot drop code`, (t) => {
    const f = fixture(t), full = path.join(f.pano, f.file);
    const src = fs.readFileSync(full, 'utf8');
    const at = game === 'cs2' ? cs2Tokens(src).find((x) => x.type === 'raw').bodyStart
      : deadlockTokens(src)[0].literals[0].bodyStart;
    fs.writeFileSync(full, src.slice(0, at) + '// source save\n' + src.slice(at));
    f.refresh();
    assert.ok(fs.readFileSync(f.generated, 'utf8').includes('source save'));
    const code = fs.readFileSync(f.generated, 'utf8');
    fs.writeFileSync(f.generated, code.replace(/\/\* @mvm:seg [^\n]+ \*\/\n/, ''));
    assert.throws(f.applyDraft, /marker|segment|literal|raw-string/);
  });

  test(`${game}: code outside source segments is rejected instead of silently omitted`, (t) => {
    const f = fixture(t);
    fs.writeFileSync(f.generated, 'var lostEdit = true;\n' + f.code);
    assert.throws(f.applyDraft, /outside source segments/);
  });
}

test('legacy extractions are backed up before their first source refresh', (t) => {
  const dir = temp(t);
  fs.writeFileSync(path.join(dir, 'workspace.js'), 'old untracked draft');
  publishExtraction(dir, [{ name: 'workspace.js', code: 'fresh source', sources: [] }], quiet);
  const backup = fs.readdirSync(dir).find((name) => name.startsWith('backup-before-sync-'));
  assert.equal(fs.readFileSync(path.join(dir, backup, 'workspace.js'), 'utf8'), 'old untracked draft');
});

test('a changed file in the last apply plan prevents writing the first plan', (t) => {
  const dir = temp(t), plans = [];
  for (const id of ['One', 'Two']) {
    const full = path.join(dir, `${id}Js.h`), generated = path.join(dir, `${id}.js`);
    const code = `/* @mvm:seg ${id}Js.h#0 */\nold`;
    fs.writeFileSync(full, 'old');
    publishExtraction(dir, [{ name: `${id}.js`, code, sources: [{ path: full, before: 'old' }] }], quiet);
    const edited = code.replace('old', 'new');
    fs.writeFileSync(generated, edited);
    plans.push(validateApply(generated, edited, [{ path: full, before: 'old', after: 'new' }]));
  }
  fs.writeFileSync(plans[1].sources[0].path, 'concurrent edit');
  assert.throws(() => commitApplications(plans), /changed during apply/);
  assert.equal(fs.readFileSync(plans[0].sources[0].path, 'utf8'), 'old');
});

test('preview HEAD uses a content hash: same-length saves change its reload token', async (t) => {
  const dir = temp(t), file = path.join(dir, 'index.html');
  fs.writeFileSync(file, 'first');
  const server = createPreviewServer(dir);
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  t.after(() => new Promise((r) => { server.close(r); server.closeAllConnections(); }));
  const url = `http://127.0.0.1:${server.address().port}`;
  const before = await fetch(url, { method: 'HEAD' });
  fs.writeFileSync(file, 'other');
  const after = await fetch(url, { method: 'HEAD' });
  assert.notEqual(before.headers.get('etag'), after.headers.get('etag'));
  assert.equal(after.headers.get('cache-control'), 'no-store');
  assert.equal(await after.text(), '');
  assert.equal(await (await fetch(url)).text(), 'other');
  assert.equal((await fetch(url, { method: 'POST' })).status, 405);
  assert.equal((await fetch(url + '/%2e%2e%5Csecret')).status, 403);
});

test('source watcher keeps working after a conflict is resolved', async (t) => {
  let version = 0, conflict = false, refreshes = 0;
  const messages = [];
  const stop = watchSources({ stamp: () => version, interval: 10, log: (s) => messages.push(s),
    refresh: () => { if (conflict) throw new Error('draft conflict'); refreshes++; } });
  t.after(stop);
  const until = async (condition) => {
    for (let i = 0; i < 100 && !condition(); i++) await new Promise((r) => setTimeout(r, 10));
    assert.ok(condition());
  };
  assert.equal(refreshes, 1);
  conflict = true; version++;
  await until(() => messages.some((s) => s.includes('draft conflict')));
  assert.equal(refreshes, 1);
  conflict = false; version++;
  await until(() => refreshes === 2);
});
