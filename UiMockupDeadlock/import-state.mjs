#!/usr/bin/env node
// Turn a captured console dump of the game's state feed into the snapshot the mockup renders from.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-stash-state.js
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-state.js   (> some file)
//   node UiMockupDeadlock/import-state.mjs <that file>
//
// WHY. A comparison against a hand-written state feed mostly measures the stand-in, not the UI: the
// first geometry diff against the game diverged at panel 58 because the mockup's stand-in had three
// camera markers and the game had none, and every panel after the marker list was out of step. With
// the game's own state loaded, the two trees are built from the same numbers and a difference is a
// difference in the UI code's behaviour between the two engines -- which is the only kind worth
// chasing.
//
// The mockup loads generated/state-snapshot.json automatically when it exists (`?live=0` in the URL
// falls back to the built-in stand-in). The snapshot is the render() ARGUMENT verbatim, so the
// projection in shim/state.js is bypassed for everything it carries; the model still drives the
// clock and the commands, and its fields are used only where the snapshot has none.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, 'generated', 'state-snapshot.json');

const file = process.argv[2];
if (!file) {
  console.error('Usage: node UiMockupDeadlock/import-state.mjs <captured console output>');
  process.exit(2);
}

const TAB = '\t';
const chunks = [];
let expected = -1;
let dial = null;
for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
  if (raw.startsWith('STATEEND' + TAB)) expected = Number(raw.split(TAB)[1]);
  else if (raw.startsWith('STATE' + TAB)) {
    const p = raw.split(TAB);
    if (p[1] === 'none' || p[1] === 'error') {
      console.error(`the probe reported: ${p.slice(1).join(' ')}`);
      process.exit(1);
    }
    chunks[Number(p[1])] = p.slice(2).join(TAB);
  } else if (raw.startsWith('DIAL' + TAB)) dial = raw.slice(5);
}

if (!chunks.length) {
  console.error(`No STATE lines in ${file}. Load deadlock-stash-state.js, wait a frame, then deadlock-dump-ui-state.js.`);
  process.exit(1);
}
if (expected >= 0 && chunks.length !== expected) {
  console.error(`Expected ${expected} chunks, found ${chunks.length} -- the capture is incomplete.`);
  process.exit(1);
}
for (let i = 0; i < chunks.length; i++) {
  if (chunks[i] === undefined) { console.error(`chunk ${i} is missing`); process.exit(1); }
}

let state;
try {
  state = JSON.parse(chunks.join(''));
} catch (e) {
  console.error(`The joined state is not valid JSON: ${e.message}`);
  process.exit(1);
}

// The cursor is wherever the operator's mouse happened to be, which is usually on another monitor
// while a capture is being taken. It is not part of the layout, and a stale off-screen cursor would
// keep every hover state in the mockup off; the model supplies a live one instead.
delete state.mx; delete state.my; delete state.lmb; delete state.rmb; delete state.shift;

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify({
  capturedAt: new Date().toISOString(),
  source: path.basename(file),
  dial: dial ? (() => { try { return JSON.parse(dial); } catch { return dial; } })() : null,
  state,
}, null, 1), 'utf8');

const L = state.layout || {};
console.log(`snapshot -> ${path.relative(process.cwd(), OUT)}`);
console.log(`  workspace=${state.workspace} open=${state.open} dock=${state.dock && state.dock.tab} ` +
            `markers=${state.markers} tick=${state.tick} paused=${state.paused}`);
console.log(`  layout: screen ${L.screenW}x${L.screenH} column ${L.inspectorW} dock ${L.dockH} ` +
            `preview ${L.previewX},${L.previewY} ${L.previewW}x${L.previewH}`);
if (dial) console.log(`  dial probe: ${dial}`);
