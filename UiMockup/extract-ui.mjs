#!/usr/bin/env node
// Pull the Panorama JS bodies out of the C++ headers so the browser mockup runs the REAL in-game
// UI code instead of a hand-written copy that would drift the day after it was written.
//
// Each generated file is the exact string the C++ hands to CUIEngine::RunScript for that surface
// (raw-string literals concatenated, #include'd fragments spliced in place), with one addition: a
// comment marker at the start of every literal, `/* @mvm:seg File.h#n */`. Those markers are what
// lets an edit made in generated/ travel BACK into the header it came from -- see apply-ui.mjs.
// They are comments, so the code runs unchanged.
//
// Run:  node UiMockup/extract-ui.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { flatten, unflatten } from './lib/panorama-headers.mjs';
import { publishExtraction, writeIfChanged } from '../Automation/lib/ui-source-sync.mjs';
import { prepareMenu } from './prepare-menu.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PANO = path.resolve(HERE, '..', 'AfxHookSource2', 'Filmmaker', 'Panorama');
const OUT = path.join(HERE, 'generated');

// In-game surfaces (what the user actually sees while a demo is playing) come first; the two
// menu-context surfaces are kept but tagged so the picker can separate them.
// `cpp` names the HUD driver for the three scripts the engine builds ON DEMAND (BuildIfNeeded on
// enable, Teardown on exit). The extractor reads the root ids out of that driver's Teardown() so
// the browser tears the same panels down the game does, and cannot drift from it.
export const SURFACES = [
  { id: 'confighud',      file: 'ConfigHudJs.h',              api: 'ConfigHud',    cpp: 'ConfigHud.cpp', label: 'Config Panel',            group: 'ingame', note: 'CAMERA / EFFECTS / RAGDOLLS tabs, all sub-tabs, sliders, RESET footer' },
  { id: 'cameraeditor',   file: 'CameraEditorJs.h',           api: 'CamEditor',    cpp: 'CameraEditorHud.cpp', label: 'Camera Editor',           group: 'ingame', note: 'Workspace, inspectors, gear popup, Customize modal, Render workspace' },
  { id: 'cameratimeline', file: 'CameraTimelineJs.h',         api: 'CamTimeline',  label: 'Camera Timeline',         group: 'ingame', note: 'Transport, markers, render range, shake menu, speed scrubber; Loop stays on the Regular Timeline' },
  // Variants reuse another surface's script rather than re-extracting it: the Render workspace
  // and the Customize Player modal are both built by CameraEditorJs.h, but each is a place you go
  // to in its own right, so each gets its own row in the picker instead of hiding behind an action.
  { id: 'render',         variantOf: 'cameraeditor',          api: 'CamEditor',    label: 'Render Workspace',        group: 'ingame', note: 'Range, output, passes, START RENDER, and the running-job state' },
  { id: 'keyeditor',      variantOf: 'cameraeditor',          api: 'CamEditor',    label: 'Key Editor',              group: 'ingame', note: 'Selected camera key: transform, lens, curve, speed and shortcuts' },
  { id: 'customize',      variantOf: 'cameraeditor',          api: 'CamEditor',    label: 'Customize Player',        group: 'ingame', note: 'Agent, weapon skin, knife and glove pickers for the selected player' },
  { id: 'grapheditor',    file: 'GraphEditorJs.h',            api: 'GraphExp',     cpp: 'GraphEditorExperimentHud.cpp', label: 'Graph Editor',            group: 'ingame', note: 'Curve channels for the selected camera path' },
  { id: 'markerhud',      file: 'MarkerHudJs.h',              api: 'MarkerHud',    label: 'Marker HUD',              group: 'ingame', note: 'Camera-path marker overlay + guidance card' },
  { id: 'moviehud',       file: 'MovieHudJs.h',               api: 'MovieHud',     label: 'Movie HUD',               group: 'ingame', note: 'Reposition / edit-mode director card' },
  { id: 'demobar',        file: 'DemoBarButtonsJs.h',         api: null,           label: 'Demo Bar Buttons',        group: 'ingame', note: 'Buttons docked onto the native CS2 demo playback bar' },
  { id: 'aimcrosshair',   file: 'AimCrosshairHudJs.h',        api: 'AimXhair',     label: 'Aim Crosshair HUD',       group: 'ingame', note: 'Free-aim crosshair overlay' },
  { id: 'fxdebug',        file: 'FxDebugHudJs.h',             api: 'FxDebugHud',   label: 'FX Debug HUD',            group: 'ingame', note: 'Per-shot muzzle/tracer/smoke telemetry squares' },
  { id: 'eastereggs',     file: 'EasterEggOverlayJs.h',       api: 'MvmEggs',      label: 'Easter Eggs',             group: 'ingame', note: 'Gen X hints, E-girl sparkles, Sony Vegas intervention, Clippy' },
  { id: 'filmmakergui',   file: 'FilmmakerGuiJs.h',           api: 'Filmmaker',    label: 'Demos Tab (main menu)',   group: 'menu',   note: 'Main-menu demo library + organizer. Menu context, not in-game.' },
  { id: 'workshop', file: 'WorkshopFilterJs.h', files: ['PlayMenuStripJs.h', 'WorkshopFilterJs.h', 'WorkshopKeywordEditorJs.h'], sourceDir: '../PlayMenu', api: 'MvmPlay', label: 'Workshop Maps', group: 'menu', note: 'Native map grid with shared Workshop filters and keyword editor.' },
  { id: 'videoresolution',file: 'VideoSettingsResolutionJs.h',api: 'MvmResolutionSettings', label: 'Custom Resolution (video settings)', group: 'menu', note: 'Injected into CS2 Video settings. Menu context, not in-game.' },
];

