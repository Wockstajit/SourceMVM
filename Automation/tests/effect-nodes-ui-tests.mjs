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

test('Frame all zooms out until the whole graph fits, and the pointer maps through the zoom', () => {
  const frame = grab('ngFrameAll');
  assert.match(frame, /ngZoom = Math\.max\(0\.3, Math\.min\(1, \(c\.w - margin\) \/ Math\.max\(x1 - x0, 1\), \(c\.h - margin\) \/ Math\.max\(y1 - y0, 1\)\)\);/);
  assert.match(frame, /ngPan\.x = Math\.round\(\(c\.w \/ ngZoom - \(x1 - x0\)\) \/ 2 - x0\);/);
  assert.match(grab('ngPump'), /var g = \{ x: cp\.x \/ ngZoom - ngPan\.x, y: cp\.y \/ ngZoom - ngPan\.y \};/);
  assert.match(grab('ngApplyZoom'), /scale3d\(/);
});

test('the wheel zooms about the pointer; Shift and Ctrl pan', () => {
  const wheel = grab('ngWheel');
  assert.match(wheel, /if \(st\.shift\) \{ ngPan\.x \+= notches \* 60 \/ ngZoom; return true; \}/);
  assert.match(wheel, /if \(st\.ctrl\) \{ ngPan\.y \+= notches \* 60 \/ ngZoom; return true; \}/);
  assert.match(wheel, /ngZoom = Math\.max\(0\.25, Math\.min\(2, ngZoom \* Math\.pow\(1\.15, notches\)\)\);/);
  assert.match(wheel, /ngPan\.x = cp\.x \/ ngZoom - gx; ngPan\.y = cp\.y \/ ngZoom - gy;/);
});

test('clicks and notches on the graph window stay on it, in either layout', () => {
  const pub = grab('ngPublishRect');
  assert.match(pub, /root\.SetAttributeString\('graphrect', r\)/);
  assert.match(grab('ngPump'), /function ngPump\(s\) \{\s*ngPublishRect\(\);/);
  const cpp = read('AfxHookSource2/Filmmaker/Panorama/ConfigHud.cpp');
  assert.match(cpp, /UiLayout_SetPanelRectString\(UiPanel::NodeGraph, m_bridge\.GetAttributeString\(m_root, m_symGraphRect, ""\)\.c_str\(\)\);/);
  assert.match(cpp, /void ConfigHud::OnExit\(\) \{\s*\/\/[^\n]*\n\s*UiLayout_SetPanelRect\(UiPanel::NodeGraph, 0, 0, 0, 0\);/);
  const layout = read('AfxHookSource2/Filmmaker/Panorama/UiLayout.cpp');
  assert.match(layout, /if \(Contains\(g_rects\[\(int\)UiPanel::NodeGraph\], x, y\)\) return UiPanel::NodeGraph;/);
  const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');
  assert.match(movie, /in\.onPanel = UiLayout_PanelAt\(x, y\) == UiPanel::NodeGraph;/);
  assert.match(movie, /at == UiPanel::NodeGraph\) && ConfigHud_Enabled\(\)\) \{ ConfigHud_PushWheel/);
  const widgets = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudWidgetsJs.h');
  assert.match(widgets, /cfgOverPanel\(host, mx, my\) && !ngCovers\(mx, my\)\)/);
});

test('a moved or resized window does not read its old rect back while the save is in flight', () => {
  const rd = grab('ngReadStored');
  assert.match(rd, /if \(ngPendingStore !== null\) \{/);
  assert.match(grab('ngPersistRect'), /ngPendingStore = v;/);
});

test('the zoomed layers are far larger than the canvas, so nothing clips at the scaled edge', () => {
  assert.match(js, /l\.style\.width = '12000px'; l\.style\.height = '12000px';/);
});

test('a node card repaints when one of its inputs is wired or unwired', () => {
  const paint = grab('ngPaintCard');
  assert.match(paint, /var wired = \(ngEdgeInto\(n\.id, 0\) \? 1 : 0\) \+ \(ngEdgeInto\(n\.id, 1\) \? 2 : 0\);/);
  assert.match(paint, /var sig = \[[^\]]*\bwired\b[^\]]*\]\.join/);
});

test('the graph nodes list scrolls inside a fixed box', () => {
  assert.match(js, /var nodesWell = scrollWell\(nodesSec, 6 \* 32\);/);
});

test('the add search matches names and keywords and only offers nodes a dropped wire can feed', () => {
  const ctx = vm.createContext({ T: (s) => s });
  vm.runInContext(/var NODE_TYPES = \[[\s\S]*?\];/.exec(js)[0] + /function ngIsMaskProcessor[^\n]*\n/.exec(js)[0]
    + grab('ngTypeMatches') + grab('ngLinkPort'), ctx);
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
  assert.equal(ctx.ngLinkPort(mask, find('mask_feather')), 0);
  assert.equal(ctx.ngLinkPort(image, find('vignette')), 0);
  assert.equal(ctx.ngTypeMatches(find('light_rays'), 'god'), true);
});

