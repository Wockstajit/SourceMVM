#!/usr/bin/env node
// Does translated text still fit? (Docs/panorama/localization.md)
//
//   node Automation/i18n/fit-audit.mjs <language[,language...]> [view ...] [--json <dir>] [--all]
//
// Opens each mockup view (the ones UiMockup/capture-mock.mjs captures) twice in headless Chrome,
// once in English and once with ?lang=<language>, and compares every text label:
//
//   cut     the text is wider than the box it was given. In the browser it is cut with "…"; in
//           game a label set to shrink is scaled down instead, so a large overflow means small,
//           hard-to-read text. Reported when it overflows more than it does in English.
//   wraps   the label takes more lines than it does in English, which pushes the rows under it.
//
// Running text (a hint, a description) is allowed to wrap; a label that already takes two or more
// lines in English is treated as running text and only reported when it is cut. --all lists
// those as well.
//
// The browser is not the game: fonts and metrics differ by a few pixels, so this finds the labels
// worth shortening and the game capture confirms them. Needs Chrome or Edge (CHROME_EXE to name it)
// and an up-to-date UiMockup/generated (node UiMockup/extract-ui.mjs).

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, rm } from 'node:fs/promises';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { REPO } from './strings.mjs';

const MOCK = path.join(REPO, 'UiMockup');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.webm': 'video/webm',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf' };

function findChrome() {
  for (const p of [process.env.CHROME_EXE, 'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']) if (p && fs.existsSync(p)) return p;
  throw new Error('No Chrome/Edge found. Set CHROME_EXE.');
}
function serve() {
  const srv = createServer(async (req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(MOCK, rel);
    if (!file.startsWith(MOCK)) { res.writeHead(403).end(); return; }
    let buf; try { buf = await readFile(file); } catch { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(buf);
  });
  return new Promise((ok, bad) => { srv.on('error', bad); srv.listen(0, '127.0.0.1', () => ok(srv)); });
}
function dumpDom(chrome, url) {
  const profile = path.join(os.tmpdir(), 'mvm-fit-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
      '--window-size=1920,1080', '--dump-dom', '--virtual-time-budget=' + (90000 + 30000 * langs.length), '--user-data-dir=' + profile,
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

// The views capture-mock.mjs knows, read from its VIEWS table (it is a script, not a module).
function views() {
  const src = fs.readFileSync(path.join(MOCK, 'capture-mock.mjs'), 'utf8');
  const out = {};
  for (const m of src.matchAll(/^\s{2}([a-z0-9_]+):\s*"((?:[^"\\]|\\.)*)",?\s*$/gm)) out[m[1]] = m[2];
  // Plain surfaces as well: a view exists only where someone needed a capture.
  for (const s of ['confighud', 'cameraeditor', 'cameratimeline', 'grapheditor', 'render', 'keyeditor',
    'customize', 'markerhud', 'moviehud', 'header', 'onboarding']) if (!out[s]) out[s] = 'surface=' + s;
  return out;
}

const argv = process.argv.slice(2);
const jsonAt = argv.indexOf('--json');
const jsonDir = jsonAt >= 0 ? argv[jsonAt + 1] : null;
const showAll = argv.includes('--all');
const plain = argv.filter((a, i) => !a.startsWith('--') && (jsonAt < 0 || i !== jsonAt + 1));
if (!plain.length) { console.error('usage: node Automation/i18n/fit-audit.mjs <language[,language...]> [view ...] [--json <dir>] [--all]'); process.exit(2); }
const langs = plain[0].split(',');
const all = views();
const names = plain.length > 1 ? plain.slice(1) : Object.keys(all);

const chrome = findChrome();
const srv = await serve();
const base = `http://127.0.0.1:${srv.address().port}/index.html?bare=1&watch=0&textaudit=1&`;
const byLang = Object.fromEntries(langs.map((l) => [l, { findings: [], seen: new Set() }]));
let failed = 0;

function compare(name, lang, en, tr) {
  const { findings, seen } = byLang[lang];
  // Same build, same state: the two lists line up one to one. When a view ends up in a different
  // state (a label-driven click did not land), compare what does line up by position.
  const aligned = en.length === tr.length;
  let cut = 0, wraps = 0;
  for (let i = 0; i < tr.length; i++) {
    const t = tr[i];
    const e = aligned ? en[i] : en.find((x) => x.where === t.where && Math.abs(x.x - t.x) < 3 && Math.abs(x.y - t.y) < 3);
    if (!e || e.text === t.text) continue;
    const running = e.lines >= 2 && !t.nowrap;
    const isCut = t.clip > Math.max(2, e.clip);
    const isWrap = t.lines > e.lines && (!running || showAll);
    if (!isCut && !isWrap) continue;
    const id = (isCut ? 'cut|' : 'wraps|') + e.text + '|' + t.text;
    if (seen.has(id)) continue;
    seen.add(id);
    if (isCut) cut++; else wraps++;
    findings.push({ view: name, kind: isCut ? 'cut' : 'wraps', english: e.text, text: t.text, where: t.where,
      overflowPx: t.clip, boxPx: t.w, lines: t.lines, englishLines: e.lines });
  }
  console.log(`${cut || wraps ? 'CHECK' : 'ok   '} ${lang.padEnd(11)} ${name.padEnd(22)} ${tr.length} labels`
    + (aligned ? '' : ' (state differs from English)') + (cut || wraps ? `  cut ${cut}  wraps ${wraps}` : ''));
}

// One browser start per view: the page audits English, then loads each language in turn, rebuilds
// and steers to the same view again (?auditlangs, UiMockup/compare.js).
async function audit(name) {
  const query = all[name];
  if (!query) { console.log(`SKIP ${name}: no such view`); return; }
  const got = parsePre(await dumpDom(chrome, base + query + '&auditlangs=' + langs.join(',')), 'mvm-text');
  if (!got || !got.english) { console.log(`FAIL ${name.padEnd(22)} the page never reported its text`); failed++; return; }
  for (const l of langs) {
    const tr = got.langs[l];
    if (!Array.isArray(tr)) { console.log(`FAIL ${l.padEnd(11)} ${name.padEnd(22)} ${tr && tr.error ? tr.error : 'no result'}`); failed++; continue; }
    compare(name, l, got.english, tr);
  }
}

// A few views at a time: each one is a whole browser working through every language.
for (let i = 0; i < names.length; i += 3) await Promise.all(names.slice(i, i + 3).map(audit));
srv.close();

if (jsonDir) fs.mkdirSync(jsonDir, { recursive: true });
for (const l of langs) {
  const { findings } = byLang[l];
  if (langs.length === 1) {
    for (const f of findings) {
      console.log(`  ${f.kind.padEnd(5)} [${f.view}] ${JSON.stringify(f.english)} -> ${JSON.stringify(f.text)}`
        + (f.kind === 'cut' ? `  (${f.overflowPx}px over a ${f.boxPx}px box)` : `  (${f.englishLines} -> ${f.lines} lines)`));
    }
  }
  console.log(`${l}: ${findings.filter((f) => f.kind === 'cut').length} cut, ${findings.filter((f) => f.kind === 'wraps').length} wrapping, in ${names.length} views`);
  if (jsonDir) fs.writeFileSync(path.join(jsonDir, l + '.json'), JSON.stringify(findings, null, 1) + String.fromCharCode(10));
}
process.exitCode = failed ? 1 : 0;
