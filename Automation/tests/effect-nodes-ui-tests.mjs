import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import vm from 'node:vm';

// The CONFIG > NODES tab and its graph editor window (ConfigHudNodesTabJs.h). Static checks for the
// workflow the owner asked for, plus the pure helpers run in a sandbox.
const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const nodes = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudNodesTabJs.h');
const config = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudJs.h');
const effectNodes = read('AfxHookSource2/Filmmaker/EffectNodes/EffectNodes.cpp');
const js = [...nodes.matchAll(/R"CFJS\(([\s\S]*?)\)CFJS"/g)].map((m) => m[1]).join('\n');

function grab(name) {
  const m = new RegExp('function ' + name + '\\([^)]*\\) \\{[\\s\\S]*?\\n    \\}\\n').exec(js);
  assert.ok(m, name + ' not found');
  return m[0];
}

test('the graph window lives in the overlay layer, above the TIMELINE dock', () => {
  assert.match(js, /ngWin = mk\('Panel', cfgOverlay\(\)\)/);
  assert.doesNotMatch(js, /mk\('Panel', root\);\s*\n\s*ngOverlay/);
});

test('the pump and the wheel are driven from Config every frame', () => {
  assert.match(config, /cfgTextPump\(st\); ngPump\(st\); \}/);
  assert.match(config, /if \(ngWheel\(n\)\) return;/);
});

test('a single press drags a node; wires, pan, move and resize are gestures of the pump', () => {
  const pump = grab('ngPump');
  for (const kind of ['node', 'wire', 'pan', 'move', 'size']) assert.match(pump, new RegExp("kind: '" + kind + "'"));
  // The press selects and starts the drag in one go: no double click.
  assert.match(pump, /hit\.kind === 'node'\) \{\s*if \(hit\.n\.id !== ngSelected\)/);
  // Right-click opens the node menu, the wire menu or the add menu.
  assert.match(pump, /ngNodeMenu\(hit\.n\)/);
  assert.match(pump, /nodesCmd\('disconnect ' \+ we\.id\)/);
  assert.match(pump, /ngOpenAddMenu\(\{ x: g\.x - NODE_W \/ 2/);
  // A wire dropped on empty space adds a node that takes it.
  assert.match(pump, /ngOpenAddMenu\(\{ x: g\.x, y: g\.y - NODE_HDR - 18 \}, ge\.from\)/);
});

test('Delete and Ctrl+D act on the selected node on the NODES tab, never on Final Output', () => {
  assert.match(js, /keyHooks\.push\(function \(name\) \{\s*if \(activeTab !== 'nodes'\) return;\s*var n = nodesSelected\(\); if \(!n \|\| n\.type === 'output'\) return;/);
  assert.match(js, /name === 'delete'\) nodesCmd\('remove '/);
  assert.match(js, /name === 'duplicate'\) nodesCmd\('duplicate '/);
});

test('clicking a mask input or a list row restyles the buttons instead of rebuilding them', () => {
  const wire = grab('nodeWireSync');
  // The rebuild signature holds the selection and the masks, not the wire the click changes.
  assert.match(wire, /var sig = \(wants \? sel\.id : -1\) \+ '\|' \+ masks\.map/);
  assert.match(wire, /setBtnOn\(b, cur \? b\.__maskId === cur\.from : b\.__maskId === -1/);
  const list = grab('nodesListSync');
  assert.match(list, /var setSig = nodes\.map\(function \(n\) \{ return n\.id; \}\)\.join/);
});

test('the graph nodes list scrolls inside a fixed box', () => {
  assert.match(js, /var nodesWell = scrollWell\(nodesSec, 6 \* 32\);/);
});

test('the add search matches names and keywords and only offers nodes a dropped wire can feed', () => {
  const ctx = vm.createContext({ T: (s) => s });
  vm.runInContext(/var NODE_TYPES = \[[\s\S]*?\];/.exec(js)[0] + grab('ngTypeMatches') + grab('ngLinkPort'), ctx);
  const find = (t) => ctx.NODE_TYPES.find((x) => x.t === t);
  assert.equal(ctx.ngTypeMatches(find('glow'), 'bloom'), true);
  assert.equal(ctx.ngTypeMatches(find('grade'), 'SATUR'), true);
  assert.equal(ctx.ngTypeMatches(find('sky_mask'), 'grade'), false);
  const mask = { mask: true }, image = { mask: false };
  assert.equal(ctx.ngLinkPort(mask, find('glow')), 1);
  assert.equal(ctx.ngLinkPort(image, find('glow')), 0);
  assert.equal(ctx.ngLinkPort(mask, find('mask_combine')), 0);
  assert.equal(ctx.ngLinkPort(image, find('mask_combine')), -1);
  assert.equal(ctx.ngLinkPort(image, find('depth_range')), -1);
});

test('the model refuses wires that would leave Final Output dark, and can duplicate and unlink', () => {
  assert.match(effectNodes, /const ConnectCheck c = CheckConnect\(g_graph, fromId, toId, toPort\);/);
  assert.match(effectNodes, /0 == _stricmp\(sub, "duplicate"\)/);
  assert.match(effectNodes, /0 == _stricmp\(sub, "unlink"\)/);
  assert.match(effectNodes, /\\"note\\":\\"/);
});
