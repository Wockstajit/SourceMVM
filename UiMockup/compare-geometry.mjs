// Diffs the browser mockup's panel geometry against the real game's, so "is it one to one" has an
// answer in numbers rather than in impressions.
//
//   1. in game:  mirv_filmmaker livejs context hud
//                mirv_filmmaker livejs load <abs>/Automation/livejs/dump-ui-geometry.js
//      ...capture the console output to a file (Automation/netcon/cs2-netcon.ps1 prints it).
//   2. node UiMockup/compare-geometry.mjs <that file> [--surface cameraeditor] [--tol 1.5]
//
// Both sides run the SAME panel script, so their trees must agree panel for panel. The walk is
// therefore aligned by position, not by id (only ~8% of panels have one), and the first structural
// divergence is reported as a finding in its own right -- a missing or extra panel is a bigger bug
// than a few px of drift, and it also invalidates every rect after it.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, mkdir, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import os from 'node:os';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// Which mockup surface builds which game root. The game has every root alive at once; the mockup
// runs one surface at a time, so a comparison is per-root.
const ROOT_SURFACE = {
  CamEditorRoot: 'cameraeditor',
  CamEditorRenderRoot: 'render',
  CamEditorConfirmRoot: 'cameraeditor',
  CamEditorDebugRoot: 'cameraeditor',
  ConfigHudRoot: 'confighud',
  CamTimelineRoot: 'cameratimeline',
  CamTimelineRangeRoot: 'cameratimeline',
  GraphExpRoot: 'grapheditor',
  MarkerHudRoot: 'markerhud',
  MovieHudRoot: 'moviehud',
  AimXhairRoot: 'aimcrosshair',
  FxDebugHudRoot: 'fxdebug'
};

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml',
  '.webm': 'video/webm', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf'
};

function findChrome() {
  if (process.env.CHROME_EXE && existsSync(process.env.CHROME_EXE)) return process.env.CHROME_EXE;
  for (const p of ['C:/Program Files/Google/Chrome/Application/chrome.exe',
                   'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
                   'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']) {
    if (existsSync(p)) return p;
  }
  throw new Error('No Chrome/Edge found. Set CHROME_EXE.');
}

function serve(port) {
  const srv = createServer(async (req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(HERE, rel);
    if (!file.startsWith(HERE)) { res.writeHead(403).end(); return; }
    let buf;
    try { buf = await readFile(file); } catch { res.writeHead(404).end('nope'); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(buf);
  });
  return new Promise((ok, bad) => { srv.on('error', bad); srv.listen(port, '127.0.0.1', () => ok(srv)); });
}

// Chrome --dump-dom prints the settled DOM, which is where compare.js parks the JSON. That keeps
// this to two moving parts (a static server and a browser) instead of a devtools driver.
function dumpDom(chrome, url) {
  const profile = path.join(os.tmpdir(), 'mvm-geom-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, [
      '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
      '--window-size=1920,1080', '--dump-dom', '--virtual-time-budget=18000',
      '--user-data-dir=' + profile, '--no-first-run', '--no-default-browser-check',
      '--disable-features=CalculateNativeWinOcclusion', url
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', (d) => { out += d; });
    p.on('close', () => { rm(profile, { recursive: true, force: true }).catch(() => {}); ok(out); });
  });
}

function extractGeom(html) {
  const m = /<pre id="mvm-geom"[^>]*>([\s\S]*?)<\/pre>/.exec(html);
  if (!m) return null;
  const json = m[1].replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&');
  try { return JSON.parse(json); } catch { return null; }
}

// The game dump is TSV: GEOM \t depth \t id \t type \t x \t y \t w \t h \t visible \t text,
// bracketed by GEOMROOT lines naming which root the following rows belong to.
function parseGameDump(text) {
  const roots = {};
  let cur = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^\s*/, '');
    if (line.startsWith('GEOMROOT\t')) {
      const [, name, statusRaw] = line.split('\t');
      const status = (statusRaw || '').trim();
      cur = status === 'PRESENT' ? (roots[name] = []) : null;
      if (status !== 'PRESENT') roots[name] = null;
      continue;
    }
    if (!line.startsWith('GEOM\t')) continue;
    const f = line.split('\t');
    if (f.length < 9 || !cur) continue;
    cur.push({
      d: +f[1], id: f[2], ty: f[3],
      x: +f[4], y: +f[5], w: +f[6], h: +f[7], v: +f[8],
      t: (f[9] || '').trim()
    });
  }
  return roots;
}

