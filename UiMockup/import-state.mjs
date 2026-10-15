// Turns a console capture of Automation/livejs/dump-ui-state.js into
// UiMockup/generated/state-snapshot.json, which the mockup will then use instead of its
// hand-written stand-in state.
//
//   node UiMockup/import-state.mjs <console-capture.txt>
//   node UiMockup/import-state.mjs <capture.txt> --name attach-shot   # keep several snapshots
//
// Why bother: a hand-written feed drifts from the engine's, and every place it drifts shows up as a
// difference in the mockup that has nothing to do with the UI code -- a slider at the wrong value,
// a section that is open in game and shut here. A real capture makes the browser show a real
// moment, and it makes UiMockup/compare-geometry.mjs report layout faults instead of state noise.
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

const argv = process.argv.slice(2);
let src = null, name = 'state-snapshot';
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--name') name = argv[++i];
  else src = argv[i];
}
if (!src) {
  console.error('usage: node UiMockup/import-state.mjs <console-capture.txt> [--name <id>]');
  process.exit(2);
}

const text = await readFile(src, 'utf8');

// One capture holds every root's feed. Reassemble each on its own (so a truncated chunk is caught
// against the length the game reported) and then merge them into one object -- the roots overlap on
// the common fields and disagree on nothing, but each contributes something the others lack.
const feeds = [];
let cur = null;
for (const raw of text.split(/\r?\n/)) {
  const line = raw.replace(/^\s*/, '');
  if (line.startsWith('STATEERR')) { console.error(line); process.exit(1); }
  if (line.startsWith('STATEBEGIN	')) {
    const f = line.split('	');
    cur = { root: f[1], expected: Number(f[2]), chunks: [] };
    feeds.push(cur);
    continue;
  }
  if (line.startsWith('STATE	') && cur) {
    const i = line.indexOf('	', 6);
    cur.chunks[Number(line.slice(6, i))] = line.slice(i + 1);
  }
}
if (!feeds.length) {
  console.error('no STATEBEGIN in that capture -- did livejs run? (mirv_filmmaker livejs context hud first)');
  process.exit(1);
}

const merged = {};
const seen = [];
for (const f of feeds) {
  if (f.chunks.some((c) => c === undefined)) {
    console.error(`${f.root}: chunk(s) missing (${f.chunks.filter(Boolean).length} of ${f.chunks.length}) -- raise -ReadSeconds`);
    process.exit(1);
  }
  const escaped = f.chunks.join('');
  if (escaped.length !== f.expected) {
    console.error(`${f.root}: length mismatch, expected ${f.expected} got ${escaped.length} -- raise -ReadSeconds on the netcon capture`);
    process.exit(1);
  }
  const json = escaped.replace(/\\u([0-9a-f]{4})/gi, (_, h) => String.fromCharCode(parseInt(h, 16)));
  let obj;
  try { obj = JSON.parse(json); } catch (e) {
    console.error(`${f.root}: reassembled text is not JSON: ${e.message}`);
    process.exit(1);
  }
  const added = Object.keys(obj).filter((k) => !(k in merged));
  Object.assign(merged, obj);
  seen.push(`${f.root} (${Object.keys(obj).length} fields, +${added.length} new)`);
}
const parsed = merged;

await mkdir(path.join(HERE, 'generated'), { recursive: true });
const out = path.join(HERE, 'generated', name + '.json');
await writeFile(out, JSON.stringify(parsed, null, 2));
console.log('captured from ' + feeds.length + ' root(s):');
for (const line of seen) console.log('  ' + line);
console.log(`merged: ${Object.keys(parsed).length} top-level fields`);
console.log(`  tick ${parsed.tick}  paused ${parsed.paused}  count ${parsed.count}  selected ${parsed.selected}`);
console.log(`-> ${path.relative(path.resolve(HERE, '..'), out)}`);
console.log('the mockup picks it up automatically; ?live=0 in the URL falls back to the built-in state.');
