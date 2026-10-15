// Does the Deadlock browser mockup still tell the truth about the game?
//
//   node --test Automation/tests/deadlock-mockup-tests.mjs
//
// The mockup's whole value is that it runs the REAL in-game UI code, so the only way it can start
// lying is at its edges: the extraction that pulls the code out, the round trip that puts edits back,
// and the two places where it re-implements something C++ owns (the layout solver and the dock
// metrics). Those edges are what this file guards. It never launches anything.
//
// Its own script with its own entry point, deliberately. UiMockup/ has an equivalent for CS2 and
// the two share nothing: different extraction model, different shim, different state feed.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { tokenize, unflatten, flattenMacro } from '../../UiMockupDeadlock/lib/deadlock-macros.mjs';
import { fragments, runScriptOrder, macroIndex } from '../../UiMockupDeadlock/lib/fragments.mjs';
import { extract, VIEWS, HOTKEYS } from '../../UiMockupDeadlock/extract-deadlock-ui.mjs';
import { parseBindings, bindings } from '../../UiMockupDeadlock/lib/hotkeys.mjs';
import { apply } from '../../UiMockupDeadlock/apply-deadlock-ui.mjs';
import * as L from '../../UiMockupDeadlock/lib/workspace-layout.mjs';

const REPO = path.resolve(import.meta.dirname, '..', '..');
const PANO = path.join(REPO, 'AfxHookSource2', 'Filmmaker', 'Deadlock');
const DRIVER = path.join(PANO, 'DeadlockWorkspaces.cpp');
const MOCKUP = path.join(REPO, 'UiMockupDeadlock');

const near = (a, b, tol = 1e-6) => Math.abs(a - b) < tol;

// ---------------------------------------------------------------- extraction

test('the fragment list comes from the C++, not from a list kept by hand', () => {
  const order = runScriptOrder(DRIVER);
  assert.ok(order.length >= 10, `only ${order.length} fragments found -- the driver shape changed`);

  // The style fragment defines the helpers every other one calls, so it cannot be anywhere but
  // first. The shell's build() asks each docked editor to build into the dock body, so it cannot be
  // anywhere but last. Both are properties of the C++ ordering, and both are silent when wrong: the
  // symptom is a missing dock, which looks like a layout bug and gets chased in the wrong file.
  assert.equal(order[0], 'DEADLOCK_STYLE_JS', 'the style fragment must load first');
  assert.equal(order[order.length - 1], 'DEADLOCK_WORKSPACE_JS', 'the shell must load last');

  const index = macroIndex(PANO);
  for (const macro of order) {
    assert.ok(index[macro], `${macro} is pushed by the driver but no header defines it`);
  }
});

test('every macro extracts to a non-empty script', () => {
  for (const f of fragments(PANO, DRIVER)) {
    const js = flattenMacro(PANO, f.file, f.macro);
    assert.ok(js.trim().length > 200, `${f.macro} extracted only ${js.length} chars`);
    // The marker count has to match the literal count, or the applier cannot find a slot to write
    // one of them back into.
    const src = fs.readFileSync(path.join(PANO, f.file), 'utf8');
    const macro = tokenize(src, f.file).find((m) => m.name === f.macro);
    const markers = (js.match(/@mvm:seg /g) || []).length;
    assert.equal(markers, macro.literals.length,
      `${f.macro}: ${markers} markers for ${macro.literals.length} literals`);
  }
});

test('a split macro keeps its literals separate', () => {
  // MSVC caps one raw literal at 16380 bytes and TRUNCATES SILENTLY past it, so the long fragments
  // are split with a `)CFJS" R"CFJS(` seam on a single line. The seam must stay on one line: a
  // newline between the two would end the #define and the rest would become stray C++. Extraction
  // has to see both halves as one macro, and the applier has to be able to tell them apart again.
  const split = fragments(PANO, DRIVER)
    .map((f) => {
      const src = fs.readFileSync(path.join(PANO, f.file), 'utf8');
      return { ...f, macro: tokenize(src, f.file).find((m) => m.name === f.macro) };
    })
    .filter((f) => f.macro.literals.length > 1);
  assert.ok(split.length >= 3, 'expected several split macros; the fragments shrank unexpectedly');
  for (const f of split) {
    const js = flattenMacro(PANO, f.file, f.macro.name);
    const bodies = unflatten(js);
    assert.equal(bodies[f.file][f.macro.name].length, f.macro.literals.length);
  }
});

