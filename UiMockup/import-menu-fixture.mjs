// Import the read-only dump-menu-fixture.js netcon capture, removing local paths.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
export function parseMenuCapture(source) {
  const chunks = Object.create(null), pending = new Set(), result = { tiles: [] };
  for (const line of source.split(/\r?\n/)) {
    const at = line.indexOf('MENUFIXTURE ');
    if (at < 0) continue;
    const row = JSON.parse(line.slice(at + 12));
    if (!/^(demos|maps|tile|organizer|workshop|persona|viewport)$/.test(row.kind) || !Number.isInteger(row.offset) || row.offset < 0 || typeof row.chunk !== 'string') throw new Error('Invalid menu capture chunk');
    if (row.offset === 0) {
      if (pending.has(row.kind)) throw new Error('Incomplete ' + row.kind + ' capture');
      chunks[row.kind] = ''; pending.add(row.kind);
    }
    if (!pending.has(row.kind)) throw new Error('Incomplete ' + row.kind + ' capture');
    if (row.offset !== chunks[row.kind].length) throw new Error('Incomplete ' + row.kind + ' capture');
    chunks[row.kind] += row.chunk;
    if (row.end) {
      pending.delete(row.kind);
      const value = JSON.parse(chunks[row.kind]);
      if (row.kind === 'tile') result.tiles.push(value); else result[row.kind] = value;
    }
  }
  if (pending.size) throw new Error('Incomplete ' + [...pending].join(', ') + ' capture');
  if (!result.demos?.demos || !result.maps) throw new Error('Capture must include demos and workshop maps');
  return result;
}
export function sanitizeMenuFixture(data) {
  const organizer = data.organizer || { sort: 'date', rootCollapsed: false, groups: [], demos: {} };
  const records = {};
  for (const demo of data.demos.demos) {
    const oldKey = (demo.folder.replace(/\//g, '\\').replace(/\\+$/, '') + '\\' + demo.fileName).toLowerCase();
    demo.folder = 'mock-game/replays';
    const newKey = ('mock-game\\replays\\' + demo.fileName).toLowerCase();
    if (organizer.demos && organizer.demos[oldKey]) records[newKey] = organizer.demos[oldKey];
  }
  organizer.demos = records; data.organizer = organizer;
  data.demos.installRoot = 'mock-game/'; data.demos.folders = ['mock-game/replays'];
  if (data.workshop) data.workshop.root = 'mock-workshop/730';
  data.source = 'Local CS2 menu capture; disk actions are simulated in the browser.';
  return data;
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const data = parseMenuCapture(fs.readFileSync(process.argv[2], 'utf8'));
  // Cache installed map artwork locally so the preview also works without Steam/network.
  const assetDir = path.join(HERE, 'assets', 'workshop');
  fs.mkdirSync(assetDir, { recursive: true });
  for (const map of data.maps) {
    const raw = map.imageUrl || '';
    const source = raw.replace(/^file:\/\//, '');
    if (fs.existsSync(source)) {
      const name = String(map.workshop_id) + path.extname(source);
      fs.copyFileSync(source, path.join(assetDir, name));
      map.imageUrl = 'assets/workshop/' + name;
    } else if (/^https:\/\/images\.steamusercontent\.com\//.test(raw)) {
      const name = String(map.workshop_id) + '.jpg', output = path.join(assetDir, name);
      if (!fs.existsSync(output) && !process.argv.includes('--no-thumbnails')) {
        try {
          const response = await fetch(raw, { signal: AbortSignal.timeout(15000) });
          if (!response.ok) throw new Error('HTTP ' + response.status);
          fs.writeFileSync(output, Buffer.from(await response.arrayBuffer()));
        } catch (error) { console.warn('Using Steam URL for ' + map.name + ': ' + error.message); }
      }
      if (fs.existsSync(output)) map.imageUrl = 'assets/workshop/' + name;
    } else map.imageUrl = 'assets/panorama/images/map_icons/screenshots/360p/random_png.png';
  }
  sanitizeMenuFixture(data);
  fs.mkdirSync(path.join(HERE, 'generated'), { recursive: true });
  fs.writeFileSync(path.join(HERE, 'generated', 'menu-fixture.json'), JSON.stringify(data, null, 2));
  console.log(`Imported ${data.maps.length} workshop maps and ${data.demos.demos.length} demos.`);
}
