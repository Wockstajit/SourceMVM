#!/usr/bin/env node
// Diff the mockup's panel tree against the running game's, position by position.
//
//   1) in game:  mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-geometry.js
//   2) save the console output to a file
//   3) node UiMockupDeadlock/compare-geometry.mjs <that file> --view <id>
//
//   --view <id>   compare against one view's dump (default: camera-native)
//   --tol <px>    how far apart two rects may be before it is reported (default 3)
//   --at <n>      print both OUR trees side by side around index n, to see what diverged
//
// TWO GROUPS, TWO RULES. The probe dumps our workspace and then the game's own replay bar.
//
//   OURS is compared POSITIONALLY: both sides run the same panel script, so the trees have to agree
//   panel for panel, and a STRUCTURAL divergence (a panel present in one and not the other) is
//   reported first and stops the rect comparison, because everything after it is misaligned anyway.
//
//   THE GAME'S BAR is compared BY ID: the mockup stands it up from the game's decompiled layout as
//   far as our code reaches into it, not to the last hover label, so a positional walk would break
//   at the first panel the stand-in leaves out. Every id the stand-in does have is matched to the
//   game's and its rect compared; ids only the game has are listed as stand-in gaps, which is
//   information rather than a fault.
//
// Hidden subtrees are pruned on both sides before anything is compared. They are not on screen, and
// they CANNOT be compared: several panels keep lazily-grown pools whose size depends on how many
// frames that editor has ever rendered, not on the UI.
//
// The game's numbers come from the probe, in design px, window space. The mockup's come from
// capture-views.mjs, in the same space. Run that first with the game's state imported
// (import-state.mjs), or the comparison measures the stand-in state rather than the UI.

import fs from 'node:fs';
import path from 'node:path';
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
const view = flag('--view', 'camera-native');
const tol = Number(flag('--tol', '3'));
const at = flag('--at', null);
const gameFile = args.find((a) => !a.startsWith('--'));

if (!gameFile) {
  console.error('Usage: node UiMockupDeadlock/compare-geometry.mjs <captured console output> [--view id] [--tol px] [--at n]');
  process.exit(2);
}

const mockFile = path.join(REPO, 'Automation', 'output', 'deadlock-ui', 'mock', `${view}.geom.txt`);
if (!fs.existsSync(mockFile)) {
  console.error(`No mockup dump for "${view}" at ${mockFile}\nRun: node UiMockupDeadlock/capture-views.mjs ${view}`);
  process.exit(2);
}

const TAB = String.fromCharCode(9);

// ---- the game's dump ---------------------------------------------------------------------------
// Lines arrive inside whatever else the console printed, so they are picked out by prefix. A netcon
// capture is full of unrelated engine chatter and refusing to parse it would make the tool useless.
function parseGame(text) {
  const groups = { mvm: [], native: [] };
  const missing = [];
  let group = 'mvm';
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/\r$/, '');
    if (line.startsWith('GEOMGROUP' + TAB)) { group = line.split(TAB)[1] === 'native' ? 'native' : 'mvm'; continue; }
    if (line.startsWith('GEOMMISSING' + TAB)) { missing.push(line.split(TAB)[1]); continue; }
    if (line.startsWith('GEOMERR' + TAB)) {
      console.error(`the probe threw in game: ${line.split(TAB).slice(1).join(' ')}`);
      process.exit(1);
    }
    if (!line.startsWith('GEOM' + TAB)) continue;
    const p = line.split(TAB);
    if (p.length < 9) continue;
    groups[group].push({
      depth: Number(p[1]), id: p[2] === '-' ? '' : p[2], type: p[3],
      x: Number(p[4]), y: Number(p[5]), w: Number(p[6]), h: Number(p[7]),
      visible: p[8] === '1', text: p.slice(9).join(' ') || '',
    });
  }
  return { ...groups, missing };
}

