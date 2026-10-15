#!/usr/bin/env node
// Pull the Deadlock workspace's Panorama JS out of the C++ macros so the browser mockup runs the
// REAL in-game UI code instead of a hand-written copy that would drift the day after it was written.
//
// The output is ONE file, generated/workspace.js, and that is not a simplification -- it is what the
// game does. DeadlockWorkspaces.cpp pushes twelve macros through RunScript back to back into a
// single shared JavaScript scope, so the concatenation IS the surface. A fragment extracted on its
// own would not run: they call each other's helpers.
//
// Every literal is preceded by `/* @mvm:seg File.h#MACRO#n */`. Those markers are comments, so the
// code runs unchanged, and they are what lets an edit made in generated/ travel back into the header
// it came from -- see apply-deadlock-ui.mjs.
//
// Run:  node UiMockupDeadlock/extract-deadlock-ui.mjs

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { flattenMacro, fragMarker } from './lib/deadlock-macros.mjs';
import { fragments } from './lib/fragments.mjs';
import { bindings } from './lib/hotkeys.mjs';
import { publishExtraction, writeIfChanged } from '../Automation/lib/ui-source-sync.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const PANO = path.resolve(HERE, '..', 'AfxHookSource2', 'Filmmaker', 'Deadlock');
export const DRIVER = path.join(PANO, 'DeadlockWorkspaces.cpp');
// The hotkey table the hook polls (DeadlockHotkeys.cpp g_bindings[]), so the browser feeds the same
// keys to the same gate.
export const HOTKEYS = path.resolve(HERE, '..', 'AfxHookSource2', 'Deadlock', 'DeadlockHotkeys.cpp');
const OUT = path.join(HERE, 'generated');

// The VIEWS of the one surface. Deadlock's workspace has two independent axes -- which workspace is
// open (the right-hand inspector) and which editor is docked along the bottom -- and the picker
// offers the combinations worth looking at rather than pretending each is a separate panel. This
// mirrors how the game is actually driven: two console commands, not nine panels.
export const VIEWS = [
  { id: 'camera-native', label: 'Camera Editor + Regular Timeline', workspace: 'camera', tab: 'native',
    note: "Right column is the camera inspector; the dock hosts Deadlock's own replay bar." },
  { id: 'camera-timeline', label: 'Camera Editor + Camera Timeline', workspace: 'camera', tab: 'camera',
    note: 'Transport, clock ruler, marker track and marker actions in a 208px dock.' },
  { id: 'camera-graph', label: 'Camera Editor + Graph', workspace: 'camera', tab: 'graph',
    note: 'The curve editor docked at 422px: per-channel curves, ruler, keyframes.' },
  { id: 'config', label: 'Config', workspace: 'config', tab: 'native',
    note: 'The Config inspector with the replay bar still docked.' },
  { id: 'render', label: 'Render', workspace: 'render', tab: 'camera',
    note: 'The render inspector over the camera timeline, which is where a range is set.' },
  // THE BOOT STATE, and what the game shows after a replay loads: nothing of ours open, the
  // game's own replay bar with our Cam Editor / Config / Render / Loop buttons on it, and the
  // director card. Every workspace is reached from here by clicking, as in game.
  { id: 'closed', label: 'Replay only (nothing open)', workspace: 'off', tab: 'native',
    note: "Deadlock's replay bar with Cam Editor / Config / Render / Loop, and the director card. The default." },
];

export function extract(panoDir = PANO, driverPath = DRIVER, outDir = OUT, log = console.log, hotkeysPath = HOTKEYS) {
  fs.mkdirSync(outDir, { recursive: true });

  const frags = fragments(panoDir, driverPath);
  const parts = [];
  let literals = 0;

  for (const f of frags) {
    parts.push(fragMarker(f.file, f.macro));
    const body = flattenMacro(panoDir, f.file, f.macro);
    parts.push(body);
    // NOTHING between fragments, deliberately. A separator here would be indistinguishable from the
    // literal's own content when the applier reads the generated file back -- everything between one
    // marker and the next IS the body -- so every fragment would come back one newline longer than
    // the header it was taken from and the round trip would never be exact.
    //
    // It is not needed either: the marker is a BLOCK comment, so even a literal that ends without a
    // trailing newline joins cleanly onto the next fragment. RunScript evaluates each fragment as
    // its own program and every fragment is already an IIFE, so concatenating them is equivalent.
    const n = (body.match(/@mvm:seg /g) || []).length;
    literals += n;
    log(`  ${String(body.split('\n').length).padStart(6)} lines  ${f.macro.padEnd(32)} <- ${f.file}${n > 1 ? `  (${n} literals)` : ''}`);
  }

  const code = parts.join('') + '//# sourceURL=mvm-deadlock/workspace.js\n';

  const manifest = {
    surface: 'workspace',
    src: 'generated/workspace.js',
    fragments: frags,
    views: VIEWS,
    // The hook's own key table, so app.js can feed the browser's keys through the same gate.
    hotkeys: fs.existsSync(hotkeysPath) ? bindings(hotkeysPath) : [],
    lines: code.split('\n').length,
    literals,
  };
  publishExtraction(outDir, [{ name: 'workspace.js', code,
    sources: [...new Set(frags.map((f) => f.file))].map((file) => ({ path: path.join(panoDir, file),
      before: fs.readFileSync(path.join(panoDir, file), 'utf8') })) }], log);
  writeIfChanged(path.join(outDir, 'manifest.json'), JSON.stringify(manifest, null, 2));

  log(`\n${frags.length} fragments, ${literals} literals, ${manifest.lines} lines -> ` +
      `${path.relative(process.cwd(), outDir) || '.'}/workspace.js`);
  return manifest;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  extract();
}
