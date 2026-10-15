// The harness: loads the extracted workspace script, stands up Deadlock's own HUD around it, drives
// it a frame at a time, and routes the commands it issues back into the state feed.
//
// The sequence mirrors the game's exactly, because getting it wrong is indistinguishable from a bug
// in the panel:
//
//   1. The HUD exists first. In game our hook attaches to a HUD that is already running, and the
//      Regular Timeline host goes looking for `#hud_replay` -- so the native tree is built BEFORE
//      the workspace script runs, not after.
//   2. The fragments run in one shared scope, in the order DeadlockWorkspaces.cpp pushes them.
//      That is a single concatenated file, which is what generated/workspace.js is.
//   3. $.MvmWorkspace.build() once.
//   4. $.MvmWorkspace.render(state) every frame, with the state object the C++ would have composed.
//
// And it BOOTS THE WAY THE GAME DOES after a replay loads: nothing of ours open, the game's own
// replay bar carrying our Cam Editor / Config / Render / Loop buttons, the director card up. Every
// workspace, tab and popup is reached from there by clicking inside the UI or by the hook's own
// hotkeys (read out of DeadlockHotkeys.cpp). The sidebar exists for the popups, overlays and
// reported states a click cannot reach.
//
// Nothing here reaches into the panels. The only ways in are render() and the command router, which
// are the only two the game has.

import { solve } from './lib/workspace-layout.mjs';

const host = document.getElementById('host');
const stage = document.querySelector('.stage');
const stagewrap = document.getElementById('stagewrap');
const backdrop = document.getElementById('backdrop');
const viewport = document.getElementById('viewport');
const statusEl = document.getElementById('status');
const devEl = document.getElementById('dev');
const devBody = document.getElementById('devbody');
const viewList = document.getElementById('viewlist');
const actionsEl = document.getElementById('actions');

const STAGE_W = 1920;
const STAGE_H = 1080;

// The shim divides every measurement it hands the script by this, so the panel works in the logical
// 1920x1080 space the game does no matter how the page is zoomed.
window.MvmDeadlockMockup = { stageScale: 1, onLog: null };

let manifest = null;
let model = null;
let router = null;
let native = null;
let view = null;
let devTab = 'commands';
let buildError = null;
let lastFrameAt = 0;
let renderErrors = [];
let scriptText = '';
let scriptStamp = '';
const keyLog = [];

// Mirror the game's client-pixel cursor feed. Value tiles, speed dials and the inspector grip
// consume this same state; emulating invisible Slider events hid the broken Deadlock controls.
function feedCursor(e) {
  if (!model) return;
  const r = stage.getBoundingClientRect(), k = window.MvmDeadlockMockup.stageScale || 1;
  model.mx = (e.clientX - r.left) / k;
  model.my = (e.clientY - r.top) / k;
  model.lmb = !!(e.buttons & 1); model.rmb = !!(e.buttons & 2); model.shift = !!e.shiftKey;
}
window.addEventListener('pointermove', feedCursor);
window.addEventListener('pointerdown', feedCursor);
window.addEventListener('pointerup', feedCursor);
window.addEventListener('blur', () => { if (model) { model.lmb = model.rmb = model.shift = false; model.numberKeys = 0; } });

