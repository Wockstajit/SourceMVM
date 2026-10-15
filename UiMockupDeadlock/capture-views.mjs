#!/usr/bin/env node
// Drive every view headless: prove it builds and renders, screenshot it at a true 1920x1080, and
// write the geometry dump that compare-geometry.mjs diffs against the game.
//
//   node UiMockupDeadlock/capture-views.mjs                 every view
//   node UiMockupDeadlock/capture-views.mjs camera-graph    only this one
//   node UiMockupDeadlock/capture-views.mjs --out some/dir --port 8899
//
// EXITS NON-ZERO IF ANY VIEW THREW. That is the point: a ReferenceError partway through a build
// leaves the script registered on nothing and the panel half-drawn, which in game reads as "the UI
// is just ugly" rather than as an error, and a parse check cannot see it because the file parses.
//
// This runs in a REAL BROWSER, which is what it adds over the Node-stub check in
// Automation/tests/deadlock-panorama-build-tests.mjs. The stub answers "did the script finish"; only
// a real layout engine answers "and where did everything end up", which is the question a mockup
// exists to answer.
//
// It serves the folder itself, so nothing has to be running first. Chrome is found via CHROME_EXE or
// the usual install locations.

import { createServer } from 'node:http';
import { spawn, execFileSync } from 'node:child_process';
import { readFile, mkdir, writeFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..');

const args = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = args.indexOf(name);
  if (i < 0) return fallback;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const OUT = path.resolve(flag('--out', path.join(REPO, 'Automation', 'output', 'deadlock-ui', 'mock')));
const PORT = Number(flag('--port', '8899'));
const only = args.filter((a) => !a.startsWith('--'));

const CHROME = process.env.CHROME_EXE || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
].find((p) => existsSync(p));

if (!CHROME) {
  console.error('No Chrome or Edge found. Set CHROME_EXE.');
  process.exit(1);
}

// Always extract first: the page serves whatever was last extracted, so a stale generated/ would
// quietly check the previous version of the UI.
execFileSync('node', [path.join(HERE, 'extract-deadlock-ui.mjs')], { stdio: 'pipe' });
const manifest = JSON.parse(await readFile(path.join(HERE, 'generated', 'manifest.json'), 'utf8'));
const views = manifest.views.filter((v) => !only.length || only.includes(v.id));
if (!views.length) {
  console.error(`No such view. Known: ${manifest.views.map((v) => v.id).join(', ')}`);
  process.exit(1);
}