// ---- the mockup's dump -------------------------------------------------------------------------
// "depth id type x y w h visible text", with a separator line before the game's bar.
function parseMock(text) {
  const groups = { mvm: [], native: [] };
  let group = 'mvm';
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    if (line.startsWith('---')) { group = 'native'; continue; }
    const p = line.split(' ');
    if (p.length < 8) continue;
    groups[group].push({
      depth: Number(p[0]), id: p[1] === '-' ? '' : p[1], type: p[2],
      x: Number(p[3]), y: Number(p[4]), w: Number(p[5]), h: Number(p[6]),
      visible: p[7] === '1', text: p.slice(8).join(' ') || '',
    });
  }
  return groups;
}

function pruneHidden(rows) {
  const out = [];
  let hideBelow = -1;
  for (const r of rows) {
    if (hideBelow >= 0 && r.depth > hideBelow) continue;
    hideBelow = -1;
    out.push(r);
    if (!r.visible) hideBelow = r.depth;
  }
  return out;
}

const rect = (r) => `${String(r.x).padStart(5)},${String(r.y).padStart(5)} ${String(r.w).padStart(5)}x${String(r.h).padStart(4)}`;
const dist = (a, b) => Math.max(Math.abs(a.x - b.x), Math.abs(a.y - b.y), Math.abs(a.w - b.w), Math.abs(a.h - b.h));

const game = parseGame(fs.readFileSync(gameFile, 'utf8'));
const mock = parseMock(fs.readFileSync(mockFile, 'utf8'));

if (!game.mvm.length) {
  console.error(`No GEOM rows in ${gameFile}. Did the livejs probe run, and is this its output?`);
  process.exit(2);
}
if (game.missing.length) {
  console.log(`Roots the GAME could not find: ${game.missing.join(', ')}`);
  console.log('  (a root missing in game is a build failure there, not a mockup difference)\n');
}

let failed = false;

// ================================================================ OURS: positional
{
  const g = pruneHidden(game.mvm), m = pruneHidden(mock.mvm);
  console.log(`OUR PANELS   game ${game.mvm.length} (${g.length} visible)   mockup ${mock.mvm.length} (${m.length} visible)`);

  if (at !== null) {
    const n = Number(at);
    const lo = Math.max(0, n - 6), hi = Math.min(Math.max(g.length, m.length), n + 7);
    const fmt = (r) => r ? `${String(r.depth).padStart(2)} ${(r.id || '-').padEnd(24).slice(0, 24)} ${rect(r)}` : '-'.padEnd(52);
    console.log('\n  #     GAME                                              MOCKUP');
    for (let i = lo; i < hi; i++) console.log(`${String(i).padStart(4)}${i === n ? ' >' : '  '} ${fmt(g[i])}   ${fmt(m[i])}`);
    process.exit(0);
  }

  const len = Math.min(g.length, m.length);
  let diverged = -1;
  for (let i = 0; i < len; i++) {
    if (g[i].id !== m[i].id || g[i].depth !== m[i].depth) { diverged = i; break; }
  }
  if (diverged >= 0) {
    console.log(`\n  STRUCTURAL DIVERGENCE at index ${diverged}:`);
    console.log(`    game  : depth ${g[diverged].depth}  ${g[diverged].id || '-'}  (${g[diverged].type})`);
    console.log(`    mockup: depth ${m[diverged].depth}  ${m[diverged].id || '-'}  (${m[diverged].type})`);
    console.log(`\n    node UiMockupDeadlock/compare-geometry.mjs ${path.basename(gameFile)} --view ${view} --at ${diverged}`);
    console.log('  The rect comparison is skipped past this point: the walks are out of step.');
    failed = true;
  } else {
    if (g.length !== m.length) {
      const longer = g.length > m.length ? g : m;
      console.log(`  Same tree as far as it goes; the ${g.length > m.length ? 'game' : 'mockup'} has ${Math.abs(g.length - m.length)} more visible panel(s):`);
      for (const r of longer.slice(len, len + 8)) console.log(`    depth ${r.depth}  ${r.id || '-'}  (${r.type})`);
      failed = true;
    } else {
      console.log('  Zero structural divergences: the same tree, panel for panel.');
    }
    const off = [];
    let hiddenBoth = 0, visMismatch = 0;
    for (let i = 0; i < len; i++) {
      const a = g[i], b = m[i];
      // A panel hidden on BOTH sides has no position worth comparing: Panorama keeps a hidden
      // panel's last layout, the browser collapses it to nothing, and neither is what the user sees.
      if (!a.visible && !b.visible) { hiddenBoth++; continue; }
      if (a.visible !== b.visible) {
        visMismatch++;
        console.log(`  visibility differs at ${i}: ${a.id || '-'}  game=${a.visible} mockup=${b.visible}`);
        continue;
      }
      const d = dist(a, b);
      if (d > tol) off.push({ i, d, a, b });
      if (a.text !== b.text && (a.text || b.text)) {
        console.log(`  text differs at ${i}: ${a.id || '-'}\n      game  : "${a.text}"\n      mockup: "${b.text}"`);
      }
    }
    console.log(`  ${off.length} of ${len - hiddenBoth - visMismatch} visible panels are more than ${tol}px apart` +
                ` (${hiddenBoth} hidden on both sides, ${visMismatch} visibility mismatches).`);
    if (visMismatch) failed = true;
    off.sort((p, q) => q.d - p.d);
    for (const o of off.slice(0, 30)) {
      console.log(`  ${String(o.i).padStart(4)}  ${(o.a.id || '-').padEnd(24).slice(0, 24)}  game ${rect(o.a)}  mock ${rect(o.b)}  (${o.d}px)`);
    }
    if (off.length > 30) console.log(`  ... and ${off.length - 30} more`);
    const labels = off.filter((o) => o.a.type === 'Label').length;
    if (labels) {
      console.log(`  ${labels} of those are Labels: Chrome measures text a little wider than Panorama,`);
      console.log('  so a few px on a label is expected; a Panel that is off is not.');
    }
    if (off.length) failed = true;
  }
}