// ---------------------------------------------------------------- keys
// Two things the C++ polls with GetAsyncKeyState, fed from the browser's real keyboard:
//
//   * numberKeys (DeadlockWorkspaces.cpp): the digit / Backspace / Enter / Escape bitmask the
//     value tiles type from, only while the page has focus and the UI owns the mouse.
//   * the hotkeys (DeadlockHotkeys.cpp g_bindings[]): read out of the C++ by the extractor into
//     manifest.hotkeys, and gated here exactly as Hotkeys_RunMainThreadFrame gates them -- focused,
//     a replay loaded, the workspace open where the row says so, standing down for the free cam
//     where the row says so, and never twice inside the bounce window.
const NUMBER_BITS = { Backspace: 10, Enter: 11, Escape: 12 };
function numberBit(e) {
  if (/^Digit[0-9]$/.test(e.code)) return Number(e.code.slice(5));
  if (/^Numpad[0-9]$/.test(e.code)) return Number(e.code.slice(6));
  return NUMBER_BITS[e.key] === undefined ? -1 : NUMBER_BITS[e.key];
}
function keysOwnedByChrome(e) {
  const tag = (e.target && e.target.tagName) || '';
  return /^(INPUT|TEXTAREA|SELECT)$/.test(tag) || (e.target && e.target.isContentEditable);
}
const RESOLVERS = {
  // DeadlockHotkeys.cpp PlayPauseCommand: the live pause state decides.
  PlayPauseCommand: (m) => (m.paused ? 'demo_resume' : 'demo_pause'),
};
const BOUNCE_MS = 250;
const keyDown = new Set();
window.addEventListener('keydown', (e) => {
  if (!model) return;
  model.shift = !!e.shiftKey;
  if (keysOwnedByChrome(e)) return;
  const bit = numberBit(e);
  if (bit >= 0 && model.uiMouse) model.numberKeys |= (1 << bit);
  // Edge detection runs even while gated off, as in the C++: a key held across the gate opening
  // must not fire on the first active frame.
  const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
  const pressed = !keyDown.has(key);
  keyDown.add(key);
  if (!pressed) return;
  for (const b of (manifest && manifest.hotkeys) || []) {
    if (b.key !== key) continue;
    const gate = document.hasFocus() && model.demo && (!b.requiresWorkspace || model.open);
    if (!gate) { keyLog.push(`${b.name}: gated off`); continue; }
    if (b.standsDownForFreeCam && model.freecam) { keyLog.push(`${b.name}: free cam has input`); continue; }
    const now = performance.now();
    if (b.lastMs && now - b.lastMs < BOUNCE_MS) continue;
    const line = b.resolve ? (RESOLVERS[b.resolve] ? RESOLVERS[b.resolve](model) : null) : b.command;
    if (!line) continue;
    b.lastMs = now;
    keyLog.push(`${b.name} -> ${line}`);
    if (keyLog.length > 60) keyLog.shift();
    e.preventDefault();
    router.run(line);
  }
});
window.addEventListener('keyup', (e) => {
  if (!model) return;
  model.shift = !!e.shiftKey;
  const bit = numberBit(e);
  if (bit >= 0) model.numberKeys &= ~(1 << bit);
  keyDown.delete(e.key.length === 1 ? e.key.toUpperCase() : e.key);
});

// The graph's right-click Ease/Align/Delete menu. In game DeadlockGraph.cpp opens it from the
// OS-side cursor probe and publishes it in the graph body's local space; here a right-click on
// the graph band opens it at the same local coordinates and any click closes it.
stage.addEventListener('contextmenu', (e) => {
  if (!model || !model.open || model.dockTab !== 'graph') return;
  const st = model.lastState;
  if (!st || !st.layout || !st.layout.valid) return;
  const k = window.MvmDeadlockMockup.stageScale || 1;
  const r = stage.getBoundingClientRect();
  const px = (e.clientX - r.left) / k, py = (e.clientY - r.top) / k;
  const plotY = st.graph.plotY, plotH = st.graph.plotH, plotW = st.graph.plotW;
  if (px < 0 || px > plotW || py < plotY || py > plotY + plotH) return;
  e.preventDefault();
  model.graphMenu = { open: true, x: Math.round(px), y: Math.round(py - plotY) };
});
stage.addEventListener('pointerdown', (e) => {
  if (model && model.graphMenu.open && e.button === 0) model.graphMenu.open = false;
});

// ---------------------------------------------------------------- scale

function fitStage(mode) {
  const pad = 24;
  let k;
  if (mode === 'fit') {
    k = Math.min(
      (viewport.clientWidth - pad) / STAGE_W,
      (viewport.clientHeight - pad) / STAGE_H
    );
    k = Math.min(k, 1);
  } else {
    k = mode === '200' ? 2 : 1;
  }
  window.MvmDeadlockMockup.stageScale = k;
  stage.style.transform = `scale(${k})`;
  stagewrap.style.width = `${STAGE_W * k}px`;
  stagewrap.style.height = `${STAGE_H * k}px`;
  // #backdrop is a SIBLING of .stage, so it never receives the stage's scale -- its own background
  // size has to track it, or a real capture paints at native size inside a box half that wide and
  // you get the top-left corner of the frame blown up behind correctly-scaled panels.
  backdrop.style.setProperty('--shot-w', `${STAGE_W * k}px`);
  for (const [id, m] of [['btn-fit', 'fit'], ['btn-100', '100'], ['btn-200', '200']]) {
    document.getElementById(id).classList.toggle('on', mode === m);
  }
}

// ---------------------------------------------------------------- the surface

async function loadScript() {
  // Cache-busted on purpose: the whole point of editing generated/workspace.js is to see the result,
  // and a 304 would show the previous one.
  const res = await fetch(`generated/workspace.js?t=${Date.now()}`);
  if (!res.ok) throw new Error(`generated/workspace.js: ${res.status} -- run extract-deadlock-ui.mjs`);
  return res.text();
}