test('every node type the tab offers exists in the model, with an inspector schema', () => {
  const math = read('AfxHookSource2/Filmmaker/EffectNodes/EffectNodesMath.h');
  const types = [...js.matchAll(/\{ t: '([a-z_]+)', name:/g)].map((m) => m[1]);
  assert.ok(types.length >= 38, 'only ' + types.length + ' node types');
  const schemas = /var NODE_SCHEMAS = \{([\s\S]*?)\n    \};/.exec(js)[1];
  for (const t of types) {
    assert.match(math, new RegExp('return "' + t + '";'), t + ' has no token in EffectNodesMath.h');
    assert.match(schemas, new RegExp('\\b' + t + ':\\s*\\['), t + ' has no inspector schema');
  }
});

test('Starter presets is a dropdown, like the other preset pickers', () => {
  assert.match(js, /var nodesPresetDrop = dropdownRow\(nodesSec, T\('Starter presets'\), NODE_PRESETS,/);
  assert.doesNotMatch(js, /optionGrid\(nodesSec, T\('Starter presets'\)/);
});

test('the model refuses wires that would leave Final Output dark, and can duplicate and unlink', () => {
  assert.match(effectNodes, /const ConnectCheck c = CheckConnect\(g_graph, fromId, toId, toPort\);/);
  assert.match(effectNodes, /0 == _stricmp\(sub, "duplicate"\)/);
  assert.match(effectNodes, /0 == _stricmp\(sub, "unlink"\)/);
  assert.match(effectNodes, /\\"note\\":\\"/);
});

// Every node shows its settings on its card, as on a Blender node: a slider row per number, a click
// toggles an On/Off row, and a '(0 Name, 1 Name)' label is a choice that a click steps through.
test('node cards carry their settings and parse choice lists', () => {
  const box = { T: (s) => s, NODE_SCHEMAS: {}, ngParamLive: {} };
  vm.createContext(box);
  vm.runInContext(grab('ngParamInfo') + grab('ngParamText') + grab('ngNextOption'), box);
  const warp = box.ngParamInfo([0, 'Mode (0 Ripple, 1 Swirl, 2 Wave, 3 Bulge, 4 Pinch)', 0, 4, 0, '']);
  assert.equal(warp.name, 'Mode');
  assert.equal(warp.opts.length, 5);
  assert.equal(box.ngParamText(warp, 1), 'Swirl');
  assert.equal(box.ngNextOption(warp, 4), 0);
  const ja = box.ngParamInfo([0, '合成 (0 比較(明), 1 加算, 2 にじみ)', 0, 2, 0, '']);
  assert.equal(ja.name, '合成');
  assert.equal(box.ngParamText(ja, 0), '比較(明)');
  const amount = box.ngParamInfo([1, 'Strength', 0, 4, 2, 'x']);
  assert.equal(amount.opts, null);
  assert.equal(box.ngParamText(amount, 1.5), '1.50x');
  assert.equal(box.ngParamText(box.ngParamInfo([6, 'Hide when blocked', 0, 1, 0, '', true]), 1), 'On');
  // The rows are hit before the card body, dragged as sliders, and a new overlapping graph is arranged.
  assert.match(js, /return \{ kind: 'param', n: m, row: row, info: ngParamInfo\(sch\[row\]\) \};/);
  assert.match(grab('ngParamDrag'), /inf\.min \+ frac \* \(inf\.max - inf\.min\)/);
  assert.match(js, /if \(fresh && ngOverlaps\(\)\) ngTidyPending = true;/);
  assert.match(js, /tbtn\(T\('Arrange'\), function \(\) \{ ngTidy\(\); \}\);/);
});

// A Player Matte keeps to the players: what stands in front of the world reference must also be near a
// player's body (plants sway between the two renders), the first-person arms and gun stay out unless
// asked for, and a smoke hides a player unless the node sees through it.
test('Player Matte ignores swaying plants and the viewmodel, and smoke hides players', () => {
  const render = read('AfxHookSource2/Filmmaker/EffectNodes/EffectNodesRender.cpp');
  const hooks = read('AfxHookSource2/RenderSystemDX11Hooks.cpp');
  const matte = read('AfxHookSource2/Filmmaker/Render/RenderPlayerMatte.cpp');
  assert.match(render, /\? InFrontOfWorld\(w\) \* NearAnyPlayer\(w\)/);
  assert.match(render, /if \(here < 48\.0 && p1\.x < 0\.5\) isPlayer = 0\.0;/);
  assert.match(render, /if \(p0\.w < 0\.5\) isPlayer \*= 1\.0 - SmokeCover\(w\);/);
  assert.match(hooks, /Filmmaker::EffectNodes::SetSmokes\(spheres, count\);/);
  assert.match(hooks, /\(Filmmaker::LensFx_DofOn\(\) \|\| Filmmaker::EffectNodes::Active\(\)\)/);
  assert.match(matte, /WorldDecals::AnyEnabled\(\) \|\| nodes;/);
  assert.match(nodes, /\[3, T\('See through smoke'\), 0, 1, 0, '', true\], \[4, T\('First-person arms'\), 0, 1, 0, '', true\]/);
});
