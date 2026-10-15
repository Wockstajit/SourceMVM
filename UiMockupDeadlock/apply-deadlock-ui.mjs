#!/usr/bin/env node
// Carry an edit made in generated/workspace.js back into the C++ macro it came from.
//
// The generated file is not a copy of the game's UI code; it IS the game's UI code, with a marker on
// every literal saying where it came from. So editing it in the browser and writing it back is a
// real round trip rather than a re-implementation, and the test proves it: extract -> apply leaves
// every header byte for byte unchanged.
//
//   node UiMockupDeadlock/apply-deadlock-ui.mjs              write the changes back
//   node UiMockupDeadlock/apply-deadlock-ui.mjs --dry-run    report what would change
//   node UiMockupDeadlock/apply-deadlock-ui.mjs --check      exit 1 if generated/ and the headers disagree
//
// What it refuses to do, because each of these would produce C++ that does not compile or a header
// that silently lost code:
//
//   * write a header whose literal count no longer matches the generated file (the header moved
//     underneath the extraction -- re-extract instead),
//   * write a body containing `)CFJS"`, which would end the raw literal early and spill the rest of
//     the JS into the C++ as syntax,
//   * write a literal over MSVC's 16380-byte raw-string cap, which the compiler TRUNCATES SILENTLY.
//     A panel that builds fine here and half-builds in game is exactly that, and it reads as "the
//     UI is just ugly" rather than as an error. Split the macro at a seam instead.
//
// Each header's own line endings are preserved: the tree is mixed CRLF and LF, sometimes inside one
// file, so the bodies are written back in whatever convention the file already used.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { tokenize, unflatten, newlineOf } from './lib/deadlock-macros.mjs';
import { PANO } from './extract-deadlock-ui.mjs';
import { validateApply, commitApplications } from '../Automation/lib/ui-source-sync.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const GENERATED = path.join(HERE, 'generated', 'workspace.js');

// MSVC's raw-string literal cap. Past it the compiler truncates without a diagnostic.
const MAX_LITERAL = 16380;

export function apply(panoDir = PANO, generated = GENERATED, { dryRun = false, check = false } = {}) {
  if (!fs.existsSync(generated)) {
    throw new Error(`${generated} does not exist -- run extract-deadlock-ui.mjs first`);
  }
  const code = fs.readFileSync(generated, 'utf8');
  const bodies = unflatten(code);
  const report = { changed: [], unchanged: [], files: 0 };
  const sources = [];

  for (const [file, macros] of Object.entries(bodies)) {
    const full = path.join(panoDir, file);
    let src = fs.readFileSync(full, 'utf8');
    const before = src;
    const nl = newlineOf(src);
    let edits = [];

    for (const [macroName, literals] of Object.entries(macros)) {
      const macro = tokenize(src, file).find((x) => x.name === macroName);
      if (!macro) throw new Error(`${file}: ${macroName} is gone -- re-extract`);
      if (macro.literals.length !== literals.length) {
        throw new Error(
          `${file}#${macroName}: the header has ${macro.literals.length} literals but the ` +
          `generated file has ${literals.length} -- the header moved, re-extract`
        );
      }
      macro.literals.forEach((lit, n) => {
        const body = literals[n];
        if (body === undefined) throw new Error(`${file}#${macroName}: literal ${n} missing`);
        if (body.includes(')CFJS"')) {
          throw new Error(`${file}#${macroName} literal ${n} contains )CFJS" -- it would end the raw string early`);
        }
        const current = src.slice(lit.bodyStart, lit.bodyEnd);
        const lf = (s) => s.replace(/\r\n/g, '\n');
        const literalNl = current.includes('\r\n') ? '\r\n' : current.includes('\n') ? '\n' : nl;
        const out = lf(body) === lf(current) ? current : lf(body).replace(/\n/g, literalNl);
        if (Buffer.byteLength(out, 'utf8') > MAX_LITERAL) {
          throw new Error(
            `${file}#${macroName} literal ${n} is ${Buffer.byteLength(out, 'utf8')} bytes, over MSVC's ` +
            `${MAX_LITERAL}-byte raw-string cap -- the compiler would truncate it silently. ` +
            'Split the macro with a )CFJS" R"CFJS( seam on one line.'
          );
        }
        if (current !== out) edits.push({ start: lit.bodyStart, end: lit.bodyEnd, text: out, macroName, n });
      });
    }

    report.files++;
    if (!edits.length) { report.unchanged.push(file); sources.push({ path: full, before, after: src }); continue; }

    // Back to front, so an earlier edit cannot move a later one's offsets.
    edits.sort((a, b) => b.start - a.start);
    for (const e of edits) src = src.slice(0, e.start) + e.text + src.slice(e.end);

    report.changed.push({ file, literals: edits.map((e) => `${e.macroName}#${e.n}`) });
    sources.push({ path: full, before, after: src });
  }
  const plan = validateApply(generated, code, sources);
  if (!dryRun && !check) commitApplications([plan]);
  return report;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const dryRun = process.argv.includes('--dry-run');
  const check = process.argv.includes('--check');
  let report;
  try {
    report = apply(PANO, GENERATED, { dryRun, check });
  } catch (e) {
    console.error(`  FAIL  ${e.message}`);
    process.exit(1);
  }
  for (const c of report.changed) {
    console.log(`  ${check || dryRun ? 'would write' : 'wrote'}  ${c.file}  (${c.literals.join(', ')})`);
  }
  console.log(`\n${report.changed.length} of ${report.files} headers ` +
              `${check || dryRun ? 'differ' : 'updated'}; ${report.unchanged.length} unchanged.`);
  if (check && report.changed.length) {
    console.error('\ngenerated/workspace.js and the headers disagree.');
    process.exit(1);
  }
}