function buildSurface() {
  buildError = null;
  renderErrors = [];

  const ctx = window.PanoramaShim.reset(host);
  window.PanoramaShim.onCommand = (cmd) => router.run(cmd);

  // The game's HUD first, then ours on top of it -- the order the hook sees.
  native = window.MvmDeadlockNativeHud.build(ctx);

  try {
    // Indirect eval, so the script runs in global scope exactly as RunScript executes it. A direct
    // eval would give it a module scope, and every `$.MvmX = ...` registration would land somewhere
    // the next fragment could not see.
    (0, eval)(scriptText);
  } catch (e) {
    buildError = `script threw while loading: ${e && e.stack ? e.stack : e}`;
    return;
  }

  if (!window.$.MvmWorkspace) {
    buildError = '$.MvmWorkspace did not register -- a fragment died before the shell ran';
    return;
  }
  try {
    window.$.MvmWorkspace.build();
  } catch (e) {
    buildError = `build() threw: ${e && e.stack ? e.stack : e}`;
  }
}

// A view is a place: which workspace is open and which editor is docked. `closed` only shuts the
// workspace -- the dock tab is remembered across closes in game (DeadlockDock.cpp g_tab), so it is
// left alone.
function applyView(v) {
  view = v;
  if (v.workspace === 'off') {
    model.open = false;
  } else {
    model.open = true;
    model.workspace = v.workspace;
    model.dockTab = v.tab;
  }
  syncSidebar(true);
}

// The view the model is in right now, derived every frame so the sidebar follows what the UI's
// own buttons did rather than the last row that was clicked.
function viewFor(m) {
  const ws = m.open ? m.workspace : 'off';
  return manifest.views.find((v) => v.workspace === ws && (ws === 'off' || v.tab === m.dockTab))
    || { id: ws === 'off' ? 'closed' : `${ws}-${m.dockTab}`, label: ws === 'off' ? 'Replay only (nothing open)' : `${ws} + ${m.dockTab}`, workspace: ws, tab: m.dockTab };
}
let lastViewId = null;
function syncSidebar(force) {
  const v = viewFor(model);
  if (!force && v.id === lastViewId) return;
  lastViewId = v.id;
  view = v;
  for (const b of viewList.querySelectorAll('.view-btn')) {
    b.classList.toggle('active', b.dataset.id === v.id);
  }
}

// ---------------------------------------------------------------- the frame

function frame(now, oneShot) {
  const dt = lastFrameAt ? Math.min(0.1, (now - lastFrameAt) / 1000) : 0;
  lastFrameAt = now;

  router.tick(dt);
  window.MvmDeadlockNativeHud.tick(native, model);

  if (!buildError) {
    const state = window.MvmDeadlockState.project(model, solve);
    try {
      window.$.MvmWorkspace.render(state);
    } catch (e) {
      // In game a throw here is swallowed by the C++ wrapper and reported as one console line, so the
      // panel simply stops updating. Surfacing it is the single biggest thing this harness adds.
      const msg = String(e && e.stack ? e.stack : e);
      if (!renderErrors.includes(msg)) renderErrors.push(msg);
    }
    model.lastState = state;
  }

  syncSidebar(false);
  paintStatus();
  if (devEl.classList.contains('open')) paintDev();
  if (!oneShot) schedule();
}

// requestAnimationFrame is suspended while the tab (or the embedding pane) is hidden, which would
// freeze the state feed and make every control look dead on the next look. Fall back to a timer
// when hidden so the panel keeps rendering and state stays live either way.
function schedule() {
  if (document.hidden) setTimeout(() => frame(performance.now()), 33);
  else requestAnimationFrame(frame);
}

function paintStatus() {
  const w = window.PanoramaShim.styleWrites();
  const parts = [
    `<b>${view ? view.label : ''}</b>`,
    `workspace=${model.open ? model.workspace : 'off'}`,
    `dock=${model.dockTab}`,
    `tick=${model.tick}`,
    `speed=${model.speed.toFixed(2)}x`,
    `mouse=${model.uiMouse ? 'UI' : 'GAME'}`,
    model.blackout ? '<span class="warn">F8 BLACKOUT</span>' : '',
    `style writes=${w.count}`,
  ].filter(Boolean);
  if (buildError) parts.push('<span class="warn">BUILD FAILED &mdash; see Dev</span>');
  else if (renderErrors.length) parts.push(`<span class="warn">render threw &times;${renderErrors.length}</span>`);
  if (router.gaps.length) parts.push(`<span class="warn">${router.gaps.length} gaps</span>`);
  statusEl.innerHTML = parts.join(' &nbsp;|&nbsp; ');
}

// ---------------------------------------------------------------- dev panel

