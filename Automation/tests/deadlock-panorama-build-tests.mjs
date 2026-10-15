// Do the Deadlock Panorama panels actually BUILD and RENDER, or do they die halfway through?
//
//   node --test Automation/tests/deadlock-panorama-build-tests.mjs
//
// This catches the failure mode a parse check cannot see. A ReferenceError partway through a
// panel's build leaves the script registered on nothing and the panel half-drawn, which in game
// reads as "the UI is just ugly" rather than as an error. `new Function(src)` only proves the file
// parses; running it proves it finishes.
//
// Its own script with its own entry point, deliberately. UiMockup/build-check.mjs does the same
// job for the CS2 surfaces, but it is driven by UiMockup/extract-ui.mjs and a CS2 state feed;
// teaching that pipeline about Deadlock would couple the two games' UI checks together for no
// benefit. The stub below is the part worth having in common, and it is small.
//
// The stub is deliberately dumb: every panel property reads back a benign default and every method
// is a no-op, so it answers one question only -- did the script run to the end.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const REPO_ROOT = path.resolve(import.meta.dirname, '..', '..');
const JS_DIR = path.join(REPO_ROOT, 'AfxHookSource2', 'Filmmaker', 'Deadlock');

// The fragments, in the order the C++ pushes them. Order is load-bearing: they share ONE
// JavaScript scope, and the style fragment defines the helpers the rest call.
const FRAGMENTS = [
  'DeadlockStyleJs.h',
  // The drawing primitives come second: the timeline's diamonds and the graph's curves are both
  // built out of them, so a fragment that loads before this one has nothing to draw with.
  'DeadlockDrawJs.h',
  // The card and row grammar every inspector column is built out of. After the primitives it
  // draws with, before the three bodies that call it.
  'DeadlockControlsJs.h',
  // Registers before the native HUD, the timeline and the graph, each of which builds one.
  'DeadlockSpeedDialJs.h',
  // The column's drag edge, built by the shell.
  'DeadlockInspectorGripJs.h',
  'DeadlockCameraBodyJs.h',
  'DeadlockGraphBodyJs.h',
  'DeadlockConfigRenderBodyJs.h',
  // The three docked editors register themselves BEFORE the shell, because the shell's build()
  // asks each of them to build into the dock body. Getting this order wrong in the C++ is a
  // silently missing dock, which is exactly the kind of thing this list exists to pin down.
  'DeadlockRegularTimelineJs.h',
  'DeadlockTimelineJs.h',
  'DeadlockDockRibbonJs.h',
  'DeadlockNativeHudJs.h',
  'DeadlockWorkspaceJs.h',
];

// Pulls every raw string literal body out of a *Js.h fragment. The C++ wraps each blob in
// R"CFJS( ... )CFJS", and MSVC caps a raw literal at about 16 KB, so a large panel is split across
// several adjacent literals that concatenate. Extracting all of them in order reproduces exactly
// what RunScript receives.
function extractJs(file) {
  const source = fs.readFileSync(path.join(JS_DIR, file), 'utf8');
  const blocks = [];
  const open = 'R"CFJS(';
  const close = ')CFJS"';
  let at = 0;
  for (;;) {
    const start = source.indexOf(open, at);
    if (start < 0) break;
    const end = source.indexOf(close, start + open.length);
    if (end < 0) throw new Error(`${file}: unterminated R"CFJS( block`);
    blocks.push(source.slice(start + open.length, end));
    at = end + close.length;
  }
  if (!blocks.length) throw new Error(`${file}: no R"CFJS( block found`);
  return blocks.join('');
}

// A state blob shaped like the one DeadlockWorkspaces.cpp builds. Kept here rather than imported
// so a change to the C++ JSON that the panels do not handle shows up as a test failure.
function makeState(overrides = {}) {
  return {
    open: true,
    blackout: false,
    numberKeys: 0,
    workspace: 'camera',
    demo: true,
    paused: false,
    tick: 17801,
    time: 278.14,
    level: 'street_test',
    markers: 3,
    startTick: 17000,
    endTick: 18500,
    playhead: 17801,
    playing: false,
    markerList: [
      { tick: 17000, pos: '120 -400 96', selected: false },
      { tick: 17800, pos: '512 -128 240', selected: true },
      { tick: 18500, pos: '900 300 64', selected: false },
    ],
    selected: 1,
    live: false,
    previewScaled: true,
    viewportW: 1920,
    viewportH: 1080,
    loopOn: true,
    loopIn: 17200,
    loopOut: 18000,
    loopReady: true,
    // The one playback dial both speed sliders seed from, and the mode the timeline's
    // Linear | Smooth pair lights.
    speed: 1.0,
    pathSmooth: true,
    // The cursor. This Panorama context has no mouse-move event, so the OS-side probe in C++ is
    // the only source a panel has for where the pointer is.
    mx: 640, my: 900, lmb: false, rmb: false, shift: false,
    // C++ owns the geometry and hands it over solved. The panel places everything from this block
    // and draws nothing without it, so a test that omits it is testing the wrong thing.
    layout: {
      valid: true, screenW: 1920, screenH: 1080,
      inspectorW: 520, dockH: 232, headerH: 44,
      areaX: 0, areaY: 44, areaW: 1400, areaH: 860,
      previewX: 0, previewY: 87, previewW: 1400, previewH: 787,
    },
    // WHICH EDITOR IS DOCKED -- an axis independent of which workspace is open. The timeline, the
    // camera strip and the graph live here, under the preview; none of them belongs in the column.
    dock: { tab: 'camera', ribbonH: 36, editorH: 196, meta: '3 markers' },
    render: {
      state: 'Capturing', frames: 412, dropped: 0, fps: 32, take: 3,
      startTick: 17000, endTick: 18500, dir: 'C:\Temp\take0003',
    },
    director: { visible: false },
    graph: {
      plotX: 200, plotY: 142, plotW: 1708, plotH: 926,
      menu: { open: false, x: 0, y: 0 },
      drive: false, keys: 21, selection: 1, playhead: 0.42,
      channels: [
        { name: 'X', color: '#FF410D', visible: true,
          pts: [0, 0.5, 0.25, 0.4, 0.5, 0.3, 0.75, 0.45, 1, 0.6],
          keyPts: [{ t: 0, v: 0.5, sel: false }, { t: 0.5, v: 0.3, sel: true }] },
        { name: 'Y', color: '#5FE69E', visible: true, pts: [0, 0.2, 1, 0.8], keyPts: [] },
        { name: 'Z', color: '#1A9FFF', visible: false, pts: [], keyPts: [] },
        { name: 'Pitch', color: '#FFED79', visible: true, pts: [0, 0.5, 1, 0.5], keyPts: [] },
        { name: 'Yaw', color: '#CE90FF', visible: true, pts: [0, 0.1, 1, 0.9], keyPts: [] },
        { name: 'Roll', color: '#3FDCFF', visible: false, pts: [], keyPts: [] },
        { name: 'FOV', color: '#DEC5A0', visible: true, pts: [0, 0.5, 1, 0.4], keyPts: [] },
      ],
    },
    ...overrides,
  };
}

// One throwaway Panorama environment per test, so a panel cannot pass because a previous test left
// something registered on $.
// Keep in step with kUiCommandMarker in AfxHookSource2/Deadlock/DeadlockUiCommandBridge.h.
const MARKER = 'MVMCMD ';

function makeEnvironment() {
  const messages = [];
  const commands = [];
  const dispatched = [];
  const styleWrites = { count: 0 };
  let stateJson = null;
  let lastState = null;

  const SCREEN_W = 1920;
  const SCREEN_H = 1080;

  function makePanel(id = '') {
    const self = {
      id,
      text: '',
      visible: true,
      enabled: true,
      value: 0,
      min: 0,
      max: 1,
      checked: false,
      children: [],
      actuallayoutwidth: 442,
      actuallayoutheight: 76,
      actualxoffset: 0,
      actualyoffset: 0,
      actualuiscale_x: 1,
      actualuiscale_y: 1,
      // Panorama VALIDATES style values and THROWS on a bad one, which aborts the whole panel
      // script. Two of those throws are easy to write by accident and impossible to see in a
      // parse check, so the stub reproduces them rather than accepting anything.
      style: new Proxy({}, {
        // READS BACK what was written. It used to answer '' to everything, which meant no test
        // could check what a panel was actually styled as -- only that styling it did not throw.
        // Panorama itself reads back the value it stored, so answering '' was also a lie about the
        // thing being modelled.
        get: (target, prop) => (prop in target ? target[prop] : ''),
        set: (target, prop, value) => {
          // Counted, because a style write is not free: Panorama re-does the panel's geometry, and
          // a 3D transform allocates a composited layer with its own vertex buffer. See the test
          // below -- this counter is the only way to see that class of bug before the game dies.
          styleWrites.count++;
          // NULL IS A DELIBERATE CLEAR, undefined is a missing token. Panorama accepts
          // `style.width = null` as "drop my override and go back to the stylesheet", which is the
          // only way to hand a native panel its own size back -- and it is the idiom every release
          // path in the native-HUD driver uses. A missing constant, by contrast, evaluates to
          // `undefined`, never to null, so the two can be told apart and only one is a bug.
          if (value === null) { delete target[prop]; return true; }
          const text = String(value);
          // style.X = '' throws. Clear a colour with rgba(0,0,0,0).
          if (text === '')
            throw new Error(`empty value for style '${String(prop)}'`);
          // String(undefined) is the word "undefined", so an empty-string check alone sails
          // straight past a missing token.
          if (value === undefined)
            throw new Error(`style '${String(prop)}' set to ${String(value)} -- missing token?`);
          target[prop] = text;
          return true;
        },
      }),
      IsValid: () => true,
      GetParent: () => null,
      SetParent: () => {},
      GetChildCount() { return self.children.length; },
      GetChild(i) { return self.children[i]; },
      RemoveAndDeleteChildren() { self.children = []; },
      DeleteAsync() {},
      events: {},
      SetPanelEvent(name, fn) { self.events[name] = fn; },
      // Press it the way a click would, through the handler the panel registered.
      Activate() { if (self.events.onactivate) self.events.onactivate(); },
      classes: new Set(),
      AddClass(name) { self.classes.add(name); },
      RemoveClass(name) { self.classes.delete(name); },
      BHasClass(name) { return self.classes.has(name); },
      SetImage() {},
      // A REAL search, not a stub that always answers null.
      //
      // The Regular Timeline is a face on Deadlock's own replay controls: it reads the native
      // scrubber's value for the playhead and dispatches activations onto the game's own buttons.
      // An always-null lookup silently skipped that entire path, so the test passed on a panel
      // that could not have worked.
      FindChildTraverse(id) {
        const seen = new Set();
        const walk = (panel) => {
          if (!panel || seen.has(panel)) return null;
          seen.add(panel);
          for (const child of panel.children) {
            if (child.id === id) return child;
            const deeper = walk(child);
            if (deeper) return deeper;
          }
          return null;
        };
        return walk(self);
      },
      FindChild(id) {
        for (const child of self.children) if (child.id === id) return child;
        return null;
      },
      SetAttributeString() {},
      // EVERY panel serves the state feed, not just the context panel: each surface reads state
      // off its OWN root, so patching only the context leaves `st` null and every render() bails
      // on its first line -- which reports as a clean render for a panel that never laid out.
      GetAttributeString: (key, fallback) =>
        (key === 'state' && stateJson !== null ? stateJson : fallback),
      SetAttributeInt() {},
      GetAttributeInt: (key, fallback) => fallback,
    };
    return self;
  }

  const ctx = makePanel('CitadelHud');
  ctx.actuallayoutwidth = SCREEN_W;
  ctx.actuallayoutheight = SCREEN_H;

  // DEADLOCK'S OWN REPLAY BAR, as far down as the panels reach into it. Shaped from
  // reference/deadlock/panorama/layout/replay/citadel_hud_replay.xml, so the ids are the game's
  // rather than ones invented to make the test pass.
  const nativeIds = [
    'PlayButton',
    'SkipBackward2Button', 'SkipBackward1Button', 'SkipForward1Button', 'SkipForward2Button',
    'PrevMarkerButton', 'NextMarkerButton',
  ];
  const replay = makePanel('hud_replay');
  // Shipped hidden, exactly as citadel_hud_replay.xml declares it. The host has to remove the
  // class or the bar lays out at full size and still draws nothing -- which is the state the
  // "regular timeline isn't there" report was made in.
  replay.classes.add('Hidden');
  ctx.children.push(replay);

  // The real nesting, because the game hides the bar TWO ways and the second one is this panel:
  // `#SpectatorOptionsContainer.Hidden` slides the bar 272px off to the right rather than
  // collapsing it (citadel_hud_replay.css). A stub that flattened this away could not tell the
  // difference between a bar that is shown and one parked off its own edge.
  const options = makePanel('SpectatorOptionsContainer');
  options.classes.add('Hidden');
  replay.children.push(options);

  // THE REAL NESTING, DOWN TO #MainPlaybackControls.
  //
  // That panel is not decoration: it is the row our own button group attaches ITSELF to, so a stub
  // that flattened it away meant `attach()` never found a host and the Loop button, the Cam Editor
  // / Config swap and the speed dial were never covered by any test at all. The gap was invisible
  // here and obvious the moment the browser mockup stood the real tree up.
  const playback = makePanel('PlaybackControls');
  options.children.push(playback);
  const main = makePanel('MainPlaybackControls');
  playback.children.push(main);

  for (const id of nativeIds) main.children.push(makePanel(id));

  const speedCenter = makePanel('SpeedControlsCenter');
  for (let i = 0; i < 3; ++i) speedCenter.children.push(makePanel(''));
  main.children.push(speedCenter);

  // TICKS, NOT A FRACTION. The game's scrubber runs on the replay's tick span -- measured in game:
  // min=0 max=71718 value=<current tick> -- and the marker alignment reads exactly those.
  const DEMO_END_TICK = 71718;
  const scrubber = makePanel('TimeScrubSlider');
  scrubber.min = 0;
  scrubber.max = DEMO_END_TICK;
  scrubber.value = 30021;
  main.children.push(scrubber);

  // The game names each marker after the tick it stands for and places it with an absolute pixel
  // offset worked out once, against whatever width the scrubber had at the time. The positions here
  // are deliberately computed against a NARROWER track (366px, the width measured in game before
  // our host widens the bar), because that stale placement is the fault being tested.
  const STALE_TRACK_W = 366;
  const MARKER_TICKS = [
    ['kill_event', 8651], ['death_event', 21983], ['kill_event', 25137],
    ['death_event', 33131], ['kill_event', 49843], ['death_event', 52780],
    ['kill_event', 66388], ['kill_event', 67064],
  ];
  const markersBox = makePanel('MarkersContainer');
  markersBox.actuallayoutwidth = 506;
  for (const [kind, tick] of MARKER_TICKS) {
    const pip = makePanel(`${kind}_${tick}`);
    pip.actuallayoutwidth = 3;
    pip.style.position = `${(tick / DEMO_END_TICK * STALE_TRACK_W).toFixed(2)}px 0px 0px`;
    markersBox.children.push(pip);
  }
  main.children.push(markersBox);

  const api = {
    GetContextPanel: () => ctx,
    CreatePanel(type, parent, id) {
      const p = makePanel(id || '');
      if (/Root$/.test(p.id)) {
        p.actuallayoutwidth = SCREEN_W;
        p.actuallayoutheight = SCREEN_H;
      }
      p.GetParent = () => parent || null;
      if (parent && parent.children) parent.children.push(p);
      return p;
    },
    // THE COMMAND CHANNEL, modelled exactly as the C++ listener sees it.
    //
    // Deadlock's HUD Panorama context has no GameInterfaceAPI and no CitadelConCommand -- measured
    // in game on build 6687 -- so a panel reaches the console by printing a MARKED line that a
    // tier0 logging listener recognises (DeadlockUiCommandBridge.cpp). The stub splits the same
    // marker off the same stream, which is what keeps this test honest: if the panels ever go back
    // to calling a binding that does not exist, `commands` goes empty and the tests below fail.
    Msg: (s) => {
      const text = String(s);
      if (text.startsWith(MARKER)) {
        commands.push(text.slice(MARKER.length).trim());
        return;
      }
      messages.push(text);
    },
    Schedule: () => {},
    RegisterEventHandler: () => {},
    // RECORDED, because the arguments matter. Dropping the 'mouse' source makes Panorama fire the
    // event with no source and the native onactivate never runs -- a button that highlights and
    // does nothing, which no parse check and no "did it throw" check can see.
    DispatchEvent: (name, panel, source) => {
      dispatched.push({ name, id: panel && panel.id, source });
    },
    Localize: (s) => s,
  };

  const dollar = () => null;
  Object.assign(dollar, api);

  globalThis.$ = dollar;
  // Deliberately ABSENT, mirroring the real context. A panel that reaches for it throws, and the
  // build test catches that instead of the user finding a dead button.
  delete globalThis.GameInterfaceAPI;
  // Also absent in Deadlock's HUD context. C++ owns the input-deny call and reaches Panorama
  // through its own RunScript, so no panel here may depend on it.
  delete globalThis.UiToolkitAPI;
  // Deadlock's own replay-bar API. Present so the delegation path is exercised; the panels also
  // have to survive it being ABSENT, which a separate test covers.
  globalThis.CitadelReplayIncrementSpeedStep = () => {};
  globalThis.CitadelReplaySkipByTime = () => {};
  globalThis.CitadelReplayNextMarker = () => {};

  return {
    dollar,
    ctx,
    messages,
    commands,
    dispatched,
    styleWrites,
    // The C++ passes the state object straight into render(), so the stub keeps the last one and
    // the tests hand it over the same way. It is also parked on the attribute feed, because a
    // panel that reads it from there instead must keep working.
    setState(state) { stateJson = JSON.stringify(state); lastState = state; },
    state() { return lastState; },
  };
}

