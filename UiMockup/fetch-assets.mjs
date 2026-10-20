#!/usr/bin/env node
// Extract the CS2 UI assets the Source MVM panels actually reference (the dotted panel
// background, fade masks, and the shared UI icon set) out of the game's VPK, so the mockup shows
// Valve's own artwork rather than stand-ins.
//
// Compiled resources change extension on the way out: .vtex -> .png, .vsvg -> .svg. The shim's
// imageUrl() applies the same rewrite, so s2r:// references resolve without touching the UI code.
//
// Run:  node UiMockup/fetch-assets.mjs

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CLI, PAK } from './lib/vrf.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, 'assets');

const FILTERS = [
  'panorama/images/icons/ui/',
  'panorama/images/backgrounds/bluedots_large',
  'panorama/images/masks/top-bottom-fade-4',
  'panorama/images/map_icons/',
  'panorama/images/icons/ct_logo',
  'panorama/images/icons/t_logo',
].join(',');

for (const [label, p] of [['Source2Viewer CLI', CLI], ['CS2 pak01_dir.vpk', PAK]]) {
  if (!p || !fs.existsSync(p)) {
    console.error(`${label} not found:\n  ${p || '(no CS2 install found in Steam)'}\nSet SOURCEMVM_VRF_CLI / CS2_PAK to override.`);
    process.exit(1);
  }
}

fs.mkdirSync(OUT, { recursive: true });
execFileSync(CLI, ['-i', PAK, '-o', OUT, '-d', '-f', FILTERS, '--threads', '4'], { stdio: 'inherit' });

// The Customize modal shows each agent, weapon finish, knife and glove with its own inventory
// thumbnail (applySwatch in CameraEditorCustomizeJs.h). The catalog names exactly which ones, so
// only those are extracted: the whole econ tree is several GB of variants nothing references.
// Paths are batched to keep each command line well under the Windows limit.
const CATALOG = path.resolve(HERE, '..', 'AfxHookSource2', 'Filmmaker', 'Panorama', 'CameraEditorCosmeticsCatalog.inc');
const econ = [...new Set([...fs.readFileSync(CATALOG, 'utf8').matchAll(/"img":"([^"]+)"/g)].map((m) => m[1]))]
  .map((img) => `panorama/images/${img}_png.vtex_c`)
  .filter((vpkPath) => !fs.existsSync(path.join(OUT, vpkPath.replace(/\.vtex_c$/, '.png'))));
for (let i = 0; i < econ.length; i += 200) {
  execFileSync(CLI, ['-i', PAK, '-o', OUT, '-d', '-f', econ.slice(i, i + 200).join(','), '--threads', '4'], { stdio: 'ignore' });
  console.log(`econ thumbnails: ${Math.min(i + 200, econ.length)} / ${econ.length}`);
}

// Our own Panorama textures (the outliner icon set, compiled from FX/ui into the FX pack) are
// copied beside CS2's, under the name the shim derives from `s2r://.../<name>_png.vtex`.
const OWN_SRC = path.resolve(HERE, '..', 'FX', 'ui', 'panorama', 'images', 'sourcemvm');
const OWN_DST = path.join(OUT, 'panorama', 'images', 'sourcemvm');
fs.mkdirSync(OWN_DST, { recursive: true });
for (const f of fs.existsSync(OWN_SRC) ? fs.readdirSync(OWN_SRC) : []) {
  if (f.endsWith('.png')) fs.copyFileSync(path.join(OWN_SRC, f), path.join(OWN_DST, f.replace(/\.png$/, '_png.png')));
}

// Per-player death animations: the picker's clip list and its preview both come from the baked
// library, which the browser parses itself (shim/preview3d.js), so it is copied as it ships.
const DEATH_SRC = path.resolve(HERE, '..', 'FX', 'death-animations', 'ragdoll_animations.mvmanim');
const DEATH_DST = path.join(OUT, 'ragdoll');
fs.mkdirSync(DEATH_DST, { recursive: true });
if (fs.existsSync(DEATH_SRC)) fs.copyFileSync(DEATH_SRC, path.join(DEATH_DST, 'ragdoll_animations.mvmanim'));
else console.warn(`ragdoll animation file not found: ${DEATH_SRC}`);
// Agent models for the 3D previews are exported on demand by the preview server
// (Automation/ui/preview.mjs, assets/models/), because every agent is ~20 MB with its textures.

// Panorama's animated panel backgrounds are LOOSE .webm files under the game folder, not VPK
// entries, so they are copied rather than extracted. file://{resources}/ resolves to csgo/panorama/.
const VIDEO_SRC = path.join(path.dirname(PAK), 'panorama', 'videos');
const VIDEO_DST = path.join(OUT, 'videos');
const WANT_VIDEOS = ['linemap.webm'];
if (fs.existsSync(VIDEO_SRC)) {
  fs.mkdirSync(VIDEO_DST, { recursive: true });
  for (const v of WANT_VIDEOS) {
    const src = path.join(VIDEO_SRC, v);
    if (fs.existsSync(src)) fs.copyFileSync(src, path.join(VIDEO_DST, v));
    else console.warn(`video not found: ${src}`);
  }
  console.log(`${WANT_VIDEOS.length} panel video(s) copied`);
}

// The launcher already ships the real Stratum2 faces; copy them in so the mockup's typography is
// the game's rather than a system fallback.
const FONT_SRC = path.resolve(HERE, '..', 'SourceMovieMakerLauncher', 'public', 'fonts');
const FONT_DST = path.join(HERE, 'fonts');
if (fs.existsSync(FONT_SRC)) {
  fs.mkdirSync(FONT_DST, { recursive: true });
  for (const f of ['stratum2-regular.otf', 'stratum2-medium.otf', 'stratum2-bold.otf', 'stratum2mono-regular.otf']) {
    const src = path.join(FONT_SRC, f);
    if (fs.existsSync(src)) fs.copyFileSync(src, path.join(FONT_DST, f));
  }
  console.log('fonts copied to UiMockup/fonts/');
} else {
  console.warn('launcher fonts not found; the mockup will fall back to a system font');
}

const GAME_FONTS = path.join(path.dirname(PAK), 'panorama', 'fonts');
for (const name of ['stratum2condensed-regular.otf', 'stratum2condensed-medium.otf', 'stratum2condensed-bold.otf', 'stratum2regular_monodigit.ttf', 'stratum2bold_monodigit.ttf']) {
  const source = path.join(GAME_FONTS, name);
  if (fs.existsSync(source)) fs.copyFileSync(source, path.join(FONT_DST, name));
}
let n = 0;
(function walk(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    if (e.isDirectory()) walk(path.join(d, e.name)); else n++;
  }
})(OUT);
console.log(`\n${n} assets under UiMockup/assets/`);