function geometryDump() {
  // The same walk Automation/livejs reports from the running game, in the same format, so a line from
  // here and a line from there can be compared directly. depth id type x y w h visible text
  const out = [];
  const hostRect = host.getBoundingClientRect();
  const k = window.MvmDeadlockMockup.stageScale || 1;
  const walk = (panel, depth) => {
    const r = panel.el.getBoundingClientRect();
    // As the game's probe reports it: Panorama's `.text` getter answers the DISPLAYED string, so a
    // label styled text-transform:uppercase reads back in capitals, and only line breaks and tabs
    // are collapsed -- runs of spaces are part of the string.
    let text = (panel.text || '').toString().replace(/[\r\n\t]+/g, ' ').slice(0, 40);
    try {
      const tt = getComputedStyle(panel.textEl || panel.el).textTransform;
      if (tt === 'uppercase') text = text.toUpperCase();
      else if (tt === 'lowercase') text = text.toLowerCase();
    } catch { /* detached */ }
    out.push(
      `${depth} ${panel.id || '-'} ${panel.type} ` +
      `${Math.round((r.left - hostRect.left) / k)} ${Math.round((r.top - hostRect.top) / k)} ` +
      `${Math.round(r.width / k)} ${Math.round(r.height / k)} ` +
      `${panel.visible ? 1 : 0}${text ? ' ' + text : ''}`
    );
    for (const c of panel.children) walk(c, depth + 1);
  };
  const root = window.PanoramaShim.findById('MvmWorkspaceRoot');
  if (root) walk(root, 0);
  const bar = window.PanoramaShim.findById('hud_replay');
  if (bar) { out.push('--- the game\'s own replay bar ---'); walk(bar, 0); }
  return out.join('\n');
}

function paintDev() {
  let html = '';
  if (buildError) {
    html += `<span class="hdr">BUILD FAILED</span><span class="gap">${esc(buildError)}</span>\n\n`;
  }
  if (renderErrors.length) {
    html += `<span class="hdr">render() threw</span>`;
    html += renderErrors.map((e) => `<span class="gap">${esc(e)}</span>`).join('\n') + '\n\n';
  }

  if (devTab === 'commands') {
    html += '<span class="hdr">Console commands this UI issued</span>';
    html += router.log.length
      ? router.log.slice(-120).reverse().map((l) => `<span class="cmd">${esc(l)}</span>`).join('\n')
      : '(none yet -- click something)';
  } else if (devTab === 'log') {
    html += '<span class="hdr">$.Msg output (everything that was NOT a command)</span>';
    html += window.PanoramaShim.logs().slice(-120).reverse().map(esc).join('\n') || '(nothing)';
  } else if (devTab === 'gaps') {
    html += '<span class="hdr">Commands with no mockup behaviour</span>';
    html += router.gaps.length
      ? router.gaps.slice(-60).reverse().map((g) => `<span class="gap">${esc(g.line)}</span>\n    ${esc(g.why)}`).join('\n')
      : '(none -- every command the UI issued moved the state)';
    const unknown = window.PanoramaShim.unknownStyleProps();
    const keys = Object.keys(unknown);
    html += '\n\n<span class="hdr">Style properties the shim did not map</span>';
    html += keys.length
      ? keys.map((k) => `<span class="gap">${esc(k)}</span> x${unknown[k]}`).join('\n')
      : '(none)';
  } else if (devTab === 'geometry') {
    html += '<span class="hdr">depth id type x y w h visible text</span>';
    html += esc(geometryDump());
  } else if (devTab === 'state') {
    html += '<span class="hdr">The object handed to $.MvmWorkspace.render()</span>';
    html += esc(JSON.stringify(model.lastState || {}, null, 1));
  } else if (devTab === 'keys') {
    html += '<span class="hdr">Hotkeys (DeadlockHotkeys.cpp g_bindings[], via the extractor)</span>';
    html += ((manifest && manifest.hotkeys) || []).map((b) =>
      `${esc(b.name.padEnd(16))} ${esc(b.command || ('(' + b.resolve + ')'))}` +
      (b.requiresWorkspace ? '  [workspace must be open]' : '') +
      (b.standsDownForFreeCam ? '  [stands down for the free cam]' : '')).join('\n') || '(none parsed)';
    html += '\n\n<span class="hdr">Presses</span>';
    html += keyLog.slice().reverse().map(esc).join('\n') || '(none yet)';
    html += `\n\nnumberKeys=${model.numberKeys}  uiMouse=${model.uiMouse}  focused=${document.hasFocus()}`;
  }
  devBody.innerHTML = html;
}

function esc(s) {
  return String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
}

// ---------------------------------------------------------------- reaching the popups
// Most dialogs are opened by an internal function with no api entry point, so they are reached
// the way a user reaches them: find the real control by its label and fire its own handler.
function livePanels() {
  return [...host.querySelectorAll('.pnl')].map((e) => e.__panel).filter((p) => p && p.visible !== false && p.el.getBoundingClientRect().width > 0);
}
function activateByText(needle) {
  const want = String(needle).toUpperCase();
  for (let p of livePanels()) {
    if (!p.text || String(p.text).toUpperCase().indexOf(want) < 0) continue;
    while (p && !(p._events && p._events.onactivate)) p = p.parent;
    if (p) { p._events.onactivate(); return true; }
  }
  window.PanoramaShim.logs().push(`[action] no activatable control labelled: ${needle}`);
  return false;
}
function run(cmd) { router.run(cmd); }
function after(ms, fn) { setTimeout(fn, ms); }