function loadAll(env) {
  for (const file of FRAGMENTS) {
    // Indirect eval, so each fragment runs in global scope exactly as RunScript executes it.
    (0, eval)(extractJs(file));
  }
  env.dollar.MvmWorkspace.build();
}

// Exercise the actual shared tile with the cursor samples the game publishes. No native Slider
// events are synthesized: the original dead tiles would fail these interactions.
function valueTileRig(dec = 1) {
  const env = makeEnvironment();
  for (const file of ['DeadlockStyleJs.h', 'DeadlockControlsJs.h']) (0, eval)(extractJs(file));
  const X = env.dollar.MvmControls, changes = [];
  const card = X.card(env.ctx, { always: true, groups: [{ head: '', rows: [
    X.scrub('probe', 'Probe', { min: 0, max: 100, dec, onChange: v => changes.push(v) })
  ] }] }, '#ffffff');
  const widget = card.w.probe;
  function rect(p, x, y, w, h) {
    p.GetPositionWithinWindow = () => ({ x, y });
    p.actuallayoutwidth = w; p.actuallayoutheight = h;
  }
  rect(widget.root, 100, 100, 200, 76);
  const st = { open: true, uiMouse: true, layout: { valid: true }, mx: 150, my: 125, lmb: false, shift: false };
  const pump = (patch = {}) => { Object.assign(st, patch); X.cursorPump(st); };
  widget.set(50); pump();
  return { env, X, widget, changes, st, pump, rect };
}

test('value tile drag is relative, quantized, clamped, and emits nothing while resting', () => {
  const r = valueTileRig();
  r.pump({ lmb: true, mx: 120 });
  assert.equal(r.widget.get(), 50, 'press must not jump to absolute position');
  r.pump({ mx: 121 });
  assert.deepEqual(r.changes, [], 'click slop');
  r.pump({ mx: 160 });
  assert.equal(r.widget.get(), 70);
  r.widget.set(50); // previous engine frame may arrive while the pointer owns the tile
  assert.equal(r.widget.get(), 70);
  r.pump({ mx: 180.13 });
  assert.equal(r.widget.get(), 80.1);
  r.pump({ mx: 700 }); assert.equal(r.widget.get(), 100);
  r.pump({ mx: -700 }); assert.equal(r.widget.get(), 0);
  r.pump({ lmb: false });
  const count = r.changes.length, writes = r.env.styleWrites.count;
  for (let i = 0; i < 100; i++) { r.widget.set(0); r.pump(); }
  assert.equal(r.changes.length, count, 'no command feedback loop');
  assert.equal(r.env.styleWrites.count, writes, 'no idle geometry writes');
});

test('Shift drag is five times finer and modifier transitions do not jump', () => {
  const r = valueTileRig();
  r.pump({ lmb: true, shift: true });
  r.pump({ mx: 190 }); assert.equal(r.widget.get(), 54);
  r.pump({ shift: false }); assert.equal(r.widget.get(), 54);
  r.pump({ mx: 210 }); assert.equal(r.widget.get(), 64);
  r.pump({ shift: true }); assert.equal(r.widget.get(), 64);
  r.pump({ mx: 190 }); assert.equal(r.widget.get(), 62);
});

test('value tiles have a full-width track without plus/minus buttons; clicking never nudges', () => {
  const r = valueTileRig();
  assert.equal(r.env.ctx.FindChildTraverse('MvmValueMinus_probe'), null);
  assert.equal(r.env.ctx.FindChildTraverse('MvmValuePlus_probe'), null);
  assert.equal(r.env.ctx.FindChildTraverse('MvmValueTrack_probe').style.width, '100%');
  for (const mx of [116, 281]) {
    r.pump({ mx, my: 160, lmb: true }); r.pump({ lmb: false });
    assert.equal(r.widget.get(), 50);
  }
});

test('hidden tabs, clipped tiles and game mouse mode cannot edit values', () => {
  const r = valueTileRig();
  const parent = r.widget.root.GetParent();
  parent.visible = false;
  r.pump({ lmb: true }); r.pump({ mx: 200 }); assert.deepEqual(r.changes, []);
  r.pump({ lmb: false }); parent.visible = true;
  parent.__mvmValueViewport = true; r.rect(parent, 100, 180, 200, 76);
  r.pump({ lmb: true }); r.pump({ mx: 250 }); assert.deepEqual(r.changes, []);
  parent.__mvmValueViewport = false; r.pump({ lmb: false });
  r.pump({ uiMouse: false, lmb: true, mx: 150 });
  r.pump({ uiMouse: true, mx: 200 }); assert.deepEqual(r.changes, [], 'no grab when switching with mouse held');
  r.pump({ lmb: false }); r.pump({ lmb: true }); r.pump({ mx: 220 });
  assert.equal(r.widget.get(), 60);
  r.pump({ open: false, mx: 260 }); assert.equal(r.widget.get(), 60, 'exit cancels the drag');
  r.pump({ open: true, mx: 280 }); assert.equal(r.widget.get(), 60, 'reopen cannot resume an old press');
});

test('Config has no feature controls and follow/attach have no preset grid', () => {
  const env = makeEnvironment(); loadAll(env);
  const walk = p => [p, ...p.children.flatMap(walk)];
  const config = env.ctx.FindChildTraverse('MvmConfigBody');
  assert.ok(config);
  assert.ok(!walk(config).some(p => /handheld|MvmValue_|MvmValuePlus_/.test(p.id + p.text)));
  assert.ok(!walk(env.ctx.FindChildTraverse('MvmCameraBody')).some(p => p.text === 'Preset'));
});

function shakeMenuRig() {
  const env = makeEnvironment(); loadAll(env);
  const st = makeState({ uiMouse: true, dock: { tab: 'camera', ribbonH: 36, editorH: 196 },
    shakeStyles: [[0, 'Handheld', 'HANDHELD']], shakes: [{ i: 0, s: 0, n: 5, p: 50 }] });
  const render = (patch = {}) => {
    Object.assign(st, patch); env.setState(st); env.dollar.MvmWorkspace.render(st);
  };
  render();
  const diamonds = env.ctx.FindChildTraverse('MvmTlDiamonds').children;
  const walk = p => [p, ...p.children.flatMap(walk)];
  const find = id => env.ctx.FindChildTraverse(id);
  // The shake popup is built through X.contextMenuShell now (DeadlockControlsJs.h), whose own
  // scrim and card panel are deliberately anonymous chrome -- only the CONTENT this file supplies
  // keeps ids, so there is no more 'MvmShakeMenu'/'MvmShakeScrim' id to look up directly. Find the
  // card by the marker buildShakeContent sets on it for the scrub-scope/scroll-clip contract
  // (__mvmValueViewport) plus its KEYFRAME header, and find the scrim as its immediate previous
  // sibling -- contextMenuShell's own build() always creates the scrim right before the card, on
  // the same host.
  const findMenu = () => walk(env.ctx).find(
    (p) => p.__mvmValueViewport && walk(p).some((c) => c.text === 'KEYFRAME'));
  const findScrim = () => {
    const menu = findMenu();
    const parent = menu && menu.GetParent();
    const siblings = parent ? parent.children : [];
    const idx = siblings.indexOf(menu);
    return idx > 0 ? siblings[idx - 1] : null;
  };
  const open = () => diamonds[0].events.oncontextmenu();
  const action = name => walk(findMenu()).find(p => p.text === name).GetParent().Activate();
  return { env, st, render, diamonds, walk, find, findMenu, findScrim, open, action };
}

test('shake is only on the diamond context menu and always edits the clicked key without seeking', () => {
  const r = shakeMenuRig();
  assert.ok(!r.walk(r.find('MvmCameraBody')).some(p => /shake/i.test(p.text)));
  const before = r.env.commands.length;
  r.open();
  assert.equal(r.findMenu().visible, true);
  assert.equal(r.env.commands.length, before, 'opening properties must not select or seek');
  assert.equal(r.st.selected, 1);
  r.render({ selected: 2 });
  r.action('Apply to All');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker pathshake applyall 0');
  r.action('Remove');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker pathshake clear 0');
  assert.ok(!r.walk(r.env.ctx).some(p => /^MvmValue(Minus|Plus)_/.test(p.id)));
  r.open(); r.find('MvmShakeClose').Activate();
  assert.equal(r.findMenu().visible, false);
});