function pathLabel(rows, i) {
  // Nearest ancestor with an id, plus the offsets down to this row, so a finding can be located in
  // the source without counting divs.
  const me = rows[i];
  let anchor = '(root)';
  for (let j = i; j >= 0; j--) {
    if (rows[j].d < me.d && rows[j].id) { anchor = rows[j].id; break; }
    if (j === i && rows[j].id) { anchor = rows[j].id; break; }
  }
  return `${anchor} > d${me.d} ${me.ty}${me.t ? ' "' + me.t.slice(0, 24) + '"' : ''}`;
}

// Panorama's Slider builds its own SliderTrack / SliderTrackProgress / SliderThumb / SliderDefault
// children; the shim implements a Slider as one native range input styled to match. That is a
// deliberate, documented divergence, but left alone it derails the whole aligned walk at the first
// slider. Collapse a Slider to a leaf on both sides so everything after it still lines up.
function collapseSliders(rows) {
  const out = [];
  for (let i = 0; i < rows.length; i++) {
    out.push(rows[i]);
    if (rows[i].ty !== 'Slider') continue;
    const d = rows[i].d;
    while (i + 1 < rows.length && rows[i + 1].d > d) i++;
  }
  return out;
}

// Panorama's .text getter returns the string AFTER text-transform, while the DOM keeps the authored
// case in the node and does the casing in CSS. Comparing raw would flag every uppercase label in
// the UI. Compare the rendered form: case-folded, whitespace-normalised.
function normText(t) {
  return String(t == null ? '' : t)
    .replace(/\\u([0-9a-f]{4})/gi, (_, h) => String.fromCharCode(parseInt(h, 16)))
    .replace(/\s+/g, ' ')
    .trim()
    .toUpperCase();
}

function compareRoot(name, game, mock, tol) {
  game = collapseSliders(game);
  mock = collapseSliders(mock);
  const out = { root: name, structural: null, rects: [], texts: [], counts: { game: game.length, mock: mock.length } };
  const n = Math.min(game.length, mock.length);
  for (let i = 0; i < n; i++) {
    const g = game[i], m = mock[i];
    if (g.d !== m.d || g.ty !== m.ty) {
      out.structural = { at: i, game: `d${g.d} ${g.ty} ${g.id || ''} "${g.t}"`, mock: `d${m.d} ${m.ty} ${m.id || ''} "${m.t}"` };
      break;
    }
    // A panel the engine reports invisible has no meaningful rect; comparing them produces noise.
    if (!g.v && !m.v) continue;
    if (g.v !== m.v) { out.rects.push({ i, where: pathLabel(game, i), why: `visible ${g.v} vs ${m.v}` }); continue; }
    if (![g.x, g.y, g.w, g.h, m.x, m.y, m.w, m.h].every(Number.isFinite)) continue;
    const dx = m.x - g.x, dy = m.y - g.y, dw = m.w - g.w, dh = m.h - g.h;
    if (Math.abs(dx) > tol || Math.abs(dy) > tol || Math.abs(dw) > tol || Math.abs(dh) > tol) {
      out.rects.push({
        i, where: pathLabel(game, i),
        why: `dx${dx.toFixed(1)} dy${dy.toFixed(1)} dw${dw.toFixed(1)} dh${dh.toFixed(1)}`,
        game: `${g.x},${g.y} ${g.w}x${g.h}`, mock: `${m.x},${m.y} ${m.w}x${m.h}`,
        area: Math.abs(dw) + Math.abs(dh) + Math.abs(dx) + Math.abs(dy)
      });
    }
    if (normText(g.t) !== normText(m.t)) out.texts.push({ i, where: pathLabel(game, i), game: g.t, mock: m.t });
  }
  if (!out.structural && game.length !== mock.length) {
    out.structural = { at: n, game: game.length > n ? `${game.length - n} more panels in game` : 'ended', mock: mock.length > n ? `${mock.length - n} more panels in mockup` : 'ended' };
  }
  return out;
}