// ---- a static server over the mockup folder ----------------------------------------------------
const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.css': 'text/css', '.json': 'application/json', '.png': 'image/png',
  '.jpg': 'image/jpeg', '.otf': 'font/otf', '.ttf': 'font/ttf', '.svg': 'image/svg+xml',
};
const server = createServer(async (req, res) => {
  const rel = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html';
  const file = path.join(HERE, rel);
  if (!file.startsWith(HERE)) { res.writeHead(403).end(); return; }
  try {
    const body = await readFile(file);
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    res.end(body);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((r) => server.listen(PORT, '127.0.0.1', r));

await mkdir(OUT, { recursive: true });
const profile = await import('node:fs/promises')
  .then((fs) => fs.mkdtemp(path.join(os.tmpdir(), 'mvm-deadlock-chrome-')));

// The page is driven through window.MvmDeadlockHarness, the same entry points a person uses, rather
// than by reaching into its internals -- so a change that breaks the UI for a person breaks this too.
const DRIVER = (id) => `
  (async () => {
    const wait = ms => new Promise(r => setTimeout(r, ms));
    for (let i = 0; i < 100 && !window.MvmDeadlockHarness; i++) await wait(100);
    if (!window.MvmDeadlockHarness) return { id: ${JSON.stringify(id)}, fatal: 'the harness never booted' };
    const H = window.MvmDeadlockHarness;
    H.fitStage('100');
    H.setView(${JSON.stringify(id)});
    // Several frames: the panels settle over two or three (the bar is measured, reported, and the
    // dock resized), and a one-frame check would photograph the transient.
    await wait(600);
    const before = H.styleWrites();
    await wait(500);
    const after = H.styleWrites();
    return {
      id: ${JSON.stringify(id)},
      errors: H.errors(),
      gaps: H.gaps(),
      unknownStyle: H.unknownStyle(),
      panels: H.panelCount(),
      // A STEADY-STATE FRAME SHOULD WRITE NOTHING. In game a style write re-does the panel's
      // geometry and a 3D transform allocates a composited layer with its own vertex buffer, which
      // Panorama indexes with an unsigned short -- so a panel restyled every frame exhausts 65535
      // entries and the game dies. This number is how that shows up before it does.
      styleWritesPerSecond: after - before,
      geometry: H.geometry(),
      state: H.state(),
    };
  })()
`;

let failed = 0;
const report = [];

for (const v of views) {
  const png = path.join(OUT, `${v.id}.png`);
  const jsonPath = path.join(OUT, `${v.id}.json`);
  const dumpPath = path.join(OUT, `${v.id}.geom.txt`);

  // One run does both: --screenshot writes the PNG and --dump-dom prints the page, which is the
  // only channel a headless run has back out of the browser. The harness stamps its report into a
  // hidden <pre> for exactly that reason.
  const probe = spawn(CHROME, [
    '--headless=new',
    '--disable-gpu',
    '--hide-scrollbars',
    '--window-size=1920,1080',
    '--force-device-scale-factor=1',
    `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check',
    `--screenshot=${png}`,
    // Longer than the report's own settle time, or the page is torn down mid-stamp and the run
    // reports a fault that is entirely this script's fault.
    '--virtual-time-budget=8000',
    '--dump-dom',
    `http://127.0.0.1:${PORT}/?view=${encodeURIComponent(v.id)}&report=1`,
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
  let dom = '';
  probe.stdout.on('data', (d) => { dom += d; });
  await new Promise((r) => probe.on('exit', r));

  const m = /<pre id="mvm-report"[^>]*>([\s\S]*?)<\/pre>/.exec(dom);
  if (!m) {
    console.error(`  FAIL  ${v.id}: the page never produced a report`);
    failed++;
    continue;
  }
  let result;
  try {
    result = JSON.parse(m[1].replace(/&quot;/g, '"').replace(/&amp;/g, '&')
      .replace(/&lt;/g, '<').replace(/&gt;/g, '>'));
  } catch (e) {
    console.error(`  FAIL  ${v.id}: unreadable report (${e.message})`);
    failed++;
    continue;
  }

  await writeFile(jsonPath, JSON.stringify(result, null, 1), 'utf8');
  await writeFile(dumpPath, result.geometry || '', 'utf8');

  const bad = [];
  if (result.fatal) bad.push(result.fatal);
  if (result.errors?.build) bad.push(`build: ${result.errors.build}`);
  for (const e of result.errors?.render || []) bad.push(`render: ${e.split('\n')[0]}`);
  for (const g of result.gaps || []) bad.push(`gap: ${g.line} (${g.why})`);
  const unknown = Object.keys(result.unknownStyle || {});
  if (unknown.length) bad.push(`unmapped style props: ${unknown.join(', ')}`);

  report.push({ id: v.id, panels: result.panels, writes: result.styleWritesPerSecond, bad });
  if (bad.length) {
    failed++;
    console.error(`  FAIL  ${v.id}`);
    for (const b of bad) console.error(`          ${b}`);
  } else {
    console.log(`  ok    ${v.id.padEnd(18)} ${String(result.panels).padStart(4)} panels, ` +
                `${result.styleWritesPerSecond} style writes/s`);
  }
}

server.close();
await rm(profile, { recursive: true, force: true });

console.log(`\n${report.length - failed}/${report.length} views clean -> ${path.relative(process.cwd(), OUT)}`);

// A steady-state frame writing styles is a real fault, but it is not a build failure, so it is
// reported separately rather than failing the run outright.
const churn = report.filter((r) => r.writes > 0 && !r.bad.length);
if (churn.length) {
  console.log('\nStyle churn in the steady state (in game this exhausts Panorama\'s vertex-buffer list):');
  for (const c of churn) console.log(`  ${c.id}: ${c.writes} writes/s`);
}

process.exit(failed ? 1 : 0);