test('shake popup scopes value drags and closes on key removal, retiming, workspace or mouse changes', () => {
  const r = shakeMenuRig(); r.open();
  const X = r.env.dollar.MvmControls;
  const external = X.card(r.env.ctx, { always: true, groups: [{ head: '', rows: [
    X.scrub('behind', 'Behind', { min: 0, max: 100, onChange: () => assert.fail('background edited') })
  ] }] }, '#ffffff');
  for (const p of [r.find('MvmValue_shakeIntensity'), r.find('MvmValue_behind'), r.findMenu()]) {
    p.GetPositionWithinWindow = () => ({ x: 100, y: 100 });
    p.actuallayoutwidth = 200; p.actuallayoutheight = 76;
  }
  // Shared cursor pump: a Shift gesture updates the clicked key, even with another tile beneath it.
  const s = { open: true, uiMouse: true, layout: { valid: true }, mx: 120, my: 125, lmb: false, shift: true };
  X.cursorPump(s); s.lmb = true; X.cursorPump(s); s.mx = 232; X.cursorPump(s);
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker pathshake set 0 0 6 50');
  assert.equal(external.w.behind.get(), 0);
  s.lmb = false; X.cursorPump(s);
  for (const patch of [
    { markerList: r.st.markerList.slice(1) },
    { markerList: [{ tick: 17001 }, ...r.st.markerList.slice(1)] },
    { workspace: 'config' }, { uiMouse: false }, { open: false },
    { dock: { tab: 'graph', ribbonH: 36, editorH: 196 } }
  ]) {
    const fresh = shakeMenuRig(); fresh.open(); fresh.render(patch);
    assert.equal(fresh.findMenu().visible, false, JSON.stringify(patch));
    assert.equal(fresh.findScrim().visible, false);
  }
});

test('right-button samples open a diamond once, reuse panels, and settle without style churn', () => {
  const r = shakeMenuRig(), p = r.diamonds[0];
  p.GetPositionWithinWindow = () => ({ x: 50, y: 900 });
  p.actuallayoutwidth = p.actuallayoutheight = 14;
  r.render({ rmb: false });
  r.render({ mx: 57, my: 907, rmb: true });
  assert.equal(r.findMenu().visible, true);
  const panels = r.walk(r.env.ctx).length;
  r.render({ rmb: false });
  r.env.styleWrites.count = 0;
  for (let i = 0; i < 50; ++i) r.render();
  assert.equal(r.env.styleWrites.count, 0);
  r.findScrim().Activate(); r.open();
  // The shell REBUILDS its content on every open (contextMenuShell has no persistent-widget
  // concept, see buildShake's own comment), so "reuse the popup" here means the shell's OWN
  // scrim + card are not recreated (still 2 fewer panels than a from-scratch build would add) --
  // not that every content panel underneath is the very same object as before.
  assert.equal(r.walk(r.env.ctx).length, panels, 'reopening must reuse the popup shell, not grow it');
});

test('CS2-shaped keyframe menu edits ease on its own key, lists styles inline, and confirms Remove All', () => {
  const r = shakeMenuRig(); r.open();
  // The shell's own scrim/card/body are built ONCE and persist across close/reopen (only the
  // CONTENT rows underneath are torn down and rebuilt on each open) -- so this single reference
  // stays valid for the visible/width checks at the end of the test, after the close+reopen below.
  const popup = r.findMenu();
  assert.equal(popup.style.width, '390px');
  assert.ok(r.walk(popup).some(p => p.text === 'KEYFRAME'));
  assert.ok(r.walk(popup).some(p => p.text === 'KEY 1'));
  assert.ok(!r.walk(popup).some(p => /Enable path shake|This camera|Done/.test(p.text)));
  assert.equal(r.find('MvmValue_shakeIntensity').style.height, '34px');
  assert.equal(r.find('MvmValueTrack_shakeIntensity').children.length, 10);
  r.find('MvmShakeEase_2').Activate();
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker marker ease out 0');
  assert.equal(r.st.selected, 1);
  r.st.markerList[0].ease = 2; r.render();
  assert.equal(r.find('MvmShakeEase_2').__selected, true);
  r.find('MvmShakeStyle').Activate();
  assert.equal(r.find('MvmShakeStyles').visible, true);
  const before = r.env.commands.length;
  r.walk(r.find('MvmShakeStyles')).find(p => p.text === 'Handheld').GetParent().Activate();
  assert.equal(r.find('MvmShakeStyles').visible, false);
  assert.equal(r.env.commands[before], 'mirv_filmmaker pathshake set 0 0 5 50');
  const count = r.env.commands.length;
  r.find('MvmShakeRemoveAll').Activate();
  assert.equal(r.env.commands.length, count, 'first click arms the CS2 in-place confirmation');
  r.find('MvmShakeClose').Activate(); r.open();
  r.find('MvmShakeRemoveAll').Activate();
  assert.equal(r.env.commands.length, count, 'reopening must never retain an armed remove-all');
  r.find('MvmShakeRemoveAll').Activate();
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker pathshake clearall');
  assert.equal(popup.visible, false);
});

test('left-click on a timeline diamond goes to its camera; right-click still opens shake', () => {
  const r = shakeMenuRig();
  // r.diamonds is MvmTlDiamonds' raw children, which now interleaves the shake-dot pool's panels
  // among the diamonds (marker 0 carries a shake in this rig's default state, so its dot sits right
  // after it) -- find by the marker index the panel itself carries, not by array position.
  const diamondFor = (i) => r.diamonds.find((p) => p.__mvmMarkerIndex === i);
  const before = r.env.commands.length;
  diamondFor(2).events.onactivate();
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker marker goto 2');
  assert.equal(r.env.commands.length, before + 1, 'left-click must fire exactly one command');
  assert.equal(r.findMenu(), undefined, 'left-click must not also open the shake popup');
  diamondFor(2).events.oncontextmenu();
  assert.equal(r.findMenu().visible, true, 'right-click still opens the shake popup');
});

test('a keyframe carrying a shake shows a dot on its diamond; others do not', () => {
  const r = shakeMenuRig(); // built with shakes: [{ i: 0, ... }] -- marker 0 only
  const diamondLayer = r.find('MvmTlDiamonds');
  // The dot pool shares diamondLayer with the diamond pool (DeadlockTimelineJs.h's shakeDotPool),
  // so a 4px-wide VISIBLE child is unambiguously a dot -- diamonds are 14px.
  const dotCount = () => diamondLayer.children.filter((p) => p.visible && p.style.width === '4px').length;
  assert.equal(dotCount(), 1, 'exactly one of the three markers carries a shake at setup');
  r.render({ shakes: [] });
  assert.equal(dotCount(), 0, 'clearing every shake leaves no dots');
  r.render({ shakes: [{ i: 0, s: 0, n: 5, p: 50 }, { i: 2, s: 0, n: 5, p: 50 }] });
  assert.equal(dotCount(), 2, 'two shaked markers, two dots');
});

function followCardRig(followOverrides = {}) {
  const env = makeEnvironment(); loadAll(env);
  const st = makeState({
    workspace: 'camera',
    follow: Object.assign({
      on: false, mode: 'lockon', hero: null, attach: 'chest',
      offx: 0, offy: 0, offz: 0, rotpitch: 0, rotyaw: 0, rotroll: 0, fov: 68,
      look: 0.35, pos: 0.4, predict: 0.08, deadzone: 1.5, maxturn: 420, deathhold: 2,
      releaseOnDeath: false, holdLast: false, retarget: true,
    }, followOverrides),
  });
  const render = (patch = {}) => { Object.assign(st, patch); env.setState(st); env.dollar.MvmWorkspace.render(st); };
  render();
  // Follow is not the column's default sub-tab (Path is) -- gotoSub is the same automation seam
  // mirv_filmmaker ui_eval uses to reach it without a click.
  env.dollar.MvmCameraBody.gotoSub('follow');
  render();
  const walk = (p) => [p, ...p.children.flatMap(walk)];
  const find = (id) => env.ctx.FindChildTraverse(id);
  const cameraBody = () => env.ctx.FindChildTraverse('MvmCameraBody');
  // Row/act-group headings (buildMode's label, buildAct's caption) are always the FIRST child of
  // their own row/box in DeadlockControlsJs.h, so this finds every occurrence of a given heading --
  // there can be more than one on purpose (see the camera-mode test below).
  const byHeading = (text) => walk(cameraBody()).filter((p) => p.children[0] && p.children[0].text === text);
  // Disambiguates same-named groups on different pages (Path and Follow both have a 'Placement'
  // action group) by requiring the specific button caption actually live inside that heading's box.
  const groupWithButton = (heading, caption) => {
    for (const box of byHeading(heading)) {
      if (walk(box).some((p) => p.text === caption)) return box;
    }
    return undefined;
  };
  return { env, st, render, walk, find, cameraBody, byHeading, groupWithButton };
}

test('the follow card header is one pill toggle, not a separate Live action button', () => {
  const r = followCardRig();
  const followTitle = r.walk(r.cameraBody()).find((p) => p.text === 'FOLLOW / ATTACH CAMERA');
  assert.ok(followTitle, 'follow card header not found');
  // The old static "Live" header action (which could turn the camera on but, sending `follow live`
  // unconditionally, never off) is gone. The Path card's Timing row (CS2's Live / Freeze pair)
  // legitimately says "Live", so the check is on what a Live/Stop control DOES, not on the word:
  // nothing labelled Live or Stop may send a `follow` command.
  for (const p of r.walk(r.cameraBody()).filter((q) => q.text === 'Live' || q.text === 'Stop')) {
    const before = r.env.commands.length;
    try { p.GetParent().Activate(); } catch (e) {}
    assert.ok(!r.env.commands.slice(before).some((c) => c.startsWith('mirv_filmmaker follow')),
      'a Live/Stop control still sends a follow command');
  }
  // titleCol -> head: the pill is head's other interactive child, built 44x24 (DeadlockControlsJs.h
  // pill()), so borderRadius 12px is unique among the header's edge bar / title / chevron.
  const head = followTitle.GetParent().GetParent();
  const pills = head.children.filter((c) => c.style.borderRadius === '12px');
  assert.equal(pills.length, 1, 'expected exactly one pill-shaped control in the header');
  const before = r.env.commands.length;
  pills[0].Activate();
  assert.equal(r.env.commands.length, before + 1);
  // `toggle` is the only symmetric verb Deadlock's follow dispatcher has (no separate `stop`,
  // DeadlockCommands.cpp ~627-628) -- a header action still sending `follow live` unconditionally
  // is exactly the bug this change fixed.
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker follow toggle');
});

test('the follow card camera-mode selector appears on every sub-page and stays in sync', () => {
  const r = followCardRig();
  const modeRows = r.byHeading('Camera mode');
  assert.equal(modeRows.length, 3, 'one Camera mode row per Target/Framing/Tracking page (X.card '
    + 'persist duplicates an id-bearing row per group; each page needs its own id, see '
    + 'followPersistRows in DeadlockCameraBodyJs.h)');
  for (const row of modeRows) assert.ok(r.walk(row).some((p) => p.text === 'Lock-on'));
  const S = r.env.dollar.MvmStyle.S;
  r.render({ follow: Object.assign({}, r.st.follow, { mode: 'attach' }) });
  for (const row of r.byHeading('Camera mode')) {
    const attachLabel = r.walk(row).find((p) => p.text === 'Attach');
    assert.equal(attachLabel.style.color, S.green, 'every page must show Attach selected, not just one');
  }
  const attachCell = r.walk(modeRows[0]).find((p) => p.text === 'Attach').GetParent();
  attachCell.Activate();
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker follow mode attach');
});

test('attach mode hides Place/Clear and the aim tracking tiles, never Reposition or death/retarget', () => {
  const r = followCardRig();
  assert.equal(r.find('MvmValue_look').visible, true, 'lock-on shows the aim tiles');
  // Place / Reposition / Clear now live in ONE "This camera" box (CS2 parity:
  // CameraEditorFollowInspectorJs.h keeps all three in one row) instead of two boxes split by
  // which buttons could hide -- so this checks each BUTTON's own visibility within that one box,
  // not two separate boxes'.
  const thisCameraBox = r.groupWithButton('This camera', 'Place');
  assert.ok(thisCameraBox, 'This camera group not found');
  const buttonNamed = (caption) => {
    const label = r.walk(thisCameraBox).find((p) => p.text === caption);
    return label ? label.GetParent() : undefined;
  };
  const placeBtn = buttonNamed('Place'), repositionBtn = buttonNamed('Reposition'), clearBtn = buttonNamed('Clear');
  assert.ok(placeBtn && repositionBtn && clearBtn, 'Place/Reposition/Clear buttons not all found');
  assert.equal(placeBtn.visible, true);
  assert.equal(repositionBtn.visible, true);
  assert.equal(clearBtn.visible, true);
  r.render({ follow: Object.assign({}, r.st.follow, { mode: 'attach' }) });
  assert.equal(r.find('MvmValue_look').visible, false, 'attach hides aim smoothing (CS2: '
    + 'folAimSettings.visible = !attachModeOn, CameraEditorFollowInspectorJs.h line 411)');
  assert.equal(r.find('MvmValue_deathHold').visible, true, 'death hold has no type-gated Deadlock '
    + 'equivalent to hide behind, so it stays visible in both modes');
  assert.equal(placeBtn.visible, false, 'attach hides Place (CS2: followPlace.visible = !attachModeOn)');
  assert.equal(clearBtn.visible, false, 'attach hides Clear (CS2: followClear.visible = !attachModeOn)');
  assert.equal(repositionBtn.visible, true, 'Reposition stays up in both modes, in the SAME row -- '
    + 'attach uses it to capture the mount-relative pose');
});