// Overlays that open from code rather than from a control that is visible at rest. `needs` is the
// place the popup lives, reached by the same commands the UI sends; `run` then does what a person
// would do there.
const ACTIONS = [
  { owner: 'Camera Editor', items: [
    ['Confirm: Clear all markers', ['mirv_filmmaker workspace camera'], () => activateByText('Clear all')],
    ['Dropdown: Hero (follow target)', ['mirv_filmmaker workspace camera'], () => activateByText('Select hero') || activateByText('Hero')],
  ] },
  { owner: 'Camera Timeline', items: [
    ['Shake menu (right-click a diamond)', ['mirv_filmmaker workspace camera', 'mirv_filmmaker dock camera'], () => {
      const key = livePanels().find((p) => p.__mvmMarkerIndex !== undefined && p._events && p._events.oncontextmenu);
      if (key) { key._events.oncontextmenu(); return true; }
      window.PanoramaShim.logs().push('[action] no timeline diamond to right-click');
      return false;
    }],
  ] },
  { owner: 'Graph', items: [
    ['Right-click menu (Ease / Align / Delete)', ['mirv_filmmaker workspace camera', 'mirv_filmmaker dock graph'], () => {
      model.graphMenu = { open: true, x: 260, y: 90 };
    }],
  ] },
  { owner: 'Render', items: [
    ['Render job running', ['mirv_filmmaker workspace render'], () => run('mirv_filmmaker render start')],
    ['Render job stopped', [], () => run('mirv_filmmaker render stop')],
  ] },
  { owner: 'HUD', items: [
    ['Director card (close the workspace)', ['mirv_filmmaker workspace off'], null],
    ['Blackout (F8)', [], () => run('mirv_filmmaker blackout toggle')],
    ['Close the workspace', ['mirv_filmmaker workspace off'], null],
  ] },
];

// State presets: overlays that are a STATE the C++ reports rather than a click. They stack, and
// each is a toggle -- press it again to undo just that one, or Reset to undo all of them.
const SCENARIOS = [
  ['Loop off', { loopOn: false }],
  ['No markers', { markerList: [], selected: -1 }],
  ['Path playing', { playing: true, pathOwnsView: true, paused: false }],
  ['Render capturing', { renderState: 'Capturing' }],
  ['HUD hidden', { hudHidden: true }],
  ['Mouse: GAME', { uiMouse: false }],
  ['No replay loaded', { demo: false }],
  ['Free cam has input', { freecam: true }],
];

// Patches merge ONE LEVEL into nested objects, so a scenario that flips one field of `follow` does
// not replace the rest of it.
function mergePatch(target, patch) {
  for (const k of Object.keys(patch)) {
    const v = patch[k];
    const isPlain = v && typeof v === 'object' && !Array.isArray(v);
    if (isPlain && target[k] && typeof target[k] === 'object' && !Array.isArray(target[k])) Object.assign(target[k], v);
    else target[k] = v;
  }
}
function snapshotFor(cur, patch) {
  const undo = {};
  for (const k of Object.keys(patch)) {
    const v = patch[k], curv = cur[k];
    if (v && typeof v === 'object' && !Array.isArray(v) && curv && typeof curv === 'object' && !Array.isArray(curv)) {
      undo[k] = {};
      for (const kk of Object.keys(v)) undo[k][kk] = JSON.parse(JSON.stringify(curv[kk] === undefined ? null : curv[kk]));
    } else {
      undo[k] = JSON.parse(JSON.stringify(curv === undefined ? null : curv));
    }
  }
  return undo;
}
let stateUndo = {};

function act(label) {
  const want = String(label).trim().toLowerCase();
  const btns = [...actionsEl.querySelectorAll('button')];
  const hit = btns.find((b) => b.textContent.trim().toLowerCase() === want)
    || btns.find((b) => b.textContent.trim().toLowerCase().includes(want));
  if (!hit) return false;
  hit.click();
  return true;
}

