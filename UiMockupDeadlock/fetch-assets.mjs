#!/usr/bin/env node
// Put Deadlock's OWN fonts and icons next to the mockup, so the browser draws the panel in the
// typeface and with the glyphs the game does.
//
// This matters more than it sounds. Every metric in the UI is tuned against the real faces: a
// label's width decides whether a tab ribbon fits, and Retail Demo is much narrower than a system
// sans. Substituting a fallback would make the mockup disagree with the game about what fits, and
// the disagreement would be in the direction that hides overflow rather than showing it.
//
// Three sources, in order of how much work they are:
//
//   1. FONTS are LOOSE FILES in the Deadlock install. Copied directly -- no VPK, no CLI.
//   2. MOST ICONS are already decompiled under reference/deadlock/panorama/images, which is where
//      the rest of this port reads the game's Panorama from. Copied from there.
//   3. THE NATIVE BAR's icons (hud/replay/*_psd.vtex) are not in that tree. They need the
//      Source2Viewer CLI against Deadlock's own VPK. If it is missing the mockup still runs: the
//      round buttons draw as plain discs, which is obvious rather than subtly wrong.
//
//   node UiMockupDeadlock/fetch-assets.mjs

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..');
const ASSETS = path.join(HERE, 'assets');
const FONTS = path.join(HERE, 'fonts');
const REF = path.join(REPO, 'reference', 'deadlock', 'panorama');

// Source2Viewer CLI is a HELPER, never a build dependency of this repo: a ValveResourceFormat
// checkout next to this repository unless SOURCEMVM_VRF_CLI says otherwise.
const CLI = process.env.SOURCEMVM_VRF_CLI
  || path.join(path.dirname(REPO), 'ValveResourceFormat', 'CLI', 'bin', 'Release', 'Source2Viewer-CLI.exe');

function deadlockDir() {
  if (process.env.SOURCEMVM_DEADLOCK_DIR) return process.env.SOURCEMVM_DEADLOCK_DIR;
  try {
    const out = execFileSync('powershell.exe', [
      '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
      '-File', path.join(REPO, 'Automation', 'lib', 'Resolve-DeadlockDir.ps1')
    ], { encoding: 'utf8' });
    return out.trim();
  } catch {
    return '';
  }
}

// The faces DeadlockStyleJs.h names, mapped to the files on disk. Only the weights the UI asks for:
// pulling all 56 would add megabytes the page has to load before it can measure anything.
const FONT_FILES = [
  'retaildemo-regular.otf',
  'retaildemo-medium.otf',
  'retaildemo-semibold.otf',
  'retaildemo-bold.otf',
  'retailtextdemo-regular.otf',
  'retailtextdemo-bold.otf',
  'reaver-regular.otf',
  'reaver-semibold.otf',
  'valvepulp-bold.ttf',
];

// The icons the WORKSPACE asks for by s2r:// path, which are already decompiled in the reference
// tree. Source path relative to reference/deadlock/panorama, which is also where it is served from.
const REF_IMAGES = [
  'images/transport_controls/icon_play_png.png',
  'images/transport_controls/icon_pause_png.png',
  'images/control_icons/arrow_solid_up_png.png',
  'images/control_icons/arrow_solid_down_png.png',
  'images/control_icons/arrow_solid_left_png.png',
  'images/control_icons/arrow_solid_right_png.png',
  'images/icons/icon_gear.svg',
];

// The NATIVE replay bar's icons, named in citadel_hud_replay.css. Not in the reference tree, so
// these are the ones that need the CLI.
const VPK_IMAGES = [
  'panorama/images/hud/replay/play_icon_psd.vtex',
  'panorama/images/hud/replay/pause_icon_psd.vtex',
  'panorama/images/hud/replay/jump_back2_icon_psd.vtex',
  'panorama/images/hud/replay/jump_back3_icon_psd.vtex',
  'panorama/images/hud/replay/jump_skip2_icon_psd.vtex',
  'panorama/images/hud/replay/jump_skip3_icon_psd.vtex',
  'panorama/images/hud/replay/minus_icon_psd.vtex',
  'panorama/images/hud/replay/plus_icon_psd.vtex',
];

function copyInto(from, to) {
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.copyFileSync(from, to);
}

let copied = 0, missing = [];

// ---- 1. fonts ---------------------------------------------------------------------------------
const dir = deadlockDir();
const fontSrc = dir ? path.join(dir, 'game', 'citadel', 'panorama', 'fonts') : '';
fs.mkdirSync(FONTS, { recursive: true });
if (!fontSrc || !fs.existsSync(fontSrc)) {
  missing.push(`Deadlock fonts (looked in ${fontSrc || '<no install found>'})`);
} else {
  for (const f of FONT_FILES) {
    const src = path.join(fontSrc, f);
    if (!fs.existsSync(src)) { missing.push(`font ${f}`); continue; }
    copyInto(src, path.join(FONTS, f));
    copied++;
  }
}

// ---- 2. icons already decompiled --------------------------------------------------------------
for (const rel of REF_IMAGES) {
  const src = path.join(REF, rel);
  if (!fs.existsSync(src)) { missing.push(`reference image ${rel}`); continue; }
  copyInto(src, path.join(ASSETS, 'panorama', rel));
  copied++;
}

// ---- 3. the native bar's icons, via the CLI ---------------------------------------------------
const pak = process.env.DEADLOCK_PAK
  || (dir ? path.join(dir, 'game', 'citadel', 'pak01_dir.vpk') : '');
if (!fs.existsSync(CLI) || !pak || !fs.existsSync(pak)) {
  missing.push(
    "the native replay bar's icons (needs Source2Viewer-CLI and Deadlock's pak01_dir.vpk; " +
    'set SOURCEMVM_VRF_CLI / DEADLOCK_PAK). The bar still lays out -- its buttons draw as plain discs.'
  );
} else {
  const tmp = path.join(HERE, 'generated', 'vrf-out');
  fs.rmSync(tmp, { recursive: true, force: true });
  fs.mkdirSync(tmp, { recursive: true });
  for (const rel of VPK_IMAGES) {
    try {
      execFileSync(CLI, ['-i', pak, '--vpk_filepath', rel, '-o', tmp, '-d', '--vpk_decompile'],
        { stdio: 'ignore' });
    } catch {
      missing.push(`VPK image ${rel}`);
      continue;
    }
    // The CLI writes the decompiled file under the VPK path with a converted extension.
    const out = path.join(tmp, rel.replace(/\.vtex$/i, '.png'));
    if (!fs.existsSync(out)) { missing.push(`VPK image ${rel} (extracted nothing)`); continue; }
    copyInto(out, path.join(ASSETS, rel.replace(/\.vtex$/i, '.png')));
    copied++;
  }
  fs.rmSync(tmp, { recursive: true, force: true });
}

console.log(`\n${copied} assets staged into UiMockupDeadlock/{assets,fonts}`);
if (missing.length) {
  console.log('\nNot staged:');
  for (const m of missing) console.log(`  - ${m}`);
}
