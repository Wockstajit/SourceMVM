#!/usr/bin/env node
// Put edited Panorama JS from UiMockup/generated/ BACK into the C++ headers it was extracted
// from, so a change made and checked in the browser becomes the same change in the mod.
//
//   node UiMockup/apply-ui.mjs cameraeditor          # write CameraEditorJs.h (+ its includes)
//   node UiMockup/apply-ui.mjs                       # every surface
//   node UiMockup/apply-ui.mjs --dry-run             # say what would change, touch nothing
//   node UiMockup/apply-ui.mjs --check               # exit 1 if generated/ and the headers differ
//
// How: extract-ui.mjs stamps every raw-string literal with `/* @mvm:seg File.h#n */`. This splits
// the generated file on those markers, and for each header rewrites literal n in place, leaving
// every byte of C++ around it alone and restoring the header's own line endings (the tree is mixed
// CRLF/LF on purpose -- see CLAUDE.md). It refuses to write a header whose literal count no longer
// matches, and refuses a body that would terminate the literal early, because either one would
// compile into something other than what was reviewed in the browser.
//
// After applying: fast hook-only rebuild + install (see CLAUDE.md "Build"), then relaunch CS2. The
// in-game HUDs cache their root panel when they build, so the edited script cannot be hot-loaded
// over a running one -- it needs the rebuild.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { tokenize, unflatten, newlineOf } from './lib/panorama-headers.mjs';
import { SURFACES } from './extract-ui.mjs';
import { validateApply, commitApplications } from '../Automation/lib/ui-source-sync.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_PANO = path.resolve(HERE, '..', 'AfxHookSource2', 'Filmmaker', 'Panorama');
const DEFAULT_GEN = path.join(HERE, 'generated');

// Rewrite one header's literals. Returns { changed, segments, before, after }.
export function applyToHeader(headerPath, bodies, file) {
  const src = fs.readFileSync(headerPath, 'utf8');
  const nl = newlineOf(src);
  const raws = tokenize(src, file).filter((t) => t.type === 'raw');
  if (raws.length !== bodies.length || Array.from(bodies).some((b) => b === undefined)) {
    throw new Error(`${file}: header has ${raws.length} raw-string literal(s) but generated/ carries ` +
      `${bodies.filter((b) => b !== undefined).length} of ${bodies.length} segment(s). ` +
      `Re-run extract-ui.mjs -- the header changed underneath the mockup.`);
  }
  let out = '';
  let cursor = 0;
  let changed = 0;
  raws.forEach((t, n) => {
    const closer = ')' + t.tag + '"';
    const old = src.slice(t.bodyStart, t.bodyEnd);
    const lf = (x) => x.replace(/\r\n/g, '\n');
    let body;
    if (lf(bodies[n]) === lf(old)) {
      // Unchanged content keeps its exact bytes. The tree mixes line endings even INSIDE one header,
      // and re-emitting an untouched literal with a uniform ending rewrote 178 lines of two files
      // the first time this ran -- a diff that had nothing to do with any edit.
      body = old;
    } else {
      // A changed literal takes the ending the literal itself was using, falling back to the file's.
      const nlHere = old.includes('\r\n') ? '\r\n' : (old.includes('\n') ? '\n' : nl);
      body = lf(bodies[n]).replace(/\n/g, nlHere);
      changed++;
    }
    if (body.includes(closer)) {
      throw new Error(`${file}#${n}: the edited body contains ${closer}, which would end the C++ literal early.`);
    }
    if (Buffer.byteLength(body, 'utf8') > 16380) {
      throw new Error(`${file}#${n}: edited literal exceeds MSVC's 16380-byte limit; split it at a literal seam.`);
    }
    out += src.slice(cursor, t.bodyStart) + body;
    cursor = t.bodyEnd;
  });
  out += src.slice(cursor);
  return { changed, segments: raws.length, before: src, after: out };
}

export function applySurface(id, { panoDir = DEFAULT_PANO, genDir = DEFAULT_GEN, write = true, log = console.log, planOnly = false } = {}) {
  const s = SURFACES.find((x) => x.id === id);
  if (!s) throw new Error(`unknown surface: ${id}`);
  if (s.variantOf) return { id, skipped: `view of ${s.variantOf}` };
  const jsPath = path.join(genDir, `${id}.js`);
  if (!fs.existsSync(jsPath)) throw new Error(`${jsPath} does not exist -- run extract-ui.mjs first`);
  const code = fs.readFileSync(jsPath, 'utf8');
  const bodies = unflatten(code);
  const sourceDir = s.sourceDir ? path.resolve(panoDir, s.sourceDir) : panoDir;
  const files = Object.keys(bodies);
  if (!files.length) throw new Error(`${id}.js has no @mvm:seg markers -- it predates the round trip; re-run extract-ui.mjs`);

  const results = [];
  const sources = [];
  for (const file of files) {
    const r = applyToHeader(path.join(sourceDir, file), bodies[file], file);
    results.push({ file, changed: r.changed, segments: r.segments });
    sources.push({ path: path.join(sourceDir, file), before: r.before, after: r.after });
    log(`  ${r.changed ? 'would write' : 'same  '} ${file.padEnd(36)} ${r.changed}/${r.segments} literal(s) differ`);
  }
  const plan = validateApply(jsPath, code, sources);
  if (write && !planOnly) commitApplications([plan]);
  return { id, files: results, changed: results.reduce((a, r) => a + r.changed, 0), plan };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const argv = process.argv.slice(2);
  const dry = argv.includes('--dry-run');
  const check = argv.includes('--check');
  let panoDir = DEFAULT_PANO, genDir = DEFAULT_GEN;
  const ids = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--pano') panoDir = path.resolve(argv[++i]);
    else if (argv[i] === '--gen') genDir = path.resolve(argv[++i]);
    else if (!argv[i].startsWith('--')) ids.push(argv[i]);
  }
  const targets = ids.length ? ids : SURFACES.filter((s) => !s.variantOf).map((s) => s.id);
  let total = 0, failed = 0;
  const plans = [];
  for (const id of targets) {
    console.log(`${id}:`);
    try {
      const r = applySurface(id, { panoDir, genDir, write: false, planOnly: true });
      if (r.skipped) { console.log(`  (${r.skipped})`); continue; }
      total += r.changed;
      plans.push(r.plan);
    } catch (e) {
      failed++;
      console.error(`  ERROR ${e.message}`);
    }
  }
  if (failed) { console.error('\nApply stopped; no headers were written.'); process.exit(1); }
  if (!dry && !check) {
    try { commitApplications(plans); }
    catch (e) { console.error(e.message); process.exit(1); }
  }
  if (check) {
    console.log(total ? `\n${total} literal(s) differ between generated/ and the headers` : '\ngenerated/ and the headers agree');
    process.exit(total || failed ? 1 : 0);
  }
  console.log(dry ? `\n${total} literal(s) would change` : `\n${total} literal(s) written${total ? ' -- rebuild the hook to see it in game' : ''}`);
  process.exit(failed ? 1 : 0);
}