test('no literal is near the size MSVC truncates at', () => {
  for (const f of fragments(PANO, DRIVER)) {
    const src = fs.readFileSync(path.join(PANO, f.file), 'utf8');
    const macro = tokenize(src, f.file).find((m) => m.name === f.macro);
    macro.literals.forEach((lit, n) => {
      const bytes = Buffer.byteLength(src.slice(lit.bodyStart, lit.bodyEnd), 'utf8');
      assert.ok(bytes <= 16380,
        `${f.file}#${f.macro} literal ${n} is ${bytes} bytes -- MSVC truncates it without a word`);
    });
  }
});

// ---------------------------------------------------------------- the round trip

test('extract -> apply leaves every header byte for byte unchanged', () => {
  // This is what makes editing in the browser a real round trip rather than a re-implementation. It
  // runs against a COPY, so a bug in the applier cannot damage the tree while proving itself.
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-deadlock-roundtrip-'));
  try {
    const panoCopy = path.join(tmp, 'pano');
    fs.mkdirSync(panoCopy);
    const before = new Map();
    for (const file of fs.readdirSync(PANO)) {
      if (!/Js\.h$/.test(file)) continue;
      const buf = fs.readFileSync(path.join(PANO, file));
      fs.writeFileSync(path.join(panoCopy, file), buf);
      before.set(file, buf);
    }
    fs.copyFileSync(DRIVER, path.join(panoCopy, path.basename(DRIVER)));

    const outDir = path.join(tmp, 'generated');
    extract(panoCopy, path.join(panoCopy, path.basename(DRIVER)), outDir, () => {});
    apply(panoCopy, path.join(outDir, 'workspace.js'), {});

    for (const [file, buf] of before) {
      const after = fs.readFileSync(path.join(panoCopy, file));
      assert.ok(after.equals(buf), `${file} changed across extract -> apply`);
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test('generated/ and the headers agree', () => {
  // Fails when someone edited a header and did not re-extract, which would otherwise show as a
  // browser that quietly disagrees with the source it claims to be running.
  const generated = path.join(MOCKUP, 'generated', 'workspace.js');
  if (!fs.existsSync(generated)) return; // not extracted yet; the bat does it on every run
  const report = apply(PANO, generated, { check: true });
  assert.deepEqual(report.changed, [],
    'generated/workspace.js is out of date -- run extract-deadlock-ui.mjs');
});

test('the applier refuses a body that would break the C++', () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-deadlock-refuse-'));
  try {
    const panoCopy = path.join(tmp, 'pano');
    fs.mkdirSync(panoCopy);
    for (const file of fs.readdirSync(PANO)) {
      if (/Js\.h$/.test(file)) fs.copyFileSync(path.join(PANO, file), path.join(panoCopy, file));
    }
    fs.copyFileSync(DRIVER, path.join(panoCopy, path.basename(DRIVER)));
    const outDir = path.join(tmp, 'generated');
    extract(panoCopy, path.join(panoCopy, path.basename(DRIVER)), outDir, () => {});
    const gen = path.join(outDir, 'workspace.js');

    // A closer inside the body would end the raw literal early and spill the rest of the JS into
    // the C++ as syntax.
    const text = fs.readFileSync(gen, 'utf8');
    fs.writeFileSync(gen, text.replace('(function () {', '(function () { /* )CFJS" */'), 'utf8');
    assert.throws(() => apply(panoCopy, gen, {}), /CFJS/, 'a stray closer was accepted');

    // A literal over MSVC's cap would be truncated silently, which reads in game as "the UI is just
    // ugly" rather than as an error.
    fs.writeFileSync(gen, text.replace('(function () {', '(function () {' + '//' + 'x'.repeat(17000) + '\n'), 'utf8');
    assert.throws(() => apply(panoCopy, gen, {}), /truncate/, 'an oversized literal was accepted');
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------- the layout port

// The mockup re-implements WorkspaceLayout_Solve, because in game C++ solves it and the panel draws
// nothing without an answer. Two copies of one piece of arithmetic can disagree, so these are the
// SAME cases Automation/tests/deadlock-workspace-layout-math-tests.cpp asserts against the C++.
test('the layout port matches the C++ solver on its own test vectors', () => {
  {
    const out = L.solve({ screenW: 1920, screenH: 1080, inspectorW: 520, dockH: 200, headerH: 44 });
    assert.ok(out.valid, '1920x1080 solves');
    assert.ok(near(out.inspectorW, 520), 'column keeps its requested width');
    assert.ok(near(out.areaW, 1400), 'area is screen minus column');
    assert.ok(near(out.areaH, 836), 'area is screen minus header and dock');
    assert.ok(near(out.areaY, 44), 'area starts below the header');
    assert.ok(near(out.previewW, 1400), "preview fills the band's width");
    assert.ok(near(out.previewH, 787), 'preview height follows the world aspect');
    assert.ok(out.previewY > 44, 'preview is centred vertically');
    assert.ok(near(out.blitX1 - out.blitX0, 1400 / 1920), 'blit width matches the preview');
  }
  {
    const out = L.solve({ screenW: 1920, screenH: 1080, inspectorW: 1000, dockH: 0, headerH: 0 });
    assert.ok(near(out.areaW, 920), 'area narrows with a wide column');
    const t = L.solve({ screenW: 1920, screenH: 1080, inspectorW: 1000, dockH: 700, headerH: 0 });
    assert.ok(near(t.areaH, 380), 'area shortens with a tall dock');
    assert.ok(near(t.previewH, 380), 'preview fills the height when height-bound');
    assert.ok(t.previewW < t.areaW, 'preview letterboxes at the sides');
    assert.ok(t.previewX > 0, 'preview is centred horizontally');
  }
  {
    assert.ok(near(L.clampInspector(100, 1920), 320), 'column floors at 320');
    assert.ok(near(L.clampInspector(5000, 1920), Math.floor(1920 * 0.62)), 'column caps at 62%');
    assert.ok(near(L.clampInspector(0, 1920), 0), 'no column means no column');
    assert.ok(near(L.clampInspector(520, 400), 320), 'column floor survives a tiny screen');
  }
  {
    assert.ok(!L.solve({ screenW: 0, screenH: 1080 }).valid, 'zero width is invalid');
    assert.ok(!L.solve({ screenW: NaN, screenH: 1080 }).valid, 'NaN width is invalid');
    assert.ok(!L.solve({ screenW: 1920, screenH: 1080, inspectorW: 520, dockH: 1060, headerH: 44 }).valid,
      'a dock taller than the screen is invalid');
  }
  {
    const out = L.solve({ screenW: 3440, screenH: 1440, inspectorW: 520, dockH: 200, headerH: 44 });
    assert.ok(out.valid && near(out.areaW, 2920), 'ultrawide band is wide');
    assert.ok(near(out.previewH, 1196), 'ultrawide preview is height-bound');
    assert.ok(out.previewX > 0, 'ultrawide preview letterboxes at the sides');
  }
  {
    const out = L.solve({ screenW: 1600, screenH: 1200, inspectorW: 400, dockH: 0, headerH: 0, worldAspect: 4 / 3 });
    assert.ok(near(out.previewW, 1200), '4:3 preview width follows the band height');
    assert.ok(near(out.previewH, 900), '4:3 preview height follows its aspect');
  }
  {
    assert.ok(near(L.tickToX(0, 0, 100, 10, 200), 10), 'first tick maps to strip start');
    assert.ok(near(L.tickToX(100, 0, 100, 10, 200), 210), 'last tick maps to strip end');
    assert.ok(near(L.tickToX(50, 0, 100, 10, 200), 110), 'mid tick maps to strip middle');
  }
});

test('the port uses the same constants the C++ header does', () => {
  // The realistic drift is not a rewritten formula, it is someone retuning a number in the header.
  // Reading them back out of the C++ turns that into a failure here instead of a mockup that quietly
  // lays out differently from the game.
  const src = fs.readFileSync(path.join(PANO, 'DeadlockWorkspaceLayoutMath.h'), 'utf8');
  const grab = (re, what) => {
    const m = re.exec(src);
    assert.ok(m, `could not read ${what} out of DeadlockWorkspaceLayoutMath.h`);
    return parseFloat(m[1]);
  };
  assert.equal(L.MIN_INSPECTOR, grab(/std::max\(([\d.]+), requested\)/, 'the column floor'));
  assert.equal(L.MAX_INSPECTOR_FRACTION, grab(/screenW \* ([\d.]+)/, 'the column cap'));
  assert.equal(L.MIN_AREA_W, grab(/out\.areaW < (\d+)/, 'the minimum band width'));
  assert.equal(L.MIN_AREA_H, grab(/out\.areaH < (\d+)/, 'the minimum band height'));
});

test('the mockup reserves the same dock heights the C++ does', () => {
  // shim/state.js stands in for DeadlockDock.cpp, which owns these. A mismatch would put the
  // mockup's preview and its dock band in different places from the game's.
  const src = fs.readFileSync(path.join(PANO, 'DeadlockDock.cpp'), 'utf8');
  const grab = (name) => {
    const m = new RegExp(`const int ${name} = (\\d+);`).exec(src);
    assert.ok(m, `could not read ${name} out of DeadlockDock.cpp`);
    return Number(m[1]);
  };
  const state = fs.readFileSync(path.join(MOCKUP, 'shim', 'state.js'), 'utf8');
  const mock = (name) => {
    const m = new RegExp(`var ${name} = (\\d+);`).exec(state);
    assert.ok(m, `could not read ${name} out of shim/state.js`);
    return Number(m[1]);
  };
  assert.equal(mock('RIBBON_H'), grab('kRibbonHeight'), 'ribbon height');
  assert.equal(mock('NATIVE_FALLBACK_H'), grab('kNativeFallback'), 'native fallback height');
  assert.equal(mock('CAMERA_H'), grab('kCameraHeight'), 'camera dock height');
  assert.equal(mock('GRAPH_H'), grab('kGraphHeight'), 'graph dock height');

  const ws = fs.readFileSync(path.join(PANO, 'DeadlockWorkspaces.cpp'), 'utf8');
  const wsNum = (name) => {
    const m = new RegExp(`const double ${name} = ([\\d.]+);`).exec(ws);
    assert.ok(m, `could not read ${name} out of DeadlockWorkspaces.cpp`);
    return parseFloat(m[1]);
  };
  // The DEFAULT: the width is a setting now (the column has a drag grip), and this is what it
  // starts at in both.
  assert.equal(mock('INSPECTOR_W'), wsNum('kInspectorWDefault'), 'inspector width default');
  assert.equal(mock('HEADER_H'), wsNum('kHeaderH'), 'header height');
});

// ---------------------------------------------------------------- the command router

test('the router handles every command the UI can actually issue', () => {
  // A control whose command nothing handles is a control that does nothing in the browser, which is
  // the half-working state that makes a mockup untrustworthy. The list is read out of the panel
  // source, so a new control shows up here rather than being noticed by eye.
  const subs = new Set();
  for (const f of fragments(PANO, DRIVER)) {
    const js = flattenMacro(PANO, f.file, f.macro);
    for (const m of js.matchAll(/cmd\(\s*'mirv_(?:deadlock|filmmaker) ([a-z_]+)/g)) subs.add(m[1]);
    for (const m of js.matchAll(/M\.cmd\(\s*'mirv_(?:deadlock|filmmaker) ([a-z_]+)/g)) subs.add(m[1]);
    // delegate('NativeButton', 'mirv_filmmaker verb ...') carries its fallback command as a plain
    // string argument; missing it is how `seekrel` shipped with no handler.
    for (const m of js.matchAll(/delegate\(\s*'[^']*'\s*,\s*'mirv_(?:deadlock|filmmaker) ([a-z_]+)/g)) subs.add(m[1]);
  }
  assert.ok(subs.size >= 10, `only ${subs.size} subcommands found; the scan stopped matching`);

  const router = fs.readFileSync(path.join(MOCKUP, 'shim', 'commands.js'), 'utf8');
  const handled = new Set(
    // No assumption about the parameter list: `scrubend` takes none, because the engine only needs
    // telling that the drag finished.
    [...router.matchAll(/^\s{4}([a-z]+): function \(/gm)].map((m) => m[1])
  );
  const missing = [...subs].filter((s) => !handled.has(s));
  assert.deepEqual(missing, [],
    `the UI issues these with no mockup handler: ${missing.join(', ')}`);
});

// ---------------------------------------------------------------- the views

test('the views cover both axes of the workspace', () => {
  // The workspace has two independent axes -- which inspector is open, and which editor is docked --
  // and conflating them is the bug that once put the whole timeline down the right-hand side. The
  // picker has to offer both, or a whole half of the UI is unreachable in the browser.
  const workspaces = new Set(VIEWS.map((v) => v.workspace));
  const tabs = new Set(VIEWS.map((v) => v.tab));
  for (const w of ['camera', 'config', 'render', 'off']) {
    assert.ok(workspaces.has(w), `no view opens the ${w} workspace`);
  }
  for (const t of ['native', 'camera', 'graph']) {
    assert.ok(tabs.has(t), `no view docks the ${t} editor`);
  }
  assert.equal(new Set(VIEWS.map((v) => v.id)).size, VIEWS.length, 'duplicate view id');
  // The boot state. The page opens the way the game does after a replay loads -- nothing of ours
  // open -- and that view must exist and must not be labelled as something only the game can show.
  const closed = VIEWS.find((v) => v.id === 'closed');
  assert.ok(closed && closed.workspace === 'off', 'no closed view to boot into');
  assert.ok(!/game only/i.test(closed.label + closed.note), 'the closed view is reachable in the browser now');
});

// ---------------------------------------------------------------- the hotkeys

test('the hotkeys are read out of DeadlockHotkeys.cpp, gate and all', () => {
  // The browser feeds the same rows to the same gate the hook uses. A key added or re-bound in the
  // C++ has to show up here on the next extraction; a row this parser cannot read would be a key
  // that silently does nothing in the browser.
  const rows = parseBindings(HOTKEYS);
  const byVk = Object.fromEntries(rows.map((r) => [r.vk, r]));
  for (const vk of ['K', 'F', 'L', 'VK_SPACE', 'VK_LEFT', 'VK_RIGHT', 'VK_F8']) {
    assert.ok(byVk[vk], `no binding parsed for ${vk}`);
  }
  // SPACE and the arrows collide with Deadlock's own spectator HUD, so they need the workspace open;
  // K and F8 do not (DeadlockHotkeys.cpp's own reasoning, in Binding::requiresWorkspace).
  for (const vk of ['VK_SPACE', 'VK_LEFT', 'VK_RIGHT']) assert.equal(byVk[vk].requiresWorkspace, true, `${vk} must need the workspace`);
  for (const vk of ['K', 'F', 'L', 'VK_F8']) assert.equal(byVk[vk].requiresWorkspace, false, `${vk} works anywhere`);
  assert.equal(byVk.VK_SPACE.resolve, 'PlayPauseCommand', 'SPACE resolves at press time');
  assert.equal(byVk.VK_F8.command, 'mirv_filmmaker blackout toggle');
  // And every row maps to a browser key, or the extractor would have thrown.
  const mapped = bindings(HOTKEYS);
  assert.equal(mapped.find((b) => b.vk === 'VK_SPACE').key, ' ');
  assert.equal(mapped.find((b) => b.vk === 'VK_F8').key, 'F8');
  // The hook's own engine-level lines (what SPACE and the arrows run) have to be routable, or the
  // key fires in the browser and nothing moves.
  const router = fs.readFileSync(path.join(MOCKUP, 'shim', 'commands.js'), 'utf8');
  assert.match(router, /parts\[0\] === 'demo_pause'/, 'the router does not accept demo_pause');
  assert.match(router, /parts\[0\] === 'mirv_skip'/, 'the router does not accept mirv_skip');
  assert.match(router, /^\s{4}blackout: function \(/m, 'no blackout handler for F8');
});

// ---------------------------------------------------------------- the state projection

test('the projection carries the fields BuildStateJson does that the panels read at rest', () => {
  // These are what the closed state, the value tiles, the graph menu and the director card render
  // from. Missing, each one is a panel that quietly renders wrong in the browser.
  const src = fs.readFileSync(path.join(MOCKUP, 'shim', 'state.js'), 'utf8');
  const win = {};
  new Function('window', src)(win);
  const S = win.MvmDeadlockState;
  const m = S.initial();
  assert.equal(m.open, false, 'the model boots closed, like the game after a replay loads');
  const solver = () => ({ valid: true, inspectorW: 0, leftW: 0, frameTopY: 0, frameBottomY: 0, bandBottomY: 0, areaX: 0, areaY: 0, areaW: 1920, areaH: 1080, previewX: 0, previewY: 0, previewW: 1920, previewH: 1080 });
  const closed = S.project(m, solver);
  assert.equal(closed.blackout, false);
  assert.equal(typeof closed.numberKeys, 'number');
  assert.ok(closed.graph && closed.graph.menu && closed.graph.menu.open === false, 'graph.menu missing');
  assert.ok(closed.director && closed.director.visible === true, 'the director card shows while the workspace is closed');
  // DeadlockMovieHud.cpp: hidden whenever a workspace is open, on F8, and while a take writes.
  m.open = true;
  assert.equal(S.project(m, solver).director.visible, false, 'director card must hide when a workspace is open');
  m.open = false; m.blackout = true;
  assert.equal(S.project(m, solver).director.visible, false, 'director card must hide on F8');
  m.blackout = false; m.renderState = 'Capturing';
  assert.equal(S.project(m, solver).director.visible, false, 'director card must hide while capturing');
  m.renderState = 'Idle'; m.uiMouse = false; m.numberKeys = 5;
  assert.equal(S.project(m, solver).numberKeys, 0, 'numberKeys only reach the panel in UI-mouse');
});

// ---------------------------------------------------------------- no engine bindings

test('the shim withholds the bindings Deadlock does not have', () => {
  // Measured in game on build 6687 by enumerating the global object: this Panorama context has no
  // GameInterfaceAPI and no UiToolkitAPI, and CitadelConCommand is bound per-XML-context and is
  // undefined here. A shim that provided them would let a panel reach for one, work perfectly in the
  // browser, and be a dead control in game -- the single most expensive mistake this harness exists
  // to prevent. So the absence is a FEATURE and it is asserted.
  const shim = fs.readFileSync(path.join(MOCKUP, 'shim', 'panorama.js'), 'utf8');
  assert.match(shim, /delete global\.GameInterfaceAPI/, 'the shim must not provide GameInterfaceAPI');
  assert.match(shim, /delete global\.UiToolkitAPI/, 'the shim must not provide UiToolkitAPI');
  assert.ok(!/global\.GameInterfaceAPI\s*=\s*\{/.test(shim), 'the shim defines GameInterfaceAPI');
  assert.ok(!/global\.UiToolkitAPI\s*=\s*\{/.test(shim), 'the shim defines UiToolkitAPI');

  // And the command marker has to be the one the C++ listener splits on, or every control in the
  // browser would issue commands the game would never see.
  // The marker is DEFINED in the .cpp; the header only declares it.
  const bridge = fs.readFileSync(
    path.join(REPO, 'AfxHookSource2', 'Deadlock', 'DeadlockUiCommandBridge.cpp'), 'utf8');
  const m = /kUiCommandMarker\s*=\s*"((?:[^"\\]|\\.)*)"/.exec(bridge);
  assert.ok(m, 'could not read kUiCommandMarker out of DeadlockUiCommandBridge.cpp');
  assert.ok(shim.includes(`var CMD_MARKER = '${m[1]}'`),
    `the shim's marker does not match the C++ (${m[1]})`);

  // And the length the listener compares against has to match the marker it was measured from, or
  // the bridge would split the line in the wrong place.
  const len = /kMarkerLength = (\d+)/.exec(bridge);
  assert.ok(len, 'could not read kMarkerLength');
  assert.equal(Number(len[1]), m[1].replace(/\\x02/, '\x02').length,
    'kMarkerLength disagrees with kUiCommandMarker');
});