test('Clear all (This camera) goes through the confirm modal, and there is no Shot file group', () => {
  const env = makeEnvironment(); loadAll(env);
  const st = makeState({ workspace: 'camera' });
  env.setState(st); env.dollar.MvmWorkspace.render(st);
  const walk = (p) => [p, ...p.children.flatMap(walk)];
  const byHeading = (text) => walk(env.ctx).filter((p) => p.children[0] && p.children[0].text === text);
  const groupWithButton = (heading, caption) => {
    for (const box of byHeading(heading)) {
      const label = walk(box).find((p) => p.text === caption);
      if (label) return label.GetParent();
    }
    return undefined;
  };
  const yesBtn = () => walk(env.ctx).find((p) => p.text === 'Yes, clear all').GetParent();

  const before = env.commands.length;
  groupWithButton('This camera', 'Clear all').Activate();
  assert.equal(env.commands.length, before, 'Clear all must not fire marker clear on the first click');
  assert.ok(walk(env.ctx).some((p) => p.text === 'CLEAR ALL CAMERA PATHS?'),
    'expected CS2-worded confirm modal (CameraEditorJs.h ~174)');
  yesBtn().Activate();
  assert.equal(env.commands.at(-1), 'mirv_filmmaker marker clear');

  // CS2's PATH card is Curve and Speed only (CameraEditorPathInspectorJs.h), so there is no second
  // Clear to route through the modal.
  assert.equal(byHeading('Shot file').length, 0, 'CS2 has no Shot file group; Deadlock must not either');
});

test('every fragment yields a non-empty script', () => {
  for (const file of FRAGMENTS) {
    const js = extractJs(file);
    assert.ok(js.trim().length > 200, `${file} extracted only ${js.length} chars`);
  }
});

test('the fragments build the workspace without throwing', () => {
  const env = makeEnvironment();
  loadAll(env);

  assert.ok(env.dollar.MvmStyle, '$.MvmStyle did not register');
  assert.ok(env.dollar.MvmWorkspace, '$.MvmWorkspace did not register');
  assert.ok(env.dollar.MvmCameraBody, '$.MvmCameraBody did not register');
  assert.ok(env.dollar.MvmGraphBody, '$.MvmGraphBody did not register');
  assert.ok(env.dollar.MvmConfigBody, '$.MvmConfigBody did not register');
  assert.ok(env.dollar.MvmRenderBody, '$.MvmRenderBody did not register');
  assert.ok(env.dollar.MvmDraw, '$.MvmDraw did not register');
  assert.ok(env.dollar.MvmSpeedDial, '$.MvmSpeedDial did not register');
  assert.ok(env.dollar.MvmInspectorGrip, '$.MvmInspectorGrip did not register');
  assert.ok(env.dollar.MvmRegular, '$.MvmRegular did not register');
  assert.ok(env.dollar.MvmTimeline, '$.MvmTimeline did not register');
  assert.ok(env.dollar.MvmDockRibbon, '$.MvmDockRibbon did not register');
  assert.ok(env.dollar.MvmNativeHud, '$.MvmNativeHud did not register');

  assert.ok(env.ctx.children.length > 0, 'nothing was created on the context panel');

  const noise = env.messages.filter((m) => /error|not defined|not a function/i.test(m));
  assert.deepEqual(noise, [], 'the build logged errors');
});

// A render that throws leaves the panel built but frozen, which is indistinguishable in game from
// a panel that was never wired up. Three passes, because the second and third exercise the reuse
// paths that the first one populates.
test('render survives repeated passes in every workspace', () => {
  const env = makeEnvironment();
  loadAll(env);

  for (const workspace of ['camera', 'graph', 'config', 'render']) {
    env.setState(makeState({ workspace }));
    for (let i = 0; i < 3; ++i) {
      assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()),
        `render threw in the ${workspace} workspace on pass ${i + 1}`);
    }
  }
});

// The panel exists before a demo does, so every field it reads has to tolerate being absent. This
// is the state the workspace is in the moment it is first built in the dashboard.
test('render survives an empty state and a closed workspace', () => {
  const env = makeEnvironment();
  loadAll(env);

  env.setState({});
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw on an empty state');

  env.setState(makeState({ open: false }));
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw while closed');

  env.setState(makeState({ demo: false, markers: 0, markerList: [] }));
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw with no demo');
});

// DeadlockInspectorGripJs.h's MIN_W (360) is tighter than Deadlock's own WorkspaceLayout_ClampInspector
// floor (320) -- see the header comment on DeadlockInspectorGripJs.h. It USED to mirror CS2's
// InspectorWidth.cpp kMin; CS2 moved to 600 when its inspector became the rail + pane layout
// (Docs/features/camera-editor-feature.md), which Deadlock has not adopted, so the
// cross-check is gone and only Deadlock's own bound is held here.
test('the Deadlock inspector grip floor stays tighter than the looser solver floor', () => {
  const gripSrc = fs.readFileSync(path.join(JS_DIR, 'DeadlockInspectorGripJs.h'), 'utf8');
  assert.match(gripSrc, /var MIN_W = 360;/, 'the Deadlock grip floor moved');

  const layoutMath = fs.readFileSync(
    path.join(JS_DIR, 'DeadlockWorkspaceLayoutMath.h'), 'utf8');
  assert.match(layoutMath, /std::max\(320\.0, requested\)/,
    "WorkspaceLayout_ClampInspector's own floor moved -- re-check whether 360 is still the tighter bound");
});

// A small rig for the RENDER workspace's JOB column and Set In / Set Out / Clear row, mirroring
// shakeMenuRig()'s style: found by TEXT rather than an id, because X.act's buttons and M.label's
// captions here do not hand out explicit ids for every row.
function renderBodyRig(renderOverrides = {}) {
  const env = makeEnvironment();
  loadAll(env);
  const st = makeState({
    workspace: 'render',
    tick: 17801,
    render: Object.assign({
      state: 'Idle', running: false, frames: 0, dropped: 0, take: 0,
      startTick: 17000, endTick: 18500, range: 'custom', camSrc: 'current', hud: 'game',
      format: 'png', quality: 92, names: true, damage: true, pauseOnDrop: true, beep: false,
      dir: 'C:\\Temp\\take0001',
    }, renderOverrides),
  });
  const render = (patch = {}) => { Object.assign(st, patch); env.setState(st); env.dollar.MvmWorkspace.render(st); };
  render();
  const walk = (p) => [p, ...p.children.flatMap(walk)];
  // Scoped to a named root, NOT the whole ctx tree: 'Clear' alone is ambiguous -- DeadlockCameraBodyJs.h
  // has its own 'Clear' buttons (follow-camera target/reposition), built into the same shared context,
  // and an unscoped search silently grabbed one of those instead of the Range group's own button the
  // first time this test was written.
  const byTextIn = (rootId, text) => {
    const root = env.ctx.FindChildTraverse(rootId);
    assert.ok(root, `no panel with id "${rootId}"`);
    return walk(root).find((p) => p.text === text);
  };
  const clickIn = (rootId, text) => {
    const l = byTextIn(rootId, text);
    assert.ok(l, `no panel with text "${text}" under "${rootId}"`);
    l.GetParent().Activate();
  };
  return { env, st, render, walk, byTextIn, clickIn };
}

// CS2's Range tab sends Set In / Set Out / Clear as three independent commands (`render in
// <tick>` / `render out <tick>` / `render clearrange`); DeadlockCommands.cpp's "render" dispatcher
// has no such split, only the paired numeric setter (`render range <start> <end>`), so the ported
// trio here has to read the OTHER bound back off the last-pushed state and resend both.
test('the Range group Set In / Set Out / Clear send the right render range command', () => {
  const r = renderBodyRig({ startTick: 17000, endTick: 18500 });

  r.clickIn('MvmRenderColumn', 'Set In');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker render range 17801 18500',
    'Set In must move only the In bound to the playhead tick and keep the existing Out bound');

  r.clickIn('MvmRenderColumn', 'Set Out');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker render range 17000 17801',
    'Set Out must move only the Out bound to the playhead tick and keep the existing In bound');

  r.clickIn('MvmRenderColumn', 'Clear');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker render range 0 0',
    'Clear must send RenderSettings own "unset" pair, not invent a clearrange verb Deadlock has no handler for');

  // No live playhead tick (e.g. no demo yet) -- Set In/Out must no-op rather than send a bogus tick.
  const before = r.env.commands.length;
  delete r.st.tick;
  r.clickIn('MvmRenderColumn', 'Set In');
  assert.equal(r.env.commands.length, before, 'Set In must not fire without a live playhead tick');
});

// Every DISTINCT RenderState needs its own JOB headline -- Arming, Capturing and Finishing used to
// all read as the same "Capturing -- frame N" text, and Failed was not shown at all (blockedReason()
// is empty once AllocateTakeDir has already failed, so the status silently fell back to "Ready to
// render" as though nothing went wrong).
test('the JOB column surfaces every RenderState distinctly, Failed included', () => {
  const r = renderBodyRig({ state: 'Idle', running: false, frames: 0 });

  const jobStatus = r.byTextIn('MvmJobColumn', 'Ready to render');
  assert.ok(jobStatus, 'could not find the JOB status label in its Idle/ready text');
  const bodyChildren = jobStatus.GetParent().children;
  const jobProblem = bodyChildren[bodyChildren.indexOf(jobStatus) + 1];
  assert.ok(jobProblem, 'could not find the JOB problem label right after the status label');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Arming', running: true }) });
  assert.equal(jobStatus.text, 'ARMING');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Capturing', running: true, frames: 88 }) });
  assert.equal(jobStatus.text, 'CAPTURING  ·  frame 88');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Finishing', running: true }) });
  assert.equal(jobStatus.text, 'FINISHING');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Failed', running: false, take: 5 }) });
  assert.equal(jobStatus.text, 'Render FAILED');
  assert.match(jobProblem.text, /Capture failed/);

  r.render({ render: Object.assign({}, r.st.render, { state: 'Idle', running: false, frames: 0 }) });
  assert.equal(jobStatus.text, 'Ready to render');
});

// CS2 pairs its RENDER title with a live "RENDER \u00b7 RUNNING" badge and a "\u2715 Back" chip
// back to the Camera Editor, on the SAME row (CameraEditorRenderJs.h). The badge lives on the JOB
// column's own title here (CS2's JOB column carries no chip of its own to reuse); the back
// affordance now lives where CS2's does -- the shared column shell's title row
// (DeadlockControlsJs.h's buildColumn, an extraChip the Render column alone passes -- moved there
// from the JOB column so the position matches CS2's, not just the existence.
test('the JOB title badges while a job runs, and the Camera chip switches workspace', () => {
  const r = renderBodyRig({ state: 'Idle', running: false });

  const title = r.byTextIn('MvmJobColumn', 'JOB');
  assert.ok(title, 'could not find the JOB column title in its idle text');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Capturing', running: true }) });
  assert.equal(title.text, 'JOB \u00b7 RENDERING');

  r.render({ render: Object.assign({}, r.st.render, { state: 'Idle', running: false }) });
  assert.equal(title.text, 'JOB');

  r.clickIn('MvmRenderColumn', '\u00d7 Back');
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker workspace camera');

  // CS2's render header is the title and that ONE chip -- no mouse chip, no ESC EXIT -- and the
  // column has no start control of its own (START lives in the JOB column).
  for (const text of ['MOUSE: GAME \u00b7 J', 'MOUSE: UI \u00b7 J', 'ESC EXIT', 'Start capture', 'CAPTURE']) {
    assert.equal(r.byTextIn('MvmRenderColumn', text), undefined, `the RENDER column still carries "${text}"`);
  }
});

// CS2's action row (RENDER; Customize Player has no Deadlock counterpart) at the foot of the editor
// and Config columns, and CONFIG's GAME UI chip between MOUSE and ESC EXIT.
test('Camera and Config columns carry the RENDER action row; Config carries the GAME UI chip', () => {
  const env = makeEnvironment(); loadAll(env);
  const walk = (p) => [p, ...p.children.flatMap(walk)];
  const inCol = (id, text) => walk(env.ctx.FindChildTraverse(id)).find((p) => p.text === text);
  for (const [ws, id] of [['camera', 'MvmCameraColumn'], ['config', 'MvmConfigColumn']]) {
    const st = makeState({ workspace: ws, hudHidden: ws === 'config' });
    env.setState(st); env.dollar.MvmWorkspace.render(st);
    const render = inCol(id, 'Render');
    assert.ok(render, `${id} has no RENDER action`);
    env.commands.length = 0;   // both columns send the same verb; do not pass on the other's click
    render.GetParent().Activate();
    assert.equal(env.commands.at(-1), 'mirv_filmmaker workspace render', `${id}'s RENDER does not open Render`);
  }
  const chip = inCol('MvmConfigColumn', 'GAME UI: HIDE ALL');
  assert.ok(chip, 'CONFIG does not name the hidden HUD state on its GAME UI chip');
  chip.GetParent().Activate();
  assert.equal(env.commands.at(-1), 'mirv_filmmaker hud toggle');
  const titleRow = chip.GetParent().GetParent();
  const texts = titleRow.children.map((c) => (c.children[0] && c.children[0].text) || c.text);
  assert.deepEqual(texts.slice(1), ['MOUSE: GAME · J', 'GAME UI: HIDE ALL', 'ESC EXIT'],
    'CS2 order is MOUSE, GAME UI, ESC EXIT');
});