const argv = process.argv.slice(2);
let dumpFile = null, only = null, tol = 1.5, port = 8901, limit = 25, at = null;
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--surface') only = argv[++i];
  else if (argv[i] === '--tol') tol = Number(argv[++i]);
  else if (argv[i] === '--port') port = Number(argv[++i]);
  else if (argv[i] === '--limit') limit = Number(argv[++i]);
  else if (argv[i] === '--at') at = Number(argv[++i]);
  else dumpFile = argv[i];
}
if (!dumpFile) {
  console.error('usage: node UiMockup/compare-geometry.mjs <game-dump.txt> [--surface <id>] [--tol 1.5]');
  process.exit(2);
}

const gameRoots = parseGameDump(await readFile(dumpFile, 'utf8'));
const chrome = findChrome();
const srv = await serve(port);

let totalRect = 0, totalText = 0, totalStruct = 0;
for (const [root, rows] of Object.entries(gameRoots)) {
  const surface = ROOT_SURFACE[root];
  if (!rows || !surface) continue;
  if (only && only !== surface && only !== root) continue;

  const html = await dumpDom(chrome, `http://127.0.0.1:${port}/index.html?bare=1&geom=1&surface=${surface}`);
  const geom = extractGeom(html);
  const mock = geom && geom.roots && geom.roots[root];
  console.log(`\n=== ${root}  (mockup surface: ${surface}) ===`);
  if (!mock) { console.log(`  the mockup never built ${root} for surface ${surface}`); totalStruct++; continue; }

  // --at prints both trees side by side around one index, which is how a structural divergence
  // gets identified: the walk position alone does not say WHICH panel went missing.
  if (at !== null) {
    const G = collapseSliders(rows), M = collapseSliders(mock);
    console.log(`  index | game                                   | mockup`);
    for (let i = Math.max(0, at - 6); i < Math.min(Math.max(G.length, M.length), at + 8); i++) {
      const f = (r) => r ? `d${r.d} ${r.ty}${r.id ? '#' + r.id : ''} ${r.w}x${r.h}${r.v ? '' : ' HIDDEN'} ${r.t ? '"' + r.t.slice(0, 18) + '"' : ''}`.slice(0, 38).padEnd(38) : '-'.padEnd(38);
      console.log(`  ${String(i).padStart(5)} | ${f(G[i])} | ${f(M[i])}`);
    }
  }

  const r = compareRoot(root, rows, mock, tol);
  console.log(`  panels: game ${r.counts.game}  mockup ${r.counts.mock}`);
  if (r.structural) {
    totalStruct++;
    console.log(`  STRUCTURAL DIVERGENCE at panel #${r.structural.at}`);
    console.log(`    game  : ${r.structural.game}`);
    console.log(`    mockup: ${r.structural.mock}`);
    console.log('    (rect comparison stops here -- every later index is misaligned)');
  }
  totalRect += r.rects.length;
  totalText += r.texts.length;
  if (r.rects.length) {
    console.log(`  ${r.rects.length} panels off by more than ${tol}px:`);
    for (const d of r.rects.sort((a, b) => (b.area || 0) - (a.area || 0)).slice(0, limit)) {
      console.log(`    #${String(d.i).padStart(4)} ${d.where}`);
      console.log(`          ${d.why}${d.game ? `   game ${d.game}   mock ${d.mock}` : ''}`);
    }
    if (r.rects.length > limit) console.log(`    ... ${r.rects.length - limit} more`);
  }
  if (r.texts.length) {
    console.log(`  ${r.texts.length} text mismatches:`);
    for (const d of r.texts.slice(0, limit)) console.log(`    #${String(d.i).padStart(4)} ${d.where}\n          game "${d.game}"  mock "${d.mock}"`);
    if (r.texts.length > limit) console.log(`    ... ${r.texts.length - limit} more`);
  }
  if (!r.structural && !r.rects.length && !r.texts.length) console.log('  identical within tolerance');
}
srv.close();
console.log(`\ntotals: ${totalStruct} structural, ${totalRect} rect, ${totalText} text`);