// ================================================================ THE GAME'S BAR: by id
{
  // NOT pruned. The bar's own root reports `visible=false` in game while it is plainly drawn -- the
  // game shows it through its class, not the flag -- so pruning by the flag would drop the whole
  // bar. Ids are matched instead, and a pair that is hidden on both sides is skipped.
  const g = game.native, m = mock.native;
  console.log(`\nTHE GAME'S BAR   game ${g.length} panels   mockup stand-in ${m.length} panels`);
  const byId = (rows) => {
    const map = new Map();
    for (const r of rows) if (r.id && !map.has(r.id)) map.set(r.id, r);
    return map;
  };
  const gm = byId(g), mm = byId(m);
  const shared = [...mm.keys()].filter((id) => gm.has(id));
  const onlyMock = [...mm.keys()].filter((id) => !gm.has(id));
  const onlyGame = [...gm.keys()].filter((id) => !mm.has(id));

  const off = [];
  let hiddenBoth = 0;
  for (const id of shared) {
    const a = gm.get(id), b = mm.get(id);
    if (!a.visible && !b.visible) { hiddenBoth++; continue; }
    if (a.visible !== b.visible) {
      console.log(`    visibility differs: ${id}  game=${a.visible} mockup=${b.visible}`);
      continue;
    }
    const d = dist(a, b);
    if (d > tol) off.push({ id, d, a, b });
  }
  console.log(`  ${shared.length} ids in both (${hiddenBoth} hidden on both sides); ${off.length} more than ${tol}px apart.`);
  off.sort((p, q) => q.d - p.d);
  for (const o of off) console.log(`    ${o.id.padEnd(26).slice(0, 26)}  game ${rect(o.a)}  mock ${rect(o.b)}  (${o.d}px)`);
  if (onlyMock.length) {
    console.log(`  Ids the STAND-IN has that the game does not (a stand-in fault): ${onlyMock.join(', ')}`);
    failed = true;
  }
  if (onlyGame.length) {
    console.log(`  Ids only the game has (${onlyGame.length}) -- the stand-in stops where our code stops reaching:`);
    console.log(`    ${onlyGame.slice(0, 24).join(', ')}${onlyGame.length > 24 ? ', ...' : ''}`);
  }
  if (off.length) failed = true;
}

process.exit(failed ? 1 : 0);
