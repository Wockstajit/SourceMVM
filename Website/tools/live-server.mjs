#!/usr/bin/env node
// Live preview for the download page. Serves Website/ on localhost and updates every open tab when
// a file in it is saved: stylesheet edits are swapped in place (the logo intro does not replay),
// anything else reloads the page. Local only; nothing is built or published.
//
//   node Website/tools/live-server.mjs [--port 8840] [--no-open]
//
// Website/live-preview.bat wraps this for double-click use.
import fs from 'node:fs';
import path from 'node:path';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';

const ROOT = path.resolve(import.meta.dirname, '..');
const args = process.argv.slice(2);
const portIndex = args.indexOf('--port');
const FIRST_PORT = portIndex >= 0 ? Number(args[portIndex + 1]) : 8840;
const OPEN = !args.includes('--no-open');

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.webp': 'image/webp', '.ico': 'image/x-icon', '.mp4': 'video/mp4', '.webm': 'video/webm',
  '.woff2': 'font/woff2', '.txt': 'text/plain; charset=utf-8',
};

// Injected before </body> of every served page; the files on disk never contain it.
const CLIENT = `<script>(() => {
  const live = new EventSource('/__live');
  live.onmessage = (event) => {
    const files = JSON.parse(event.data);
    if (!files.every((f) => f.endsWith('.css'))) { location.reload(); return; }
    for (const link of document.querySelectorAll('link[rel="stylesheet"]')) {
      const url = new URL(link.href);
      if (url.origin !== location.origin) continue;
      url.searchParams.set('live', Date.now());
      const next = link.cloneNode();
      next.href = url.href;
      next.onload = () => link.remove();
      link.after(next);
    }
  };
})();</script>`;

// Not part of the page: the asset tools, docs, and editor/OS scratch files.
function ignored(rel) {
  return rel.startsWith('tools/') || rel.endsWith('.md')
    || rel.split('/').some((s) => s.startsWith('.'))
    || /(~|\.tmp|\.swp|\.crdownload)$/i.test(rel);
}

const clients = new Set();

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');

  if (url.pathname === '/__live') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
    res.write(': connected\n\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405, { Allow: 'GET, HEAD' }).end();
    return;
  }

  let rel;
  try { rel = decodeURIComponent(url.pathname).replace(/^\/+/, ''); } catch { res.writeHead(400).end(); return; }
  if (rel === '' || rel.endsWith('/')) rel += 'index.html';
  let file = path.resolve(ROOT, rel);
  if (path.relative(ROOT, file).startsWith('..') || rel.split(/[\\/]/).some((s) => s.startsWith('.'))) {
    res.writeHead(403).end();
    return;
  }

  let stat;
  try {
    stat = await fs.promises.stat(file);
    if (stat.isDirectory()) { file = path.join(file, 'index.html'); stat = await fs.promises.stat(file); }
  } catch {
    res.writeHead(404, { 'Content-Type': 'text/plain' }).end(`Not found: /${rel}`);
    return;
  }

  const ext = path.extname(file).toLowerCase();
  const headers = { 'Content-Type': TYPES[ext] || 'application/octet-stream', 'Cache-Control': 'no-store' };
  const head = req.method === 'HEAD';

  if (ext === '.html') {
    const html = await fs.promises.readFile(file, 'utf8');
    const body = Buffer.from(html.includes('</body>') ? html.replace('</body>', `${CLIENT}\n</body>`) : html + CLIENT);
    res.writeHead(200, { ...headers, 'Content-Length': body.length });
    res.end(head ? undefined : body);
    return;
  }

  // Byte ranges: <video> seeks with them, and Safari will not play a video served without them.
  headers['Accept-Ranges'] = 'bytes';
  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
  if (range && (range[1] !== '' || range[2] !== '')) {
    const start = range[1] === '' ? Math.max(0, stat.size - Number(range[2])) : Number(range[1]);
    const end = range[1] === '' || range[2] === '' ? stat.size - 1 : Math.min(Number(range[2]), stat.size - 1);
    if (start > end) {
      res.writeHead(416, { 'Content-Range': `bytes */${stat.size}` }).end();
      return;
    }
    res.writeHead(206, { ...headers, 'Content-Range': `bytes ${start}-${end}/${stat.size}`, 'Content-Length': end - start + 1 });
    if (head) res.end(); else fs.createReadStream(file, { start, end }).pipe(res);
    return;
  }
  res.writeHead(200, { ...headers, 'Content-Length': stat.size });
  if (head) res.end(); else fs.createReadStream(file).pipe(res);
});

// Saves arrive as bursts of events (editors write a temp file, then rename it); batch them.
let changed = new Set();
let flush = null;
fs.watch(ROOT, { recursive: true }, (_event, name) => {
  if (!name) return;
  const rel = name.replace(/\\/g, '/');
  if (ignored(rel)) return;
  changed.add(rel);
  clearTimeout(flush);
  flush = setTimeout(() => {
    const files = [...changed];
    changed = new Set();
    const action = files.every((f) => f.endsWith('.css')) ? 'restyle' : 'reload';
    console.log(`${new Date().toLocaleTimeString()}  ${action}  ${files.join(', ')}`);
    for (const client of clients) client.write(`data: ${JSON.stringify(files)}\n\n`);
  }, 120);
});

// Keep idle event streams open.
setInterval(() => { for (const client of clients) client.write(': ping\n\n'); }, 25000).unref();

function listen(port, triesLeft) {
  server.once('error', (err) => {
    if (err.code === 'EADDRINUSE' && triesLeft > 0) listen(port + 1, triesLeft - 1);
    else { console.error(err.message); process.exit(1); }
  });
  server.listen(port, '127.0.0.1', () => {
    const address = `http://localhost:${port}`;
    console.log(`SOURCE:MVM site live at ${address}`);
    console.log(`Serving ${ROOT}`);
    console.log('Save any file in Website/ and the page updates. Ctrl+C to stop.\n');
    // Fixed localhost URL only; nothing user-supplied reaches the shell.
    if (OPEN) spawn('cmd', ['/c', 'start', '', address], { detached: true, stdio: 'ignore' }).unref();
  });
}

listen(FIRST_PORT, 10);
