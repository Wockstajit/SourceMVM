#!/usr/bin/env node
// Can every control actually be clicked, and does clicking it do only its own job?
//
//   node UiMockup/verify-clicks.mjs [surface ...]
//
// Two questions the geometry diff and verify-flow cannot answer:
//
//   1. REACHABLE -- is the control the top-most element at its own centre? A panel can be laid out
//      correctly, carry the right handler, and still be unclickable because something invisible
//      covers it or because a hittest translated into CSS took its whole subtree out of the mouse's
//      reach. document.elementFromPoint is the same question the browser asks on a real click.
//   2. ISOLATED -- does clicking it fire ONLY its own handler? Panorama routes activation to the
//      panel that was hit; a DOM click also bubbles, so a control nested inside another control's
//      row can fire both. That is what made a click on a Handheld Camera setting also toggle the
//      Handheld Camera master switch off.
//
// Runs headless at a real 1920x1080 (the automation pane reports a 0x0 window, so hit-testing has
// to happen somewhere with actual dimensions).
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.webm': 'video/webm',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf' };

function findChrome() {
  for (const p of [process.env.CHROME_EXE, 'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']) if (p && existsSync(p)) return p;
  throw new Error('No Chrome/Edge found. Set CHROME_EXE.');
}
function serve(port) {
  const srv = createServer(async (req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(HERE, rel);
    if (!file.startsWith(HERE)) { res.writeHead(403).end(); return; }
    let buf; try { buf = await readFile(file); } catch { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(buf);
  });
  return new Promise((ok, bad) => { srv.on('error', bad); srv.listen(port, '127.0.0.1', () => ok(srv)); });
}
function dumpDom(chrome, url) {
  const profile = path.join(os.tmpdir(), 'mvm-click-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
      '--window-size=1920,1080', '--dump-dom', '--virtual-time-budget=40000', '--user-data-dir=' + profile,
      '--no-first-run', url], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = ''; p.stdout.on('data', (d) => { out += d; });
    p.on('close', () => { rm(profile, { recursive: true, force: true }).catch(() => {}); ok(out); });
  });
}
function parsePre(html, id) {
  const m = new RegExp('<pre id="' + id + '"[^>]*>([\\s\\S]*?)<\\/pre>').exec(html);
  if (!m) return null;
  const s = m[1].replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
  try { return JSON.parse(s); } catch { return null; }
}

// Each entry is a STATE worth clicking, not just a surface. A plain surface only proves the
// controls that are showing at rest can be pressed, and every destructive action in this tool is
// behind a popup: "Clear All Camera Paths?", "CONFIRM -- RESET CAMERA", "Reset to default". Those
// buttons had never been checked.
// `modal` names the root the case means to open, and the case fails if it did not come up --
// otherwise a popup that stopped opening would pass by having nothing left to audit.
const argv = process.argv.slice(2);
const CASES = argv.length ? argv.map((s) => ({ name: s, q: 'surface=' + s }))
  : [
    { name: 'confighud', q: 'surface=confighud' },
    { name: 'cameraeditor', q: 'surface=cameraeditor' },
    { name: 'cameratimeline', q: 'surface=cameratimeline' },
    { name: 'render', q: 'surface=render' },
    { name: 'render+graph', q: 'surface=render&cmd=mirv_filmmaker editor curveeditor graph' },
    { name: 'keyeditor', q: 'surface=keyeditor' },
    { name: 'grapheditor', q: 'surface=grapheditor' },
    { name: 'demobar', q: 'surface=demobar' },
    { name: 'markerhud', q: 'surface=markerhud' },
    { name: 'moviehud', q: 'surface=moviehud' },
    // The modals. `customize` could not be audited at all until the audit learned that a modal
    // covering what is behind it is the point of a modal.
    { name: 'customize', q: 'surface=customize&hold=1200', modal: 'CamEditorCustomizeRoot',
      expect: ['Apply to', 'Cancel', 'Reset to default'] },
    // The one that was actually broken: the modal with the tallest dock underneath it.
    { name: 'customize+graph', q: 'surface=customize&cmd=mirv_filmmaker editor curveeditor graph&hold=1200',
      modal: 'CamEditorCustomizeRoot', expect: ['Apply to', 'Cancel', 'Reset to default'] },
    // The browser runs the generated catalogue (CameraEditorCosmeticsCatalog.inc), so both pickers
    // list what the game lists: agents grouped by faction, finishes for the weapon actually carried.
    { name: 'customize+agent', q: 'surface=cameraeditor&act=Agent picker&hold=1200',
      // Team-filtered: the captured target (Ryder) is CT, so the T agents are correctly absent.
      modal: 'CamEditorCustomizeRoot', expect: ['Default CT Agent', 'FBI'] },
    { name: 'customize+skin', q: 'surface=cameraeditor&act=Skin picker&hold=1200',
      modal: 'CamEditorCustomizeRoot', expect: ['M4A1-S'] },
    // The per-player death animation picker is an overlay INSIDE ConfigHudRoot, so it is audited as
    // a resting surface: nothing of the panel underneath may be reachable through it.
    { name: 'config+deathanims', q: 'surface=confighud&act=Death clip preview&hold=2500',
      expect: ['CLOSE', 'Death_02a', 'ASSIGN TO', 'USE AUTOMATIC PICK', 'NEXT'] },
    { name: 'customize+reset', q: 'surface=cameraeditor&act=Customize reset&hold=1400',
      modal: 'CamEditorCustomizeConfirmRoot', expect: ['Reset'] },
    { name: 'editor+gear', q: 'surface=cameraeditor&act=gear&hold=600', modal: 'CamEditorSettingsRoot',
      expect: ['SETTINGS'] },
    { name: 'editor+clearall', q: 'surface=cameraeditor&act=Clear All&hold=600', modal: 'CamEditorConfirmRoot',
      expect: ['Yes, clear all', 'No'] },
    // No panel opens here at all: the footer is a ONE-CLICK reset now (ConfigHudWidgetsJs.h
    // resetFooter dropped the arm-then-confirm idiom), so the case proves the button is built
    // and reachable after being pressed, and that nothing else got covered by pressing it.
    { name: 'config+reset', q: 'surface=confighud&act=Reset footer&hold=800', expect: ['RESET CAMERA'] },
    // The per-keyframe popup (CameraTimelineShakeMenuJs.h): its header names the key and its EASE
    // row is the one place with an In-Out button, so those two prove the right popup came up.
    { name: 'timeline+shake', q: 'surface=cameratimeline&act=Shake menu&hold=600', expect: ['KEY', 'In-Out'] },
  ];