function buildActions() {
  actionsEl.innerHTML = '';
  const label = (text, cls) => {
    const h = document.createElement('div');
    h.className = cls || 'group-label';
    h.textContent = text;
    actionsEl.appendChild(h);
  };
  label('Popups / overlays');
  for (const grp of ACTIONS) {
    label(grp.owner, 'action-owner');
    for (const a of grp.items) {
      const b = document.createElement('button');
      b.className = 'action-btn';
      b.textContent = a[0];
      b.addEventListener('click', () => {
        b.blur();
        for (const c of a[1] || []) router.run(c);
        if (!a[2]) return;
        // Let the workspace lay out before reaching for a control inside it.
        let tries = 0;
        (function attempt() {
          let done = false;
          try { done = a[2]() !== false; } catch (e) { window.PanoramaShim.logs().push('[action] ' + e); }
          if (!done && ++tries < 8) setTimeout(attempt, 150);
        })();
      });
      actionsEl.appendChild(b);
    }
  }
  label('States  (combine; click again to undo)');
  stateUndo = {};
  for (const s of SCENARIOS) {
    const b = document.createElement('button');
    b.className = 'action-btn';
    b.textContent = s[0];
    b.addEventListener('click', () => {
      b.blur();
      if (stateUndo[s[0]]) { mergePatch(model, stateUndo[s[0]]); delete stateUndo[s[0]]; b.classList.remove('on'); return; }
      stateUndo[s[0]] = snapshotFor(model, s[1]);
      mergePatch(model, s[1]);
      b.classList.add('on');
    });
    actionsEl.appendChild(b);
  }
  const r = document.createElement('button');
  r.className = 'action-btn';
  r.textContent = 'Reset states';
  r.addEventListener('click', () => {
    r.blur();
    for (const k of Object.keys(stateUndo).reverse()) mergePatch(model, stateUndo[k]);
    stateUndo = {};
    for (const x of actionsEl.querySelectorAll('.action-btn.on')) x.classList.remove('on');
  });
  actionsEl.appendChild(r);
}

// ---------------------------------------------------------------- boot

function buildSidebar() {
  buildActions();
  viewList.innerHTML = '';
  for (const v of manifest.views) {
    const b = document.createElement('button');
    b.className = 'view-btn';
    b.dataset.id = v.id;
    b.innerHTML = `${esc(v.label)}<span class="note">${esc(v.note)}</span>`;
    b.addEventListener('click', () => { b.blur(); applyView(v); });
    viewList.appendChild(b);
  }
  const info = document.createElement('div');
  info.className = 'group-label';
  info.textContent = `${manifest.fragments.length} fragments / ${manifest.literals} literals / ${manifest.lines} lines / ${(manifest.hotkeys || []).length} hotkeys`;
  viewList.appendChild(info);
}

function wireControls() {
  const bind = (id, fn) => document.getElementById(id).addEventListener('change', fn);
  bind('chk-play', (e) => { model.paused = !e.target.checked; });
  bind('chk-mouse', (e) => { model.uiMouse = e.target.checked; });
  bind('chk-demo', (e) => { model.demo = e.target.checked; });
  bind('chk-hudhide', (e) => { model.hudHidden = e.target.checked; });

  document.getElementById('btn-fit').addEventListener('click', () => fitStage('fit'));
  document.getElementById('btn-100').addEventListener('click', () => fitStage('100'));
  document.getElementById('btn-200').addEventListener('click', () => fitStage('200'));
  document.getElementById('btn-dev').addEventListener('click', () => {
    devEl.classList.toggle('open');
    document.getElementById('btn-dev').classList.toggle('on', devEl.classList.contains('open'));
  });
  document.getElementById('btn-reload').addEventListener('click', () => reload());

  for (const b of document.querySelectorAll('#devtabs button')) {
    b.addEventListener('click', () => {
      devTab = b.dataset.tab;
      for (const o of document.querySelectorAll('#devtabs button')) o.classList.toggle('active', o === b);
      paintDev();
    });
  }

  window.addEventListener('resize', () => {
    if (document.getElementById('btn-fit').classList.contains('on')) fitStage('fit');
  });
}

async function reload() {
  manifest = await (await fetch(`generated/manifest.json?t=${Date.now()}`)).json();
  scriptText = await loadScript();
  scriptStamp = String(scriptText.length);
  // Every registration lives on $, and each fragment returns early if its own name is already
  // there -- `if ($.MvmWorkspace) return;`. So a rebuild has to clear them, or the second load is a
  // no-op and the page shows the OLD code while the file on disk says otherwise.
  for (const k of Object.keys(window.$)) if (/^Mvm/.test(k)) delete window.$[k];
  buildSurface();
  syncSidebar(true);
}

// The page polls the generated file, so editing it in any editor shows the result within a second or
// so. That is the fast loop this harness exists for -- the alternative is a rebuild and a relaunch
// for every tweak, because each in-game HUD caches its root panel when it first builds and an edited
// script cannot be hot-loaded over a running one.
function watchScript() {
  setInterval(async () => {
    try {
      const responses = await Promise.all(['workspace.js', 'manifest.json'].map((file) =>
        fetch(`generated/${file}?t=${Date.now()}`, { method: 'HEAD' })));
      const tag = responses.map((res) => res.headers.get('etag') || res.headers.get('last-modified') || res.headers.get('content-length')).join('|');
      if (tag && tag !== watchScript.tag) {
        if (watchScript.tag !== undefined) await reload();
        watchScript.tag = tag;
      }
    } catch { /* the server went away; nothing to do */ }
  }, 1500);
}

