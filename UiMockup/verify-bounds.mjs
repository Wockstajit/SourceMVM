#!/usr/bin/env node
// Does every panel stay on the screen?
//
//   node UiMockup/verify-bounds.mjs [surface ...]
//
// A Panorama panel is never enlarged by its children: width:100% is exactly the parent's width and
// anything too big is squished or clipped. The browser does the opposite -- a grid/flex item's
// automatic minimum is its content -- so a root could quietly grow past 1920 and take its
// right-hand column off the side of the screen with it.
//
// That is not a cosmetic difference, because the layout reads its own result back:
// CameraEditorJs.h measures rw = root.actuallayoutwidth every frame and
// CameraEditorRenderLayoutJs.h places the settings column at rootWidth - setW. A root that grows
// to fit its children feeds its own width back in, so the column marches further right on every
// frame (measured at 1978 -> 2050 -> 2129px) until the whole RENDER panel is off-screen: no
// Camera/HUD/Output tabs, no Back button, no footer buttons.
//
// Horizontal only, deliberately. Panels legitimately run past the bottom -- Config's card list is
// taller than the screen and the game scrolls it too -- but nothing in this UI scrolls sideways.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SCREEN_W = 1920;
const TOL = 0.5;
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
  const profile = path.join(os.tmpdir(), 'mvm-bounds-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars',
      '--force-device-scale-factor=1', '--window-size=1920,1080', '--dump-dom',
      '--virtual-time-budget=24000', '--user-data-dir=' + profile, '--no-first-run', url],
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

const argv = process.argv.slice(2);
const SURFACES = argv.length ? argv : ['confighud', 'cameraeditor', 'cameratimeline', 'grapheditor',
  'render', 'keyeditor', 'customize', 'markerhud', 'moviehud', 'demobar', 'aimcrosshair'];

const chrome = findChrome();
const srv = await serve(Number(process.env.MVM_PORT || 8909));
let bad = 0;

for (const s of SURFACES) {
  const html = await dumpDom(chrome, `http://127.0.0.1:${srv.address().port}/index.html?bare=1&geom=1&watch=0&surface=${s}`);
  const geom = parsePre(html, 'mvm-geom');
  if (!geom) { console.log(`FAIL ${s.padEnd(14)} page never reported its geometry`); bad++; continue; }

  // ROOTS only. A child may legitimately sit past the edge -- the game's own Movie HUD card is
  // authored at 1682 with a width of 278 and runs to 1960, and Panorama simply clips it -- so
  // flagging every overhanging panel would report faithful reproductions as faults. A ROOT is
  // different: it IS the screen, in the engine it cannot be anything else, and the moment one
  // grows the layout that measures it starts walking its own columns off the side.
  const over = [];
  for (const [root, rows] of Object.entries(geom.roots || {})) {
    const r = (rows || []).find((x) => x.d === 0);
    if (!r || !(r.v === 1 && r.w > 0)) continue;
    if (r.w > SCREEN_W + TOL || r.x < -TOL || r.x + r.w > SCREEN_W + TOL) {
      over.push(`${root} is ${Math.round(r.w)}x${Math.round(r.h)} at ${Math.round(r.x)},`
        + `${Math.round(r.y)} -- a root must be ${SCREEN_W} wide and start at 0`);
    }
  }
  if (over.length) {
    bad++;
    console.log(`FAIL ${s.padEnd(14)} ${over.length} root(s) wider than the screen`);
    for (const o of over) console.log('       ' + o);
  } else {
    console.log(`ok   ${s.padEnd(14)} every root is exactly the screen`);
  }
}
srv.close();
console.log(bad ? `\n${bad} surface(s) with panels off-screen` : `\nno panel runs off the side of the screen`);
process.exit(bad ? 1 : 0);