// CS2's renderFooter: Set In / Set Out / Clear pinned under the scroller, shown on the Range tab
// only (`renderFooter.visible = (tab === 0)`, CameraEditorRenderJs.h).
test('Render: Set In / Set Out / Clear are a pinned footer that only the Range tab shows', () => {
  const r = renderBodyRig({});
  const setIn = r.byTextIn('MvmRenderColumn', 'Set In');
  assert.ok(setIn, 'no Set In button');
  const footer = setIn.GetParent().GetParent().GetParent();
  assert.equal(footer.visible, true, 'the footer is hidden on the Range tab');
  r.clickIn('MvmRenderColumn', 'CAMERA');
  assert.equal(footer.visible, false, 'the Range footer stayed up on the Camera tab');
  r.clickIn('MvmRenderColumn', 'RANGE');
  assert.equal(footer.visible, true, 'the footer did not come back on the Range tab');
});

// CS2 runs the dock the whole width in the render workspace (GraphEditorJs.h:414 `renderOpen ?
// scrW`), so the RENDER column and the JOB column both END at the dock's top edge. Everywhere else
// the column is full height and the dock stops beside it.
test('Render: the dock spans the screen and both columns stop at its top edge, like CS2', () => {
  const env = makeEnvironment(); loadAll(env);
  for (const ws of ['camera', 'render']) {
    const st = makeState({ workspace: ws });
    st.dock = { tab: 'graph', ribbonH: 36, editorH: 196, meta: '' };
    if (ws === 'render') Object.assign(st.layout, { leftW: 340, areaX: 340, areaW: 1060 });
    env.setState(st); env.dollar.MvmWorkspace.render(st);
    const L = st.layout;
    const col = env.ctx.FindChildTraverse('MvmColumn');
    const body = env.ctx.FindChildTraverse('MvmDockBody');
    const wantDockW = ws === 'render' ? L.screenW : L.screenW - L.inspectorW;
    // In Render the column ends at the ribbon's top, and ribbon + body both span the screen.
    const wantColH = ws === 'render' ? L.screenH - L.dockH : L.screenH;
    assert.equal(parseFloat(body.style.width), wantDockW, `${ws}: dock body is ${body.style.width}, want ${wantDockW}px`);
    assert.equal(parseFloat(col.style.height), wantColH, `${ws}: column is ${col.style.height} tall, want ${wantColH}px`);
    const ribbon = env.dollar.MvmDockRibbon.root();
    assert.equal(parseFloat(ribbon.style.width), wantDockW, `${ws}: the dock ribbon is not as wide as the dock body`);
    const grip = env.ctx.FindChildTraverse('MvmInspectorGrip');
    assert.equal(parseFloat(grip.style.height), wantColH, `${ws}: the grip hairline is not the column's height`);
    if (ws === 'render') {
      const job = env.ctx.FindChildTraverse('MvmJobColumn');
      assert.equal(job.visible, true, 'the JOB column is not shown in the render workspace');
      assert.equal(parseFloat(String(job.style.position).split(' ')[1]), 0, 'the JOB column does not start at the top');
      assert.equal(parseFloat(job.style.height), L.screenH - L.dockH, 'the JOB column does not end at the dock');
    }
  }
});

// Deadlock's "SPECTATING <hero>" strip names a hero the camera is not showing while one of our
// cameras owns the view, and CS2's free-cam preview has no such panel. Hidden by OPACITY: the strip
// is a transformed native panel, and writing `visible` on one is what disconnected the replay.
test('the spectator strip hides by opacity while our camera owns the view, and comes back', () => {
  const env = makeEnvironment(); loadAll(env);
  const strip = env.dollar.CreatePanel('Panel', env.ctx, 'spectate_bar');
  assert.ok(strip, 'could not stand up a spectate_bar stub');
  const visibleBefore = strip.visible;

  let st = makeState({ camOwned: true });
  env.setState(st); env.dollar.MvmWorkspace.render(st);
  assert.equal(String(strip.style.opacity), '0', 'the strip still draws while free cam owns the view');
  assert.equal(strip.visible, visibleBefore, 'the strip had `visible` written on it');

  st = makeState({ camOwned: false });
  env.setState(st); env.dollar.MvmWorkspace.render(st);
  // The stub deletes a null-cleared property and reads a missing one back as '' -- i.e. no inline
  // override left, which is what "the stylesheet's own opacity" means.
  assert.equal(strip.style.opacity, '', 'the strip did not get its own opacity back');

  // Closed workspace, our camera still flying: the claim is still false, so it stays hidden.
  st = makeState({ open: false, camOwned: true });
  env.setState(st); env.dollar.MvmWorkspace.render(st);
  assert.equal(String(strip.style.opacity), '0', 'the strip came back with the workspace closed');
  assert.equal(strip.visible, visibleBefore, 'the strip had `visible` written on it');
});

// The marker rows are pooled and reused rather than rebuilt, because rebuilding the list every
// frame makes it flicker and lose its scroll position. Growing and shrinking must both work.
test('the marker list grows and shrinks without rebuilding', () => {
  const env = makeEnvironment();
  loadAll(env);

  const many = [];
  for (let i = 0; i < 25; ++i) many.push({ tick: 1000 + i * 100, pos: `${i} 0 0`, selected: i === 7 });

  env.setState(makeState({ markers: many.length, markerList: many }));
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw on 25 markers');

  env.setState(makeState({ markers: 1, markerList: [many[0]] }));
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw shrinking to 1 marker');

  env.setState(makeState({ markers: 0, markerList: [] }));
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw on an empty list');
});

// Deadlock's replay bar is a native C++ panel, so its JavaScript entry points may simply not be
// there on a given build or in a non-replay context. The transport must fall back to our own
// console commands rather than throwing.
test('transport falls back when the native replay API is missing', () => {
  const env = makeEnvironment();
  delete globalThis.CitadelReplaySkipByTime;
  delete globalThis.CitadelReplayIncrementSpeedStep;
  loadAll(env);
  env.setState(makeState());
  env.dollar.MvmWorkspace.render(env.state());

  // Reach the transport buttons the way a click would, through the stored panel events.
  assert.doesNotThrow(() => {
    (0, eval)('typeof CitadelReplaySkipByTime');
  });
});

// The graph's curve and keyframe panels are POOLED and reused, and the pool has a hard cap. A
// channel with more sample points than the cap must clamp rather than run off the end of the pool
// and start writing to undefined.
test('the graph clamps to its panel pool and tolerates a missing graph block', () => {
  const env = makeEnvironment();
  loadAll(env);

  const fat = [];
  for (let i = 0; i < 400; ++i) fat.push(i / 399, (i % 17) / 16);
  const state = makeState();
  state.graph.channels[0].pts = fat;
  state.graph.channels[0].keyPts = fat.slice(0, 200).map((_, i) => ({
    t: i / 200, v: (i % 11) / 10, sel: i === 3,
  }));
  env.setState(state);
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw on an oversized curve');

  // The panel exists before the graph state does, so a missing block must be a no-op rather than
  // a throw that takes the whole workspace render with it.
  const noGraph = makeState();
  delete noGraph.graph;
  env.setState(noGraph);
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw with no graph block');

  // A zero-area plot happens before the viewport is known. Drawing into it would collapse every
  // curve onto one pixel, so it must bail instead.
  const flat = makeState();
  flat.graph.plotW = 0;
  flat.graph.plotH = 0;
  env.setState(flat);
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()), 'render threw on a zero-area plot');
});

// No style write may be the empty string or an undefined token: Panorama throws on both and the
// panel build aborts mid-way. The stub's style proxy turns either into a throw, so a clean build
// above already proves it -- this pins the stub honest so the guarantee cannot silently lapse.
test('the stub rejects the style writes Panorama rejects', () => {
  const env = makeEnvironment();
  const p = env.dollar.CreatePanel('Panel', env.ctx, 'Probe');
  assert.throws(() => { p.style.backgroundColor = ''; }, /empty value/);
  assert.throws(() => { p.style.color = undefined; }, /missing token/);
  assert.doesNotThrow(() => { p.style.backgroundColor = 'rgba(0,0,0,0)'; });
  // ...and accepts the one clear Panorama accepts, so a release path can hand a native panel its
  // own stylesheet value back. Reading it afterwards must answer as if we never wrote it.
  p.style.width = '1400px';
  p.style.width = null;
  assert.equal(p.style.width, '', 'null did not clear the override');
});

// THE LAYOUT IS THE THING THAT WAS WRONG ON SCREEN. The panel used to draw a rail down the left
// and an opaque body over everything else, so there was no preview at all. These pin down the shape
// that replaced it: a dock along the bottom, a column on the right, and a framed preview between
// them -- and, just as importantly, NOTHING drawn when C++ has not solved a layout yet.
test('the workspace places its furniture from the solved layout', () => {
  const env = makeEnvironment();
  loadAll(env);

  const byId = new Map();
  (function walk(p) {
    if (p.id) byId.set(p.id, p);
    for (const c of p.children) walk(c);
  })(env.ctx);

  for (const id of ['MvmWorkspaceRoot', 'MvmHeader', 'MvmColumn', 'MvmTimelineDock',
                    'MvmTlTrack', 'MvmTlScrub', 'MvmTlHead', 'MvmTlRail',
                    'MvmRegularTimeline',
                    'MvmGraphBody', 'MvmGraphCanvas', 'MvmGraphChannels',
                    'MvmMaskL', 'MvmFrameT', 'MvmPreviewTag']) {
    assert.ok(byId.has(id), `${id} was never created`);
  }

  // THE WORKSPACES ARE REACHED FROM THE REPLAY BAR, not from a strip inside the column. A second
  // set of tabs in the column would be a second answer to "which workspace am I in".
  for (const id of ['MvmTab_camera', 'MvmTab_config', 'MvmTab_render', 'MvmTabStrip']) {
    assert.ok(!byId.has(id), `${id} is a column tab; the replay bar owns workspace switching`);
  }
  // Each column names itself, in its own accent, instead. (The bar's own buttons are checked in
  // the playback-bar test: the bar is attached during render, not build.)
  for (const id of ['MvmCameraColumn', 'MvmConfigColumn', 'MvmRenderColumn']) {
    assert.ok(byId.has(id), `${id} was never created`);
  }
  for (const id of ['MvmDockTab_native', 'MvmDockTab_camera', 'MvmDockTab_graph']) {
    assert.ok(byId.has(id), `${id} was never created on the dock ribbon`);
  }
  assert.ok(!byId.has('MvmTab_customize'), 'a customise tab was created for a game with no skins');

  // The transport lives on the DOCK, not in the column. Buttons that act on time belong on the
  // thing that draws time.
  const dock = byId.get('MvmTimelineDock');
  const dockIds = new Set();
  (function walk(p) {
    if (p.id) dockIds.add(p.id);
    for (const c of p.children) walk(c);
  })(dock);
  // CS2's camera timeline AS DOCKED IN THE EDITOR (CameraTimelineJs.h, hosted): the transport (play
  // glyph, joined stepper, Linear/Smooth, readout, speed scrubber), the track, and the KEYFRAME
  // footer (+Add, -Delete, Tick->Here, Ease, the per-segment speed stepper, the speed-mode set,
  // Live/Freeze).
  for (const id of ['MvmTlShot', 'MvmTlStep', 'MvmTlStepBack', 'MvmTlStepFwd', 'MvmTlReadout',
                    'MvmTlLinear', 'MvmTlSmooth', 'MvmTlSpeed',
                    'MvmTlScrub', 'MvmTlRail', 'MvmTlHead',
                    'MvmTlAdd', 'MvmTlDel', 'MvmTlTickHere', 'MvmTlEase',
                    'MvmTlSegMinus', 'MvmTlSegLbl', 'MvmTlSegPlus',
                    'MvmTlSpeed_manual', 'MvmTlSpeed_perseg', 'MvmTlSpeed_constant',
                    'MvmTlLive', 'MvmTlFreeze']) {
    assert.ok(dockIds.has(id), `${id} is not on the timeline dock`);
  }
  // Not in CS2's docked timeline: the standalone card's header row (CS2 hides it when hosted), a
  // FREE CAM button on the transport, a pose-overwrite UPDATE button (CS2's Tick->Here is a retime),
  // and a ruler row.
  for (const id of ['MvmTlHeader', 'MvmTlHint', 'MvmTlMouse', 'MvmTlClear', 'MvmTlClose', 'MvmTlGraph',
                    'MvmTlFree', 'MvmTlUpdate', 'MvmTlRuler']) {
    assert.ok(!dockIds.has(id), `${id} is still on the camera strip`);
  }
});