async function boot() {
  const res = await fetch(`generated/manifest.json?t=${Date.now()}`);
  if (!res.ok) {
    statusEl.innerHTML = '<span class="warn">generated/manifest.json is missing &mdash; ' +
      'run <b>node UiMockupDeadlock/extract-deadlock-ui.mjs</b></span>';
    return;
  }
  manifest = await res.json();

  model = window.MvmDeadlockState.initial();
  router = new window.MvmDeadlockCommands.Router(model);

  // THE GAME'S OWN STATE, if it has been captured (import-state.mjs). A comparison against the
  // hand-written stand-in measures the stand-in: the first geometry diff diverged at panel 58
  // because the stand-in had three markers and the game had none. The snapshot SEEDS the model
  // rather than replacing the projection, so every control stays live and the views still work --
  // the numbers are the game's, the plumbing is unchanged. `?live=0` ignores it.
  const params0 = new URLSearchParams(location.search);
  let snapshot = null;
  if (params0.get('live') !== '0') {
    try {
      const res = await fetch(`generated/state-snapshot.json?t=${Date.now()}`);
      if (res.ok) snapshot = (await res.json()).state;
    } catch { /* no snapshot; the stand-in it is */ }
  }
  if (snapshot) seedModel(model, snapshot);

  buildSidebar();
  wireControls();

  // A real capture goes behind the panels if one has been dropped in.
  const probe = new Image();
  probe.onload = () => {
    backdrop.classList.add('has-image');
    backdrop.style.setProperty('--shot', 'url("assets/backdrop.jpg")');
  };
  probe.src = 'assets/backdrop.jpg';

  scriptText = await loadScript();
  buildSurface();

  // ?view=<id> picks the view, so a headless run can drive the page by URL rather than by faking
  // clicks. ?report=1 then stamps the result into the DOM, which is the only channel a --dump-dom
  // run has back out of the browser.
  const params = new URLSearchParams(location.search);
  const wanted = manifest.views.find((v) => v.id === params.get('view'));
  // Otherwise: CLOSED, the way the game comes up after a replay loads. A captured snapshot seeds
  // the numbers (markers, slider values, the loop) but never what is open -- it is a moment
  // mid-session, and the page starts where a fresh launch does.
  applyView(wanted || manifest.views.find((v) => v.id === 'closed') || manifest.views[0]);

  // ?bare=1 strips the harness's own chrome so a 1920x1080 window photographs EXACTLY the game
  // frame and nothing else. Without it the sidebar takes 272px of the shot and the right-hand end
  // of the screen -- the inspector column, and the bar's Loop button -- falls off the edge, which
  // makes a pixel comparison against a game capture impossible.
  if (params.has('bare') || params.has('report')) {
    document.getElementById('sidebar').hidden = true;
    statusEl.hidden = true;
    document.body.style.background = '#000';
  }

  fitStage(params.has('report') || params.has('bare') ? '100' : 'fit');
  watchScript();
  schedule();

  // Handed to the headless scripts (build-check, capture, compare-geometry) so they drive the page
  // through the same entry points a person does rather than reaching into its internals.
  window.MvmDeadlockHarness = {
    views: () => manifest.views,
    setView: (id) => {
      const v = manifest.views.find((x) => x.id === id);
      if (!v) throw new Error(`no view ${id}`);
      applyView(v);
      return v;
    },
    current: () => viewFor(model).id,
    // One frame, now, without rescheduling the loop: a headless run that is not producing frames
    // never fires requestAnimationFrame, so a driven command would change state nothing rendered.
    tick: (n) => { for (let i = 0; i < (n || 1); i++) frame(performance.now(), true); },
    act,
    run: (cmd) => router.run(cmd),
    hotkeys: () => (manifest.hotkeys || []).slice(),
    model: () => model,
    state: () => model.lastState,
    geometry: geometryDump,
    commands: () => router.log.slice(),
    gaps: () => router.gaps.slice(),
    errors: () => ({ build: buildError, render: renderErrors.slice() }),
    styleWrites: () => window.PanoramaShim.styleWrites().count,
    unknownStyle: () => ({ ...window.PanoramaShim.unknownStyleProps() }),
    panelCount: () => window.PanoramaShim.allPanels().length,
    fitStage,
  };

  // ?cmd=a;b sends console commands and ?act=A|B presses sidebar buttons after the view is up, so
  // a headless capture can photograph a popup or a reported state.
  if (params.get('cmd')) for (const c of params.get('cmd').split(';')) if (c.trim()) router.run(c.trim());
  if (params.get('act')) setTimeout(() => { for (const a of params.get('act').split('|')) if (a.trim()) act(a.trim()); }, 200);

  if (params.has('report')) stampReport();
}