const chrome = findChrome();
const srv = await serve(Number(process.env.MVM_PORT || 8907));
let totalUnreachable = 0, totalBubbled = 0;

for (const c of CASES) {
  const html = await dumpDom(chrome, `http://127.0.0.1:${srv.address().port}/index.html?bare=1&clickaudit=1&watch=0&${c.q}`);
  const rep = parsePre(html, 'mvm-clicks');
  if (!rep) { console.log(`FAIL ${c.name.padEnd(19)} page never reported`); totalUnreachable++; continue; }
  // A popup that never opened has no controls to cover, so it would pass silently.
  if (c.modal && !(rep.roots || []).some((r) => r.split(' ')[0] === c.modal)) {
    console.log(`FAIL ${c.name.padEnd(19)} ${c.modal} never opened -- nothing was audited`);
    console.log(`       roots alive: ${(rep.roots || []).join(' | ') || '(none)'}`);
    totalUnreachable++; continue;
  }
  // `expect` names controls the case exists to check, and proves the case actually REACHED its
  // state. A root being up does not prove that on its own: the Config reset footer opens no panel
  // at all, it relabels the SAME button to "CONFIRM -- RESET <name>", so only the label separates
  // an armed footer from an idle one.
  // Matched against every control, reachable or not, because these two are different questions --
  // whether anything covers a control is what the rest of this script measures, and the Config
  // footer sits below the fold of a list the game scrolls too.
  const allLabels = [...(rep.reachable || []), ...rep.unreachable.map((u) => u.label)];
  const missing = (c.expect || []).filter((want) =>
    !allLabels.some((l) => l.toLowerCase().includes(want.toLowerCase())));
  if (missing.length) {
    console.log(`FAIL ${c.name.padEnd(19)} never built: ${missing.join(', ')}`);
    console.log(`       controls: ${allLabels.join(' ~ ')}`);
    totalUnreachable += missing.length; continue;
  }
  // Only an UNRELATED panel sitting on top is a fault: containment and scrolling are normal.
  const isFault = (u) => u.why === 'covered' && u.rel === 'unrelated panel';
  const coveredCount = rep.unreachable.filter(isFault).length;
  totalUnreachable += coveredCount;
  totalBubbled += rep.bubbled.length;
  const bad = coveredCount + rep.bubbled.length;
  const behind = rep.unreachable.filter((u) => u.rel === 'behind a modal').length;
  console.log(`${bad ? 'FAIL' : 'ok  '} ${c.name.padEnd(19)} ${rep.total} controls, ${coveredCount} covered, ${rep.bubbled.length} firing a second handler`
    + (behind ? `, ${behind} behind a modal` : ''));
  console.log(`       surface=${rep.surface} apis=${(rep.apis||[]).join(',')}`);
  if (rep.roots) console.log(`       roots alive: ${rep.roots.join(' | ')}`);
  // Only "covered" is a fault. Off-screen / scrolled-out is what the game does too.
  const covered = rep.unreachable.filter(isFault);
  const scrolled = rep.unreachable.length - covered.length - behind;
  if (scrolled) console.log(`       (${scrolled} more are scrolled or below the fold -- the game scrolls too)`);
  for (const u of covered.slice(0, 14)) console.log(`       COVERED: ${u.label} [${u.rect}]  <-  ${u.covering} [${u.coverRect}]`);
  for (const b of rep.bubbled.slice(0, 12)) console.log(`       BUBBLES: ${b.label}  -- also fires ${b.also}`);
  if (process.env.MVM_LABELS) console.log('       REACHABLE: ' + (rep.reachable || []).join(' ~ '));
}
console.log(totalUnreachable + totalBubbled
  ? `\n${totalUnreachable} unreachable, ${totalBubbled} bubbling`
  : `\nevery control is reachable and fires only its own handler`);
process.exit(totalUnreachable + totalBubbled ? 1 : 0);