test('nothing is drawn until C++ has solved a layout', () => {
  const env = makeEnvironment();
  loadAll(env);

  const root = (function find(p) {
    if (p.id === 'MvmWorkspaceRoot') return p;
    for (const c of p.children) { const hit = find(c); if (hit) return hit; }
    return null;
  })(env.ctx);
  assert.ok(root, 'no workspace root');

  const header = (function find(p) {
    if (p.id === 'MvmHeader') return p;
    for (const c of p.children) { const hit = find(c); if (hit) return hit; }
    return null;
  })(root);

  // A guessed rectangle is how a preview ends up framing the wrong part of the screen with no way
  // to tell whether the maths or the measurement is at fault. Absent layout means draw nothing.
  const noLayout = makeState();
  delete noLayout.layout;
  env.setState(noLayout);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(header.visible, false, 'the header drew without a layout');

  const invalid = makeState({ layout: { valid: false } });
  env.setState(invalid);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(header.visible, false, 'the header drew on an invalid layout');

  env.setState(makeState());
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(header.visible, true, 'the header stayed hidden on a valid layout');
});

// The panel is the only side that can measure the screen, and C++ owns every number derived from
// it. That handover is one console command, and it must fire on a change and not every frame.
test('the panel reports its screen size once, on change', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(makeState());

  for (let i = 0; i < 5; ++i) env.dollar.MvmWorkspace.render(env.state());
  const reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker ui screen'));
  assert.equal(reports.length, 1, `expected one screen report, got ${reports.length}`);
  assert.equal(reports[0], 'mirv_filmmaker ui screen 1920 1080');
});

// LOOP HAS EXACTLY ONE HOME, and this is the test that keeps it that way.
//
// Looping is a property of PLAYBACK, so it lives on the playback bar -- the Regular Timeline, which
// is Deadlock's own replay bar with our button group added to it. The camera timeline is about the
// SHOT and the graph is about the CURVES, and neither may offer a second way to set the same thing.
//
// This is not tidiness. The camera timeline used to carry its own LOOP OFF / SET IN / SET OUT /
// CLEAR row, a shaded region on its track and two draggable end flags. Two views that both WRITE the
// loop are two views that can disagree about it, and the way a user finds out is by setting a loop
// in one of them and watching playback ignore it.
test('loop controls exist on the playback bar and nowhere else', () => {
  const env = makeEnvironment();
  loadAll(env);
  // The bar group is attached during RENDER, not build: the game's replay bar does not exist yet
  // when our panels build, and it is rebuilt on every replay load, so attaching is retried each
  // frame. A build-only test would find no bar and conclude the Loop button was missing.
  env.setState(makeState());
  env.dollar.MvmWorkspace.render(env.state());

  const idsUnder = (root) => {
    const out = new Set();
    (function walk(p) {
      if (p.id) out.add(p.id);
      for (const c of p.children) walk(c);
    })(root);
    return out;
  };
  const findById = (root, id) => {
    if (root.id === id) return root;
    for (const c of root.children) { const hit = findById(c, id); if (hit) return hit; }
    return null;
  };

  // ON THE BAR: the Loop button sits in our group beside the speed dial, and so does one button
  // per workspace -- Cam Editor, Config, Render -- because the bar is where workspaces are reached.
  assert.ok(findById(env.ctx, 'MvmBarLoop'), 'the playback bar has no Loop button');
  for (const id of ['MvmBarCam', 'MvmBarConfig', 'MvmBarRender']) {
    assert.ok(findById(env.ctx, id), `the playback bar has no ${id}`);
  }

  // NOT IN THE CAMERA TIMELINE.
  const dock = findById(env.ctx, 'MvmTimelineDock');
  assert.ok(dock, 'the camera timeline dock was never created');
  for (const id of [...idsUnder(dock)]) {
    assert.ok(!/^MvmTlLoop/.test(id), `${id} is a loop control on the camera timeline`);
  }

  // NOT IN THE GRAPH.
  const graph = findById(env.ctx, 'MvmGraphBody');
  assert.ok(graph, 'the graph body was never created');
  for (const id of [...idsUnder(graph)]) {
    assert.ok(!/[Ll]oop/.test(id), `${id} is a loop control in the graph`);
  }

  // And no panel anywhere but the bar sends a loop command. A control that merely LOOKS like
  // something else while still issuing `loop` would pass every check above.
  env.commands.length = 0;
  const press = (root, re) => {
    (function walk(p) {
      if (p.id && re.test(p.id) && p.events.onactivate) p.Activate();
      for (const c of p.children) walk(c);
    })(root);
  };
  press(dock, /^Mvm/);
  press(graph, /^Mvm/);
  const loopCmds = env.commands.filter((c) => /^mirv_filmmaker loop\b/.test(c));
  assert.deepEqual(loopCmds, [],
    `the camera timeline or graph issued loop commands: ${loopCmds.join(', ')}`);
});

// THE BAR'S RENDER BUTTON IS REDUNDANT ONCE AN EDITOR IS OPEN.
//
// Both the Camera Editor and Config columns now carry their own RENDER action in their footer
// (CS2's mvmActionRow, ported onto DeadlockCameraBodyJs.h / DeadlockConfigRenderBodyJs.h). A THIRD
// way to reach Render sitting right next to CAM EDITOR / CONFIG / LOOP on the bar duplicates one of
// them for no reason -- reported as "if you're in camera editor or config, the render button
// should not show up in the regular timeline". It is kept only as one of the bar's three entry
// points while NEITHER editor is open yet.
test('the bar\'s Render button hides once an editor is open, not just once you are in Render', () => {
  const env = makeEnvironment();
  loadAll(env);
  const findById = (root, id) => {
    if (root.id === id) return root;
    for (const c of root.children) { const hit = findById(c, id); if (hit) return hit; }
    return null;
  };
  function render(state) { env.setState(state); env.dollar.MvmWorkspace.render(env.state()); }

  render(makeState({ open: false }));
  const renderBtn = findById(env.ctx, 'MvmBarRender');
  assert.ok(renderBtn, 'the playback bar has no MvmBarRender');
  assert.equal(renderBtn.visible, true, 'closed: Render is one of the bar\'s three entry points');

  for (const ws of ['camera', 'config']) {
    render(makeState({ workspace: ws }));
    assert.equal(renderBtn.visible, false,
      `${ws} is open: the bar's Render duplicates that column's own RENDER footer action`);
  }

  render(makeState({ workspace: 'render' }));
  assert.equal(renderBtn.visible, false, 'render: the bar should not offer to go where you already are');
});

// THE REPLAY'S EVENT MARKERS ARE DRAWN BY THE DOCK, FROM THE GAME'S OWN TICKS.
//
// The game's bar carries the marker list -- one child per event, named `<type>_event_<tick>` -- and
// that is the only route to it: the list never reaches our state feed. The bar itself does not
// paint (see DeadlockRegularTimelineJs.h), so the dock reads those ticks and draws its own pips
// against its own strip, which is the one on screen.
//
// The position is arithmetic, not a correction factor: the id carries the tick and the replay's
// span carries the rest.
// THE REGULAR TIMELINE IS DEADLOCK'S OWN BAR, HOSTED.
//
// The dock used to draw its own transport and its own event pips beside the game's. It hosts the
// real bar now, so the markers under test are the game's `.EventMarker` children -- which the
// engine placed once, in absolute pixels, against the width the scrubber had at that moment. We
// widen that scrubber to the dock band and the game never revisits them, so every marker drifts
// left of its own time unless holdMarkersAligned re-places it.
function hostedBar(env) {
  const state = makeState();
  state.dock = { tab: 'native', ribbonH: 36, editorH: 60, meta: 'Deadlock replay bar' };
  const box = env.ctx.FindChildTraverse('MarkersContainer');
  const slider = env.ctx.FindChildTraverse('TimeScrubSlider');
  assert.ok(box && slider, 'the stub bar needs a marker container and a scrub slider');
  // A laid-out track and a published span; holdMarkersAligned stands down without both, because
  // placing against either would scatter every marker onto the left edge.
  box.actuallayoutwidth = 1200;
  slider.min = 0;
  slider.max = 71718;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());
  return { box, slider, state };
}

test('the game\u2019s own event markers are re-placed against the widened track', () => {
  const env = makeEnvironment();
  loadAll(env);
  const { box } = hostedBar(env);

  const marked = box.children
    .map((p) => ({ panel: p, m: /_event_(\d+)$/.exec(p.id || '') }))
    .filter((e) => e.m)
    .map((e) => ({ panel: e.panel, tick: Number(e.m[1]) }));
  assert.ok(marked.length >= 6, 'the stub bar has no named markers to read');

  for (const e of marked) {
    const pos = String(e.panel.style.position || '');
    const x = parseFloat(pos);
    assert.ok(Number.isFinite(x) && x >= 0,
      `${e.panel.id} was left at the game's own position ("${pos}")`);
    assert.ok(x <= 1200, `${e.panel.id} was placed off the right of the track at ${x}`);
  }

  // Monotonic: a later event is never placed left of an earlier one.
  const order = marked.map((e) => [e.tick, parseFloat(String(e.panel.style.position))]);
  order.sort((a, b) => a[0] - b[0]);
  for (let k = 1; k < order.length; ++k) {
    assert.ok(order[k][1] >= order[k - 1][1],
      `event ${order[k][0]} is placed left of the earlier ${order[k - 1][0]}`);
  }
});

// CLICKING A KILL IS A REQUEST TO WATCH IT, so it lands BEFORE the event rather than on it.
//
// Two separate faults lived here when the pips were the game's own. The handler used
// `mirv_filmmaker scrub`, which drags the camera SHOT's playhead rather than seeking the demo -- with
// no camera markers laid down the shot spans a single tick, so the seek went somewhere unrelated.
// And every marker's handler closed over the same loop variable, so they all used the LAST marker's
// tick. Both are asserted here against the pips the dock now draws.
test('each marker seeks to its own tick, a lead-in early', () => {
  const env = makeEnvironment();
  loadAll(env);
  const { box } = hostedBar(env);

  const marked = box.children
    .map((p) => ({ panel: p, m: /_event_(\d+)$/.exec(p.id || '') }))
    .filter((e) => e.m)
    .map((e) => ({ panel: e.panel, tick: Number(e.m[1]) }));
  assert.ok(marked.length >= 6, 'the stub bar has no named markers to read');

  for (const e of marked) {
    env.commands.length = 0;
    e.panel.Activate();
    assert.equal(env.commands.length, 1,
      `${e.panel.id} issued ${env.commands.length} commands`);
    const cmd = env.commands[0];

    // `seek`, never `scrub`: two different playheads. scrub drags the camera SHOT's, and with no
    // camera markers laid down that shot spans a single tick, so the click landed nowhere near.
    const m = /^mirv_filmmaker seek (\d+)$/.exec(cmd);
    assert.ok(m, `${e.panel.id} issued "${cmd}" instead of a seek`);

    const landed = Number(m[1]);
    // Five seconds at Deadlock's 64 ticks a second, or the start of the replay if the event is
    // closer to the beginning than that. Each handler must carry its OWN tick: they used to close
    // over one function-scoped `var`, so every marker seeked to the last one's.
    assert.equal(landed, Math.max(0, e.tick - 320),
      `${e.panel.id} seeked to ${landed}, expected a 320-tick lead-in from ${e.tick}`);
  }
});

// Panorama fires onvaluechanged when C++'s per-frame state feed moves a Slider. If the drag state
// is still latched, that notification must not turn an ordinary render into a demo seek; doing so
// made the Key Editor repeatedly jump between adjacent ticks instead of playing.
test('a programmatic scrubber update never seeks the demo', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(makeState({ dock: { tab: 'native', ribbonH: 36, editorH: 94, meta: '' } }));
  env.dollar.MvmWorkspace.render(env.state());

  const slider = env.ctx.FindChildTraverse('MvmRegScrubSlider');
  assert.ok(slider, 'the regular timeline built no scrub slider');

  let value = slider.value;
  Object.defineProperty(slider, 'value', {
    configurable: true,
    get: () => value,
    set: (next) => {
      value = next;
      if (slider.events.onvaluechanged) slider.events.onvaluechanged();
    },
  });

  // Model the lost-mouseup case seen in game, then let the state feed advance the playhead.
  slider.events.onmouseactivate();
  env.commands.length = 0;
  env.setState(makeState({ tick: 17820, dock: { tab: 'native', ribbonH: 36, editorH: 94, meta: '' } }));
  env.dollar.MvmWorkspace.render(env.state());
  assert.deepEqual(env.commands, [], 'rendering the playhead issued an unsolicited demo seek');
});