// Copies what the game reported into the model's own fields. Only fields the model has a home for:
// the layout is re-solved from inspectorW / dock / screen rather than copied, so a view switch in
// the browser still re-lays-out the way the game would.
function seedModel(m, s) {
  const has = (k) => s[k] !== undefined && s[k] !== null;
  // Not `open`, not `blackout`, not the dock tab: those say what the game was showing at the moment
  // of capture, and the page boots closed on the Regular Timeline like a fresh launch.
  if (has('workspace')) m.workspace = s.workspace;
  if (s.dock && s.dock.tab === 'native' && s.dock.editorH) m.measuredNativeH = s.dock.editorH;
  if (has('demo')) m.demo = !!s.demo;
  if (has('paused')) m.paused = !!s.paused;
  if (has('uiMouse')) m.uiMouse = !!s.uiMouse;
  if (has('level')) m.level = s.level;
  if (has('tick')) { m.tick = s.tick; m.playhead = has('playhead') ? s.playhead : s.tick; }
  if (has('speed')) m.speed = s.speed;
  if (has('pathSmooth')) m.pathSmooth = !!s.pathSmooth;
  if (Array.isArray(s.markerList)) {
    m.markerList = s.markerList.map((x) => ({ tick: x.tick, pos: x.pos }));
    m.selected = has('selected') ? s.selected : m.markerList.findIndex((x, i) => s.markerList[i].selected);
  }
  if (has('playing')) m.playing = !!s.playing;
  if (has('live')) m.pathOwnsView = !s.live;
  if (has('hudHidden')) m.hudHidden = !!s.hudHidden;
  if (has('previewScaled')) m.previewScaled = !!s.previewScaled;
  if (has('viewportW') && has('viewportH') && s.viewportH > 0) {
    m.viewportW = s.viewportW; m.viewportH = s.viewportH; m.worldAspect = s.viewportW / s.viewportH;
  }
  if (has('loopOn')) m.loopOn = !!s.loopOn;
  if (has('loopIn')) m.loopIn = s.loopIn;
  if (has('loopOut')) m.loopOut = s.loopOut;
  if (s.layout) {
    if (s.layout.inspectorW > 0) m.inspectorW = s.layout.inspectorW;
    if (s.layout.screenW > 0) { m.screenW = s.layout.screenW; m.screenH = s.layout.screenH; }
  }
  if (s.graph) {
    if (s.graph.drive !== undefined) m.graphDrive = !!s.graph.drive;
  }
  if (s.director) {
    if (s.director.freecam !== undefined) m.freecam = !!s.director.freecam;
    if (typeof s.director.speed === 'number') m.camSpeed = s.director.speed;
    for (const k of ['ox', 'oy', 'oz', 'pitch', 'yaw', 'roll']) if (typeof s.director[k] === 'number') m.camPose[k] = s.director[k];
  }
  if (s.render) {
    if (s.render.state) m.renderState = s.render.state;
    if (s.render.fps) m.renderFps = s.render.fps;
    if (s.render.frames !== undefined) m.renderFrames = s.render.frames;
    if (s.render.dropped !== undefined) m.renderDropped = s.render.dropped;
    if (s.render.take !== undefined) m.renderTake = s.render.take;
    if (s.render.startTick !== undefined) m.renderStart = s.render.startTick;
    if (s.render.endTick !== undefined) m.renderEnd = s.render.endTick;
    if (s.render.dir) m.renderDir = s.render.dir;
  }
  m.seededFromGame = true;
}

// Let the panels settle, then write everything a headless run needs into one element. Several frames
// rather than one, because the bar is measured, reported, and the dock resized over two or three --
// a single-frame report would photograph the transient and call it the layout.
function stampReport() {
  const pre = document.createElement('pre');
  pre.id = 'mvm-report';
  pre.style.display = 'none';
  document.body.appendChild(pre);

  setTimeout(() => {
    const before = window.PanoramaShim.styleWrites().count;
    setTimeout(() => {
      const after = window.PanoramaShim.styleWrites().count;
      pre.textContent = JSON.stringify({
        id: view ? view.id : null,
        errors: { build: buildError, render: renderErrors.slice() },
        gaps: router.gaps.slice(),
        unknownStyle: { ...window.PanoramaShim.unknownStyleProps() },
        panels: window.PanoramaShim.allPanels().length,
        // A STEADY-STATE FRAME SHOULD WRITE NOTHING. In game a style write re-does the panel's
        // geometry and a 3D transform allocates a composited layer with its own vertex buffer,
        // which Panorama indexes with an unsigned short -- so a panel restyled every frame runs
        // through 65535 entries and the game dies. This number is how that shows up first.
        styleWritesPerSecond: after - before,
        geometry: geometryDump(),
        state: model.lastState,
      });
      document.title = 'mvm-report-ready';
    }, 500);
  }, 900);
}

boot();
