#!/usr/bin/env node
// Is anything HIDDEN behind something it should be in front of?
//
//   node UiMockup/verify-layers.mjs [surface ...]
//
// Not the same question as verify-clicks.mjs. That asks whether a control can be reached by the
// mouse; a panel with pointer-events:none is invisible to hit testing yet still paints over what
// is under it. This asks who actually paints -- a modal can be perfectly clickable and still be
// covered.
//
// The fault it exists to catch: in Panorama a panel's z-index is scoped to its ROOT, so a child
// asking for z230 inside a z53 root still loses to a z65 sibling root. That is how the Customize
// modal ended up under the graph editor, and how the "clear all" confirm did before it was
// promoted to its own root (see the z-layer map in
// AfxHookSource2/Filmmaker/Panorama/PanoramaBridge.h). Reading the authored z-index cannot find
// this -- it looks correct in the source either way -- so this samples paint ownership instead.
//
// Overlaps the game itself draws are allowed: the bottom editors dock into the editor's lower
// band, the render workspace covers the editor, and a modal root covers everything. Those are in
// MAY_COVER / the modal rule inside Compare.layerAudit.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml',
  '.webm': 'video/webm', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf' };

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
  const profile = path.join(os.tmpdir(), 'mvm-layers-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars',
      '--force-device-scale-factor=1', '--window-size=1920,1080', '--dump-dom',
      '--virtual-time-budget=26000', '--user-data-dir=' + profile, '--no-first-run', url],
      { stdio: ['ignore', 'pipe', 'pipe'] });
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

// Each entry is a state worth checking, not just a surface: a modal only fights the docks when a
// dock is open, which is exactly the case that was broken.
const CASES = process.argv.slice(2).length
  ? process.argv.slice(2).map((s) => ({ name: s, q: 'surface=' + s }))
  : [
    { name: 'confighud', q: 'surface=confighud' },
    { name: 'cameraeditor', q: 'surface=cameraeditor' },
    { name: 'cameratimeline', q: 'surface=cameratimeline' },
    { name: 'grapheditor', q: 'surface=grapheditor' },
    { name: 'render', q: 'surface=render' },
    { name: 'render+graph', q: 'surface=render&cmd=mirv_filmmaker editor curveeditor graph' },
    { name: 'keyeditor', q: 'surface=keyeditor' },
    { name: 'customize', q: 'surface=customize&hold=1200' },
    // The one that was broken: the modal with the tallest dock underneath it.
    { name: 'customize+graph', q: 'surface=customize&cmd=mirv_filmmaker editor curveeditor graph&hold=1200' },
    { name: 'editor+gear', q: 'surface=cameraeditor&act=gear&hold=600' },
    { name: 'editor+clearall', q: 'surface=cameraeditor&act=Clear All&hold=600' },
    { name: 'markerhud', q: 'surface=markerhud' },
    { name: 'moviehud', q: 'surface=moviehud' },
    { name: 'demobar', q: 'surface=demobar' },
  ];

const chrome = findChrome();
const srv = await serve(Number(process.env.MVM_PORT || 8911));
let bad = 0;

for (const c of CASES) {
  const url = `http://127.0.0.1:${srv.address().port}/index.html?bare=1&layers=1&watch=0&${c.q}`;
  const rep = parsePre(await dumpDom(chrome, url), 'mvm-layers');
  if (!rep) { console.log(`FAIL ${c.name.padEnd(17)} page never reported its layers`); bad++; continue; }
  if (rep.covered.length) {
    bad++;
    console.log(`FAIL ${c.name.padEnd(17)} ${rep.covered.length} of ${rep.total} panels covered by an unrelated root`);
    console.log(`       roots: ${rep.roots.join('  ')}`);
    for (const v of rep.covered.slice(0, 8)) {
      console.log(`       "${v.label}" (${v.mine}) is ${v.frac}% painted by ${v.by}  [${v.rect}]`);
    }
  } else {
    console.log(`ok   ${c.name.padEnd(17)} ${rep.total} panels, none hidden behind an unrelated root`);
  }
}
srv.close();
console.log(bad ? `\n${bad} state(s) with something hidden` : `\nnothing is hidden behind anything it should be in front of`);
process.exit(bad ? 1 : 0);