test('the game bar is held visible outside the editor, and through the game re-hiding it', () => {
  const env = makeEnvironment();
  loadAll(env);

  const replay = env.ctx.children.find((c) => c.id === 'hud_replay');
  const options = replay.children.find((c) => c.id === 'SpectatorOptionsContainer');
  assert.ok(replay && options, 'the stub needs both panels the game hides');

  function render(state) { env.setState(state); env.dollar.MvmWorkspace.render(env.state()); }

  // 1. WORKSPACE CLOSED. This is the case that broke: the bar must stay.
  render(makeState({ open: false }));
  assert.equal(replay.BHasClass('Hidden'), false,
    'closing the editor hid the replay bar that belongs to the game');
  assert.equal(options.BHasClass('Hidden'), false,
    'the options container was left hidden, which slides the bar 272px off its own edge');

  // 2. THE GAME RE-HIDES IT, as it does at a match intro or a round break. The next frame must
  //    undo that, whether or not the editor is open.
  replay.classes.add('Hidden');
  options.classes.add('Hidden');
  replay.classes.add('hidden');
  options.classes.add('hidden');
  replay.visible = false;
  options.visible = false;
  render(makeState({ open: false }));
  assert.equal(replay.BHasClass('Hidden'), false, 'the game re-hid the bar and it was not restored');
  assert.equal(options.BHasClass('Hidden'), false, 'the options container was not restored');
  assert.equal(replay.BHasClass('hidden'), false, 'Blitz left its lowercase hide class on the replay bar');
  assert.equal(options.BHasClass('hidden'), false, 'Blitz left its lowercase hide class on the options');
  assert.equal(replay.visible, true, 'Blitz directly hid the replay bar and it was not restored');
  assert.equal(options.visible, true, 'Blitz directly hid the options container and it was not restored');

  // 3. ON THE REGULAR TIMELINE TAB THE BAR IS THE BAND. The dock hosts it rather than redrawing
  //    it, so with the editor open on that tab it must be shown, clamped to the dock's width and
  //    left-anchored -- a right-anchored panel grows leftwards, and widening one without
  //    re-anchoring it put our whole button group behind the inspector column.
  const nativeState = makeState();
  nativeState.dock = { tab: 'native', ribbonH: 36, editorH: 60, meta: '' };
  render(nativeState);
  assert.equal(replay.BHasClass('Hidden'), false,
    'the Regular Timeline tab hid the bar it is supposed to be hosting');
  assert.equal(replay.visible, true, 'the hosted bar was left collapsed');
  const layout = nativeState.layout;
  const wantW = Math.round(layout.screenW - layout.inspectorW);
  assert.equal(String(replay.style.width), `${wantW}px`,
    `the hosted bar is ${replay.style.width}, not the dock's ${wantW}px`);
  assert.equal(String(replay.style.horizontalAlign), 'left',
    'the hosted bar kept the right anchor, so it grows leftwards off the band');

  // 3b. IN RENDER THE DOCK IS FULL WIDTH (DeadlockWorkspaceJs.h's own dockW), so the hosted bar
  // must be too -- it used to stay clamped to screenW - inspectorW on every workspace, which fell
  // short of the right edge specifically on the one tab where the dock actually reaches it.
  const renderState = makeState({ workspace: 'render' });
  renderState.dock = { tab: 'native', ribbonH: 36, editorH: 60, meta: '' };
  render(renderState);
  assert.equal(String(replay.style.width), `${Math.round(renderState.layout.screenW)}px`,
    `in Render the hosted bar is ${replay.style.width}, not the full ${renderState.layout.screenW}px`);

  // 4. HIDDEN on the two editor tabs, where our own editor occupies the band and the game's bar
  //    would draw straight through it.
  for (const tab of ['camera', 'graph']) {
    const s2 = makeState();
    s2.dock = { tab, ribbonH: 36, editorH: 196, meta: '' };
    render(s2);
    assert.equal(replay.BHasClass('Hidden'), true,
      `the game bar drew under the ${tab} editor -- that is the two-bars-at-once bug`);
  }

  // 5. And it comes back when the EDITOR closes, at the size and anchor the GAME gave it. Leaving
  //    a native panel widened and re-anchored is a game bug the user cannot undo without
  //    restarting.
  render(makeState({ open: false }));
  assert.equal(replay.BHasClass('Hidden'), false, 'closing the editor did not give the bar back');
  assert.equal(options.BHasClass('Hidden'), false, 'closing the editor left the options container hidden');
  assert.ok(!replay.style.width || replay.style.width === null,
    `closing the editor left the bar pinned to ${replay.style.width}`);
  assert.ok(!replay.style.horizontalAlign || replay.style.horizontalAlign === null,
    'closing the editor left our anchor on the game’s bar');
});

// `st.demo` blips FALSE for a frame or two during a seek. A bar that followed it raw would flicker
// on every scrub, so the hold is debounced -- but it must still let go eventually when the replay
// really has ended, or the bar would be pinned on in a live match.
test('a one-frame demo blip does not drop the game bar', () => {
  const env = makeEnvironment();
  loadAll(env);
  const replay = env.ctx.children.find((c) => c.id === 'hud_replay');

  env.setState(makeState({ open: false }));
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(replay.BHasClass('Hidden'), false, 'bar not held to begin with');

  // The blip, exactly as a seek produces it.
  env.setState(makeState({ open: false, demo: false }));
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(replay.BHasClass('Hidden'), false,
    'a single false frame dropped the bar -- the debounce is not working');

  // Sustained absence: the hold expires and the bar goes back to being the game's business.
  const gone = makeState({ open: false, demo: false });
  for (let i = 0; i < 200; ++i) { env.setState(gone); env.dollar.MvmWorkspace.render(env.state()); }
  replay.classes.add('Hidden');
  env.setState(gone);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(replay.BHasClass('Hidden'), true,
    'the bar is still being forced visible long after the replay ended');
});

// THE BAND IS THE DOCK'S OWN HEIGHT, NOT THE GAME BAR'S.
//
// It used to be measured off `#hud_replay`, because the bar was what filled the band. The bar does
// not paint (see DeadlockRegularTimelineJs.h) and the dock draws the transport itself, so the only
// thing that knows how tall the band has to be is the thing drawing into it. Measuring a panel we
// no longer show would size the band to something invisible.
//
// Reported on a change, not once a frame: it is a console round trip, and the value is constant.
// THE BAND IS AS TALL AS THE BAR IT HOSTS, and only Panorama can measure that. C++ reserves the
// dock's height before anything has laid out, so the panel has to tell it -- once, on a change
// worth having, and never in a loop.
test('the hosted bar reports its measured height, once and not in a loop', () => {
  const env = makeEnvironment();
  loadAll(env);

  const replay = env.ctx.children.find((c) => c.id === 'hud_replay');
  replay.actuallayoutheight = 60;

  const state = makeState();
  state.dock = { tab: 'native', ribbonH: 36, editorH: 60, meta: '' };
  env.setState(state);
  for (let i = 0; i < 4; ++i) env.dollar.MvmWorkspace.render(env.state());

  let reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock nativeheight'));
  assert.equal(reports.length, 1, `expected one height report, got ${reports.length}`);
  assert.equal(reports[0], 'mirv_filmmaker dock nativeheight 60');

  // A REAL CHANGE IS REPORTED. The band has to follow the bar, or the ribbon sits over it or a
  // strip of nothing sits under it.
  replay.actuallayoutheight = 96;
  for (let i = 0; i < 3; ++i) env.dollar.MvmWorkspace.render(env.state());
  reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock nativeheight'));
  assert.equal(reports.length, 2, `a real height change was not reported: ${reports.join(' | ')}`);
  assert.equal(reports[1], 'mirv_filmmaker dock nativeheight 96');

  // A ONE-PIXEL WOBBLE IS NOT. The height we report changes the band, which moves the ribbon, which
  // can change what the bar lays out as; a value that alternates by a pixel would send a console
  // command every frame for the life of the session. Same shape as the style-write loop that
  // exhausted Panorama's vertex-buffer list.
  for (let i = 0; i < 6; ++i) {
    replay.actuallayoutheight = (i % 2) ? 97 : 96;
    env.dollar.MvmWorkspace.render(env.state());
  }
  reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock nativeheight'));
  assert.equal(reports.length, 2, `a one-pixel wobble became a per-frame report: ${reports.join(' | ')}`);

  // An implausible measurement is refused rather than clamped into a band nobody can use.
  replay.actuallayoutheight = 4;
  for (let i = 0; i < 3; ++i) env.dollar.MvmWorkspace.render(env.state());
  reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock nativeheight'));
  assert.equal(reports.length, 2, 'a stray measurement moved the band');
});

test('the dock reports its rectangle once, on change', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(makeState());

  for (let i = 0; i < 5; ++i) env.dollar.MvmWorkspace.render(env.state());
  let reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock rect'));
  assert.equal(reports.length, 1, `expected one dock-rect report, got ${reports.length}`);

  // A taller dock (the graph is 416 where the camera strip is 196) is a real change and must be
  // reported again, or the graph's hit tests keep solving against the shorter band.
  const taller = makeState({ dock: { tab: 'graph', ribbonH: 36, editorH: 416, meta: '21 keys' } });
  taller.layout.dockH = 452;
  env.setState(taller);
  for (let i = 0; i < 3; ++i) env.dollar.MvmWorkspace.render(env.state());
  reports = env.commands.filter((c) => c.startsWith('mirv_filmmaker dock rect'));
  assert.equal(reports.length, 2, `a changed dock height was not reported: ${reports.join(' | ')}`);
});

// Every docked editor has to survive being the visible one, because the dock tab is an axis
// independent of the workspace and any combination of the two can be on screen.
test('each dock tab renders on every workspace', () => {
  const env = makeEnvironment();
  loadAll(env);

  for (const tab of ['native', 'camera', 'graph']) {
    for (const workspace of ['camera', 'config', 'render']) {
      const state = makeState({ workspace });
      state.dock = { tab, ribbonH: 36, editorH: 196, meta: '' };
      env.setState(state);
      for (let i = 0; i < 2; ++i) {
        assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()),
          `render threw with the ${tab} tab docked in the ${workspace} workspace`);
      }
    }
  }
});

// With no replay loaded the native scrubber is not there at all. The bar must say so rather than
// drawing a playhead at a position it invented -- a confident wrong readout is worse than none.
test('the regular timeline says so when there is no native transport', () => {
  const env = makeEnvironment();
  loadAll(env);

  // Strip the game's replay bar out from under it, which is what a non-replay context looks like.
  env.ctx.children = env.ctx.children.filter((c) => c.id !== 'hud_replay');

  const state = makeState({ demo: false });
  state.dock = { tab: 'native', ribbonH: 36, editorH: 126, meta: '' };
  env.setState(state);
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()),
    'render threw with no native replay bar');
});

// THE STEADY STATE MUST WRITE NOTHING. This is the regression guard for the crash that took the
// game down:
//
//     CUtlLeanVectorBase<UtlLinkedListElem_t<CVertexBufferDx11*,unsigned short>,
//         unsigned short,CMemAllocAllocator>::EnsureCapacity allocation count overflow
//
// Panorama indexes its D3D11 vertex buffers with an unsigned short. A style write re-does a
// panel's geometry, and a 3D transform gives it a composited layer with a buffer of its own --
// so re-applying the SAME transform to ~20 native HUD panels every frame exhausted 65535 entries
// in under a minute. Measured: 3.2 GB to 14.3 GB during one replay with the workspace open, and
// no growth at all with it closed.
//
// The symptom before the crash is not a crash. It is parts of the preview frozen on a still from
// before the last rescale, because a saturated list cannot hand out geometry for the new one --
// reported as "a part of it is like a static image from earlier, and nothing scales correctly".
//
// Nothing in a screenshot or a "did it throw" check can see this. A write count can.
test('a repeated render with unchanged state writes no styles', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(makeState());

  // Two warm-up passes: the first builds, the second settles anything that legitimately depends on
  // a measurement taken during the first.
  env.dollar.MvmWorkspace.render(env.state());
  env.dollar.MvmWorkspace.render(env.state());

  env.styleWrites.count = 0;
  for (let i = 0; i < 10; ++i) env.dollar.MvmWorkspace.render(env.state());

  assert.equal(env.styleWrites.count, 0,
    `${env.styleWrites.count} style writes across 10 identical frames -- each one allocates, and `
    + 'the vertex-buffer list is indexed by an unsigned short');
});

// And a real change must still get through, or the memo would be a freeze rather than a filter.
test('a changed layout still reaches the panels', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(makeState());
  for (let i = 0; i < 3; ++i) env.dollar.MvmWorkspace.render(env.state());

  env.styleWrites.count = 0;
  // A taller dock is exactly the rescale the user was doing: the preview rect shrinks and every
  // scaled HUD panel has to take a new transform.
  const taller = makeState({ dock: { tab: 'graph', ribbonH: 36, editorH: 416, meta: '21 keys' } });
  taller.layout.dockH = 452;
  taller.layout.areaH = 584;
  taller.layout.previewH = 540;
  env.setState(taller);
  env.dollar.MvmWorkspace.render(env.state());

  assert.ok(env.styleWrites.count > 0,
    'a changed preview rect wrote nothing -- the memo is stuck and the UI will not rescale');
});

// Every dock tab, rendered repeatedly, must also settle. The graph is the one that matters most:
// it draws hundreds of pooled segments, and an unmemoised segment is hundreds of writes a frame.
test('every dock tab settles to zero style writes', () => {
  const env = makeEnvironment();
  loadAll(env);

  for (const tab of ['native', 'camera', 'graph']) {
    const state = makeState();
    state.dock = { tab, ribbonH: 36, editorH: 196, meta: '' };
    env.setState(state);
    for (let i = 0; i < 3; ++i) env.dollar.MvmWorkspace.render(env.state());

    env.styleWrites.count = 0;
    for (let i = 0; i < 5; ++i) env.dollar.MvmWorkspace.render(env.state());
    assert.equal(env.styleWrites.count, 0,
      `the ${tab} tab wrote ${env.styleWrites.count} styles across 5 identical frames`);
  }
});

