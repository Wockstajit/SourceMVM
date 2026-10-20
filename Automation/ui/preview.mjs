#!/usr/bin/env node
// Local, read-only preview server + source watcher. Never builds, launches, or controls a game.
// node Automation/ui/preview.mjs cs2|deadlock [--port 8823] [--open]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'node:http';
import { execFileSync, spawn } from 'node:child_process';
import { digest } from '../lib/ui-source-sync.mjs';
import { extractAll } from '../../UiMockup/extract-ui.mjs';
import { extract } from '../../UiMockupDeadlock/extract-deadlock-ui.mjs';
import { agentModelExporter } from '../../UiMockup/lib/vrf.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.css': 'text/css', '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml', '.glb': 'model/gltf-binary', '.otf': 'font/otf', '.ttf': 'font/ttf', '.woff2': 'font/woff2', '.webm': 'video/webm' };

// `missing(rel)` may produce a file the tree does not hold yet (a path, or a promise of one). It is
// only asked about a path that already passed the traversal checks and does not exist.
export function createPreviewServer(dir, missing = null) {
  const root = fs.realpathSync(dir);
  return createServer(async (req, res) => {
    if (!['GET', 'HEAD'].includes(req.method)) { res.writeHead(405, { Allow: 'GET, HEAD' }).end(); return; }
    try {
      const rel = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html';
      // No dotfiles (sync metadata), traversal, or directory junction escapes.
      if (rel.split(/[\\/]/).some((s) => s.startsWith('.'))) { res.writeHead(403).end(); return; }
      const wanted = path.resolve(root, rel);
      if (!path.relative(root, wanted).startsWith('..') && !fs.existsSync(wanted) && missing) {
        const made = await missing(rel.replace(/\\/g, '/'));
        if (made && fs.existsSync(made)) {
          const body = await fs.promises.readFile(made);
          res.writeHead(200, { 'Content-Type': TYPES[path.extname(made)] || 'application/octet-stream',
            'Cache-Control': 'no-store', 'Content-Length': body.length });
          res.end(req.method === 'HEAD' ? undefined : body);
          return;
        }
      }
      const full = await fs.promises.realpath(wanted);
      const relative = path.relative(root, full);
      if (relative.startsWith('..') || path.isAbsolute(relative)) { res.writeHead(403).end(); return; }
      const body = await fs.promises.readFile(full);
      res.writeHead(200, { 'Content-Type': TYPES[path.extname(full)] || 'application/octet-stream',
        'Cache-Control': 'no-store', ETag: `"${digest(body)}"`, 'Content-Length': body.length });
      res.end(req.method === 'HEAD' ? undefined : body);
    } catch { res.writeHead(404).end(); }
  });
}

// CS2 previews: agent models are exported from the VPK the first time a 3D preview asks for one, and
// the death-animation library is read where it ships when fetch-assets has not copied it.
function cs2Missing(dir) {
  const exportAgent = agentModelExporter(path.join(dir, 'assets', 'models'));
  const clips = path.join(ROOT, 'FX', 'death-animations', 'ragdoll_animations.mvmanim');
  // Our own Panorama textures (the outliner's icon set) are compiled from PNGs committed under
  // FX/ui; the shim maps `s2r://panorama/images/sourcemvm/<name>_png.vtex` to
  // `assets/panorama/images/sourcemvm/<name>_png.png`, which is served straight from that source.
  const ownImages = path.join(ROOT, 'FX', 'ui', 'panorama', 'images', 'sourcemvm');
  return (rel) => {
    if (rel === 'assets/ragdoll/ragdoll_animations.mvmanim') return clips;
    const own = /^assets\/panorama\/images\/sourcemvm\/([A-Za-z0-9_]+)_png\.png$/.exec(rel);
    if (own) return path.join(ownImages, own[1] + '.png');
    return exportAgent(rel);
  };
}

export function inputStamp(dirs) {
  const parts = [];
  for (const dir of dirs) for (const file of fs.readdirSync(dir).sort()) {
    if (/\.(?:h|inc|cpp|js)$/.test(file)) parts.push(file, fs.readFileSync(path.join(dir, file)));
  }
  return digest(Buffer.concat(parts.map((p) => Buffer.from(p))));
}

export function watchSources({ stamp, refresh, log = console.log, interval = 1000 }) {
  let previous;
  const tick = () => {
    try {
      const current = stamp();
      if (current === previous) return;
      previous = current;
      refresh();
      previous = stamp();
      log('Preview refreshed from game UI source. Build and relaunch to update the game.');
    } catch (e) { log(`Preview refresh paused: ${e.message}`); }
  };
  tick();
  const timer = setInterval(tick, interval);
  return () => clearInterval(timer);
}

export async function startPreview(game, { port = game === 'cs2' ? 8823 : 8824, open = false } = {}) {
  if (!['cs2', 'deadlock'].includes(game)) throw new Error('Usage: node Automation/ui/preview.mjs cs2|deadlock [--port N] [--open]');
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('Invalid preview port.');
  const dir = path.join(ROOT, game === 'cs2' ? 'UiMockup' : 'UiMockupDeadlock');
  const sources = [path.join(ROOT, 'AfxHookSource2', 'Filmmaker', game === 'cs2' ? 'Panorama' : 'Deadlock')];
  if (game === 'cs2') sources.push(path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'PlayMenu'));
  if (game === 'deadlock') sources.push(path.join(ROOT, 'AfxHookSource2', 'Deadlock'));
  const gen = path.join(dir, 'generated');
  fs.mkdirSync(gen, { recursive: true });
  let jsStamp;
  const derive = () => {
    if (game !== 'cs2') return;
    const next = inputStamp([gen]);
    if (next === jsStamp) return;
    execFileSync(process.execPath, [path.join(dir, 'derive-bindings.mjs')], { stdio: 'pipe', windowsHide: true });
    jsStamp = next;
  };
  // Including generated files also retries a paused refresh after the user resolves/applies a draft.
  const stopWatch = watchSources({ stamp: () => inputStamp([...sources, gen]), refresh: () => {
    try { (game === 'cs2' ? extractAll : extract)(); }
    finally { derive(); }
  } });
  const server = createPreviewServer(dir, game === 'cs2' ? cs2Missing(dir) : null);
  try {
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(port, '127.0.0.1', resolve); });
  } catch (e) { stopWatch(); throw e; }
  const url = `http://127.0.0.1:${server.address().port}/`;
  console.log(`\n${game.toUpperCase()} mockup: ${url}\nSource changes refresh automatically. Mockup drafts stay local until applied.\nClose this window or press Ctrl+C to stop.`);
  if (open && process.platform === 'win32') {
    // Fixed localhost URL only; no user text passed through a shell.
    const child = spawn('cmd.exe', ['/d', '/c', 'start', '', url], { windowsHide: true, stdio: 'ignore' });
    child.on('error', (e) => console.error(`Open ${url} in your browser (${e.message})`));
  }
  server.on('close', stopWatch);
  return server;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2), portIndex = args.indexOf('--port');
  try {
    await startPreview(args[0], { open: args.includes('--open'),
      ...(portIndex >= 0 ? { port: Number(args[portIndex + 1]) } : {}) });
  } catch (e) { console.error(e.message); process.exitCode = 1; }
}