// What the HUD driver's Teardown() deletes: every `$('#Root')` it names and the `$.Api=null` it
// ends with. Read out of the C++ rather than listed here, because the list has grown before
// (Customize and its confirm became their own roots) and a browser that left one behind would
// show a modal the game had torn down.
export function teardownOf(panoDir, cppFile) {
  const src = fs.readFileSync(path.join(panoDir, cppFile), 'utf8');
  const m = /::Teardown\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  if (!m) throw new Error(`${cppFile}: no ::Teardown() body found`);
  const body = m[1];
  const roots = [...body.matchAll(/\$\('#([A-Za-z0-9_]+)'\)/g)].map((x) => x[1]);
  const api = (/\$\.([A-Za-z0-9_]+)\s*=\s*null/.exec(body) || [])[1] || null;
  if (!roots.length || !api) throw new Error(`${cppFile}: Teardown() names no roots / no $.Api=null`);
  return { roots: [...new Set(roots)], api };
}

export function extractAll(panoDir = PANO, outDir = OUT, log = console.log) {
  fs.mkdirSync(outDir, { recursive: true });
  const manifest = [];
  const entries = [];
  let totalLines = 0;
  for (const s of SURFACES) {
    if (s.variantOf) {
      manifest.push({ ...s, lines: 0, src: `generated/${s.variantOf}.js` });
      log(`     (view)  ${s.id.padEnd(16)} <- ${s.variantOf}`);
      continue;
    }
    const sourceDir = s.sourceDir ? path.resolve(panoDir, s.sourceDir) : panoDir;
    const code = (s.files || [s.file]).map(file => flatten(sourceDir, file)).join('');
    const lines = code.split('\n').length;
    totalLines += lines;
    entries.push({ name: `${s.id}.js`, code: code + `\n//# sourceURL=mvm/${s.id}.js\n`,
      sources: Object.keys(unflatten(code)).map((file) => ({ path: path.join(sourceDir, file),
        before: fs.readFileSync(path.join(sourceDir, file), 'utf8') })) });
    const entry = { ...s, lines, src: `generated/${s.id}.js` };
    if (s.cpp) {
      entry.teardown = teardownOf(panoDir, s.cpp);
      if (entry.teardown.api !== s.api) {
        throw new Error(`${s.id}: Teardown() nulls $.${entry.teardown.api} but the surface registers $.${s.api}`);
      }
    }
    manifest.push(entry);
    log(`  ${String(lines).padStart(6)} lines  ${s.id.padEnd(16)} <- ${s.file}${entry.teardown ? `  (tears down ${entry.teardown.roots.length} roots)` : ''}`);
  }
  publishExtraction(outDir, entries, log);
  prepareMenu(outDir);
  writeIfChanged(path.join(outDir, 'manifest.json'), JSON.stringify(manifest, null, 2));
  log(`\n${manifest.length} surfaces, ${totalLines} lines -> ${path.relative(process.cwd(), outDir) || '.'}`);
  return manifest;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  extractAll();
}