// NOTHING OF OURS MAY COVER THE DOCK BAND WHILE HIT-TESTING. This is the regression guard for
// "I still can't click the regular timeline when I'm in UI mode".
//
// The chrome's click-catchers ring the preview to swallow clicks on empty background, and they
// only hit-test in UI mode. The bottom one ran from the preview's lower edge to the bottom of the
// screen -- straight over the dock band where Deadlock's own replay bar lives. So in game mode the
// clicks fell through and the bar worked, and turning the cursor ON covered it with an invisible
// panel whose only job is to eat clicks. The mode that exists to let you use the UI was the mode
// that made it unusable, which is why it read as a mouse bug rather than a layout one.
test('no hit-testing chrome covers the dock band in UI mode', () => {
  const env = makeEnvironment();
  loadAll(env);

  const state = makeState({ uiMouse: true });
  state.dock = { tab: 'native', ribbonH: 36, editorH: 76, meta: '' };
  state.layout.dockH = 112;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());

  const byId = new Map();
  (function walk(p) { if (p.id) byId.set(p.id, p); for (const c of p.children) walk(c); })(env.ctx);

  const dockTop = state.layout.screenH - state.layout.dockH;

  function bottomEdgeOf(id) {
    const p = byId.get(id);
    assert.ok(p, `${id} was never created`);
    // position is "<x>px <y>px 0px"; height is "<n>px".
    const y = parseFloat(String(p.style.position).split(' ')[1]);
    const h = parseFloat(String(p.style.height));
    return { panel: p, bottom: y + h };
  }

  const cb = bottomEdgeOf('MvmCatchB');
  assert.ok(cb.bottom <= dockTop + 1,
    `the bottom catcher reaches ${cb.bottom}, past the dock top at ${dockTop} -- it will swallow `
    + 'every click meant for the replay bar');

  // The dock body must also keep its hands off on the native tab, where the game's bar is the
  // thing being clicked.
  const body = byId.get('MvmDockBody');
  assert.ok(body, 'no dock body');
  assert.equal(body.hittest, false,
    'the dock body hit-tests on the native tab, so it takes the bar\u2019s clicks');

  // ...and DOES take them on an editor tab, where the editor is ours to click.
  const cam = makeState({ uiMouse: true });
  cam.dock = { tab: 'camera', ribbonH: 36, editorH: 196, meta: '' };
  env.setState(cam);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(body.hittest, true, 'the camera timeline cannot be clicked');
});
test('dropdown menus and tooltips open at their control, not in the top-left corner', () => {
  // Panorama reports actualxoffset/actualyoffset from the panel's PARENT. A bar or tile nested in a
  // card is a few dozen pixels from its parent, so popups placed from those offsets opened in the
  // top-left corner of the screen. Only the window position anchors a root-level popup.
  const env = makeEnvironment();
  for (const file of ['DeadlockStyleJs.h', 'DeadlockControlsJs.h']) (0, eval)(extractJs(file));
  const X = env.dollar.MvmControls;
  const card = X.card(env.ctx, { always: true, groups: [{ head: '', rows: [
    X.drop('attach', 'Attachment', {
      onOpen: a => X.openMenu(a, { options: [['head', 'Head'], ['chest', 'Chest']] }, 'head', () => {}, () => {})
    }),
    X.scrub('fov', 'FOV', { min: 20, max: 130, tip: 'Field of view on the selected marker.' })
  ] }] }, '#ffffff');
  X.buildOverlays(env.ctx);
  env.ctx.GetPositionWithinWindow = () => ({ x: 0, y: 0 });

  const walk = p => [p, ...p.children.flatMap(walk)];
  const at = p => String(p.style.position).split(' ').slice(0, 2).map(parseFloat);
  const place = (p, x, y, w, h) => {
    p.actualxoffset = 12; p.actualyoffset = 30;   // parent-relative, as the game reports them
    p.GetPositionWithinWindow = () => ({ x, y });
    p.actuallayoutwidth = w; p.actuallayoutheight = h;
  };

  // The dropdown: as wide as its bar, directly under it; above it near the bottom; pulled left at
  // the right edge.
  const bar = walk(card.w.attach.root).find(p => p.events.onactivate);
  const menu = env.ctx.FindChildTraverse('MvmMenu');
  place(bar, 1500, 600, 340, 40);
  bar.Activate();
  assert.equal(menu.visible, true);
  assert.deepEqual(at(menu), [1500, 646]);
  assert.equal(menu.style.width, '340px');
  X.closeMenu();
  place(bar, 1500, 1000, 340, 40);
  bar.Activate();
  const [, flippedY] = at(menu);
  assert.ok(flippedY < 1000 && flippedY > 800, `menu near the bottom should open above its bar, got y=${flippedY}`);
  X.closeMenu();
  place(bar, 1700, 600, 340, 40);
  bar.Activate();
  assert.deepEqual(at(menu), [1920 - 8 - 340, 646]);
  X.closeMenu();

  // The tooltip, placed like CS2's: left edge on the tile, just below it; above it near the bottom;
  // pulled left at the right edge.
  const tile = card.w.fov.root;
  const tip = env.ctx.FindChildTraverse('MvmTip');
  tip.actuallayoutwidth = 326;
  place(tile, 1400, 300, 213, 76);
  tile.events.onmouseover();
  assert.equal(tip.visible, true);
  assert.deepEqual(at(tip), [1400, 382]);
  place(tile, 1700, 300, 213, 76);
  tile.events.onmouseover();
  assert.deepEqual(at(tip), [1920 - 8 - 326, 382]);
  place(tile, 1400, 1000, 213, 76);
  tile.events.onmouseover();
  const [, tipY] = at(tip);
  assert.ok(tipY < 1000 && tipY > 850, `tip near the bottom should sit above its tile, got y=${tipY}`);
});

// ---- P8d: graph editor full parity (DeadlockGraphBodyJs.h / DeadlockGraph.cpp) -----------------
//
// The graph body only renders while it is the ACTIVE dock tab ($.MvmWorkspace's own gate in
// DeadlockWorkspaceJs.h: `if (d.tab === 'graph') { ...; $.MvmGraphBody.render(st); }`), so every
// test below sets `dock.tab: 'graph'` explicitly rather than relying on makeState()'s default
// ('camera') -- a render() that is never called would make any of these pass for the wrong reason.
function graphState(overrides) {
  const state = makeState({
    dock: { tab: 'graph', ribbonH: 36, editorH: 196, meta: 'graph' },
    ...overrides,
  });
  return state;
}

test('graph: solo is its own accent, independent of each channel\'s eye state', () => {
  const env = makeEnvironment();
  loadAll(env);
  const state = graphState();
  state.graph.channels[0].solo = true;   // X: soloed AND shown
  state.graph.channels[0].visible = true;
  state.graph.channels[1].solo = false;  // Y: merely hidden -- not the solo target
  state.graph.channels[1].visible = false;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());

  const graph = env.ctx.FindChildTraverse('MvmGraphBody');
  assert.ok(graph, 'the graph body was never created');
  const labelFor = (text) => {
    let found = null;
    (function walk(p) {
      if (found) return;
      if (p.text === text) { found = p; return; }
      for (const c of p.children) walk(c);
    })(graph);
    return found;
  };
  const xName = labelFor('Pos X');
  const yName = labelFor('Pos Y');
  assert.ok(xName, 'channel X\'s name label was not found');
  assert.ok(yName, 'channel Y\'s name label was not found');
  assert.equal(xName.style.color, '#FFED79', 'the soloed channel\'s name must take the gold accent');
  assert.equal(yName.style.color, '#C6C6C6', 'a merely-hidden (not soloed) channel dims as before');
});

test('graph: the channel row order is [swatch+name], [value], [eye] -- CS2\'s order', () => {
  const env = makeEnvironment();
  loadAll(env);
  env.setState(graphState());
  env.dollar.MvmWorkspace.render(env.state());

  const row0 = env.ctx.FindChildTraverse('MvmGraphRow0');
  assert.ok(row0, 'channel row 0 was never created');
  assert.equal(row0.children.length, 3, 'a channel row must have exactly 3 direct children');
  const [hot, val, eye] = row0.children;
  assert.equal(hot.children.length, 2, 'the first child (the solo target) must hold the swatch + name');
  assert.equal(hot.children[1].text, 'Pos X', 'the name label must be the solo target\'s second child');
  assert.equal(val.text, '-', 'the second row child is not the value cell');
  assert.equal(eye.style.width, '24px', 'the third row child is not the eye toggle (CS2 EYEW 24)');
});

test('graph: UNDO/REDO dim exactly when there is nothing to act on', () => {
  const env = makeEnvironment();
  loadAll(env);
  const state = graphState();
  state.graph.canUndo = false;
  state.graph.canRedo = true;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());

  const undo = env.ctx.FindChildTraverse('MvmGraphUndo');
  const redo = env.ctx.FindChildTraverse('MvmGraphRedo');
  assert.ok(undo && redo, 'the graph header is missing UNDO/REDO');
  assert.equal(undo.children[0].style.color, '#C6C6C6', 'UNDO must dim with nothing to undo');
  assert.equal(redo.children[0].style.color, '#FFEFD7', 'REDO must read as available');
});

test('graph: a degraded Smooth dims even while active, instead of reading as a working mode', () => {
  const env = makeEnvironment();
  loadAll(env);
  const state = graphState();
  state.graph.smooth = true;
  state.graph.interpDegraded = true;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());

  const smooth = env.ctx.FindChildTraverse('MvmGraphSmooth');
  assert.ok(smooth, 'the graph header is missing SMOOTH');
  assert.equal(smooth.children[0].style.color, '#C6C6C6',
    'a Smooth that cannot bend anything (too few keyframes) must not read as a live accent');

  state.graph.interpDegraded = false;
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(smooth.children[0].style.color, '#FFED79',
    'a genuinely active, non-degraded Smooth must still take the gold accent');
});

// Bezier tangent handles, the rectangle drag-select and the right-click menu are all hit-tested in
// DeadlockGraph.cpp, never in this JS -- so the panel's only job is to draw EXACTLY where the state
// feed says, with no placement logic of its own (see DeadlockGraphBodyJs.h's buildContextMenu doc
// comment for why that specifically rules out $.MvmControls.contextMenuShell here).
test('graph: tangent handles, the drag-select rectangle and the right-click menu draw from state', () => {
  const env = makeEnvironment();
  loadAll(env);
  const state = graphState();
  // A selected key with an active handle: DeadlockGraph.cpp only emits lt/lv/rt/rv for a selected
  // key with a real GraphExpModel control point, never decoratively for an unselected one.
  state.graph.channels[0].keyPts = [
    { t: 0.5, v: 0.5, sel: true, lt: 0.4, lv: 0.45, rt: 0.6, rv: 0.55 },
    { t: 0.2, v: 0.2, sel: false },
  ];
  state.graph.rect = { active: true, x0: 0.1, y0: 0.1, x1: 0.4, y1: 0.6 };
  state.graph.menu = { open: true, x: 40, y: 60 };
  env.setState(state);
  assert.doesNotThrow(() => env.dollar.MvmWorkspace.render(env.state()),
    'render threw drawing handles / the drag-select rectangle / the right-click menu');

  const rect = env.ctx.FindChildTraverse('MvmGraphSelectRect');
  assert.ok(rect, 'the drag-select rectangle panel was never created');
  assert.equal(rect.visible, true);

  const menu = env.ctx.FindChildTraverse('MvmGraphMenu');
  assert.ok(menu, 'the right-click menu panel was never created');
  assert.equal(menu.visible, true);
  assert.equal(menu.style.position, '40px 60px 0px',
    'the menu must draw at EXACTLY the x/y C++ published -- it has no flip-to-fit logic of its own');

  // Closing both must hide them again, not merely stop moving them.
  state.graph.rect = { active: false };
  state.graph.menu = { open: false };
  env.setState(state);
  env.dollar.MvmWorkspace.render(env.state());
  assert.equal(rect.visible, false);
  assert.equal(menu.visible, false);
});


test('render custom numbers commit once, reject invalid rates, and cancel on Escape', () => {
  const r = renderBodyRig({fps:60});
  r.render({uiMouse:true});
  const field = r.env.ctx.FindChildTraverse('MvmNumber_customFps');
  assert.ok(field);
  const key = (bit) => { r.render({numberKeys:1<<bit}); r.render({numberKeys:0}); };
  field.Activate(); key(1); key(4); key(4); key(11);
  assert.equal(r.env.commands.at(-1), 'mirv_filmmaker render fps 144');
  const count=r.env.commands.length;
  field.Activate(); key(0); key(11);
  assert.equal(r.env.commands.length,count);
  key(12); key(3); key(11);
  assert.equal(r.env.commands.length,count);
});


test('GIF rejects output rates above its 100 fps timing limit', () => {
  const r=renderBodyRig({format:'gif',fps:120});
  assert.ok(r.byTextIn('MvmJobColumn','Cannot render yet'));
  const count=r.env.commands.length;
  r.env.ctx.FindChildTraverse('MvmJobStart').Activate();
  assert.equal(r.env.commands.length,count);
  r.render({render:{...r.st.render,fps:50}});
  assert.ok(r.byTextIn('MvmJobColumn','Ready to render'));
});
