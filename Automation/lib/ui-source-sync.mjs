// Shared file-safety for both mockups. This does not emulate either game's UI.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';

export const digest = (text) => createHash('sha256').update(text).digest('hex');
const statePath = (dir) => path.join(dir, '.source-sync.json');
const readState = (dir) => fs.existsSync(statePath(dir))
  ? JSON.parse(fs.readFileSync(statePath(dir), 'utf8')) : { version: 1, files: {} };
const structure = (code) => code.match(/\/\* (?:@mvm:seg|>>>|<<<) [^\r\n]*? \*\//g) || [];
function wrapperHash(code) {
  const marks = [...code.matchAll(/\/\* (?:@mvm:seg|>>>|<<<) [^\r\n]*? \*\//g)];
  const parts = [code.slice(0, marks[0]?.index ?? code.length)];
  marks.forEach((m, i) => {
    if (!m[0].startsWith('/* @mvm:seg')) {
      parts.push(code.slice(m.index + m[0].length, marks[i + 1]?.index ?? code.length));
    }
  });
  return digest(JSON.stringify(parts));
}

export function writeIfChanged(file, data) {
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === data) return false;
  fs.writeFileSync(file, data, 'utf8');
  return true;
}

function snapshot(code, sources) {
  return { generated: digest(code), structure: structure(code), wrapper: wrapperHash(code),
    sources: Object.fromEntries(sources.map((s) => [path.basename(s.path), digest(s.after ?? s.before)])) };
}

// Validate EVERY output before writing any. A source refresh must never erase a browser draft.
// Old extractions have no baseline: preserve them in a backup before establishing one.
export function publishExtraction(dir, entries, log = console.log) {
  fs.mkdirSync(dir, { recursive: true });
  const state = readState(dir);
  const legacy = [];
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (!fs.existsSync(full)) continue;
    const current = fs.readFileSync(full, 'utf8');
    if (current === e.code) continue;
    const previous = state.files[e.name];
    if (previous && digest(current) !== previous.generated) {
      throw new Error(`${e.name} has unapplied mockup edits. Extraction stopped without overwriting them. ` +
        'Apply the draft first; if the headers also changed, reconcile both edits before extracting.');
    }
    if (!previous) legacy.push({ name: e.name, current });
  }
  if (legacy.length) {
    const backup = fs.mkdtempSync(path.join(dir, 'backup-before-sync-'));
    for (const e of legacy) fs.writeFileSync(path.join(backup, e.name), e.current, 'utf8');
    log(`Preserved older preview files in ${backup}`);
  }
  for (const e of entries) {
    writeIfChanged(path.join(dir, e.name), e.code);
    state.files[e.name] = snapshot(e.code, e.sources);
  }
  writeIfChanged(statePath(dir), JSON.stringify(state, null, 2) + '\n');
}

// Called after literal validation, before committing. Hashes catch same-count source changes,
// which literal-count checks alone cannot distinguish from intentional edits in the mockup.
export function validateApply(generated, code, sources) {
  if (!sources.length) throw new Error(`${generated}: no source segments found`);
  const previous = readState(path.dirname(generated)).files[path.basename(generated)];
  const changes = sources.filter((s) => s.before !== s.after);
  if (previous) {
    if (JSON.stringify(structure(code)) !== JSON.stringify(previous.structure)) {
      throw new Error(`${generated}: source markers changed; keep their order and boundaries intact.`);
    }
    if (previous.wrapper && previous.wrapper !== wrapperHash(code)) {
      throw new Error(`${generated}: edits outside source segments cannot be applied; place UI code inside the marked literals.`);
    }
    for (const s of sources) {
      if (s.before !== s.after && previous.sources[path.basename(s.path)] !== digest(s.before)) {
        throw new Error(`${path.basename(s.path)} changed since extraction. Apply stopped: stale mockup ` +
          'code would overwrite newer source. Reconcile the draft with the header first.');
      }
    }
  } else if (changes.length) {
    throw new Error(`${generated}: no extraction baseline. Save a copy of your draft, run the extractor, ` +
      'then transfer your intended edits into the fresh preview before applying.');
  }
  return { generated, code, sources };
}

// Preflight the whole batch (including shared headers) so a later error leaves earlier files alone.
export function commitApplications(plans) {
  const files = new Map();
  for (const p of plans) {
    if (fs.readFileSync(p.generated, 'utf8') !== p.code) throw new Error(`${p.generated} changed during apply; retry.`);
    validateApply(p.generated, p.code, p.sources);
    for (const s of p.sources) {
      if (fs.readFileSync(s.path, 'utf8') !== s.before) throw new Error(`${s.path} changed during apply; retry.`);
      if (files.has(s.path) && files.get(s.path).after !== s.after) {
        throw new Error(`${s.path}: two mockup surfaces propose different edits to the same header.`);
      }
      files.set(s.path, s);
    }
  }
  for (const s of files.values()) if (s.before !== s.after) fs.writeFileSync(s.path, s.after, 'utf8');
  for (const p of plans) {
    const dir = path.dirname(p.generated), state = readState(dir);
    state.files[path.basename(p.generated)] = snapshot(p.code, p.sources);
    writeIfChanged(statePath(dir), JSON.stringify(state, null, 2) + '\n');
  }
}
