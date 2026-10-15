// Does a Panorama surface actually BUILD and RENDER, or does it die halfway through?
//
//   node UiMockup/build-check.mjs confighud cameraeditor
//
// It runs extract-ui.mjs, then executes the surface against a throwaway Panorama stub and calls
// its render() three times against the mockup's own state feed. This catches the failure mode a
// syntax check cannot: a ReferenceError partway through the build leaves the script registered
// on nothing and the panel half-drawn, which in game reads as "the UI is just ugly" rather than
// as an error. `new Function(src)` only proves the file parses.
//
// The stub is deliberately dumb -- every panel property reads back as a benign default and every
// method is a no-op -- so it answers one question only: did the script run to the end.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';

execFileSync('node', ['UiMockup/extract-ui.mjs'], { stdio: 'pipe' });

// DEFAULT TO EVERY SURFACE, not just confighud. The old default checked one panel, so a whole
// session's worth of Camera Editor breakage passed a green `build-check` and was only found by
// launching the game: a shared fragment (WorkspaceShellJs.h / WorkspaceChromeJs.h) is spliced
// into SEVERAL scripts, so checking one of them proves nothing about the others. Naming
// surfaces explicitly still works for a quick single-panel loop.
const ALL = ['confighud', 'cameraeditor', 'cameratimeline', 'grapheditor', 'demobar', 'markerhud', 'moviehud', 'aimcrosshair', 'fxdebug', 'eastereggs', 'filmmakergui'];
const names = process.argv.slice(2);
if (!names.length) names.push(...ALL.filter((n) => fs.existsSync('UiMockup/generated/' + n + '.js')));

let failed = 0;
for (const name of names) {
  const src = fs.readFileSync('UiMockup/generated/' + name + '.js', 'utf8');
  const msgs = [];

  let stateJson = null;   // filled in once the state feed is loaded, below
  function panel() {
    return {
      // Panorama VALIDATES style values and throws on a bad one, which aborts the whole panel
      // script and leaves a half-built panel in game -- the failure mode that reads as "ugly UI"
      // rather than as an error. The stub used to accept anything, so it happily passed a build
      // that could not survive one frame in CS2. These are the values it actually rejects.
      style: new Proxy({}, {
        get: () => '',
        set: (target, prop, value) => {
          const v = String(value);
          // style.X = '' throws in Panorama; clear a colour with rgba(0,0,0,0).
          if (v === '') throw new Error("Invalid empty value for property '" + String(prop) + "'");
          // ...and so does undefined/null, which is what a MISSING THEME TOKEN evaluates to.
          // `String(undefined)` is the word "undefined", so the empty check above sails past it
          // and the stub happily accepted a write that aborts the real panel's whole build.
          if (value === undefined || value === null)
            throw new Error("Style '" + String(prop) + "' was set to " + String(value)
              + " -- a missing theme token? Panorama throws and aborts the build.");
          // font-weight takes NAMED weights only; a CSS number throws.
          if (prop === 'fontWeight' && /^[0-9]+$/.test(v))
            throw new Error("Invalid value for property 'font-weight': " + v);
          // #rrggbbaa or #rrggbb only -- a 10-digit string (accent + alpha appended to an
          // already-8-digit token) is the easy way to build one by accident.
          if (/color$/i.test(String(prop)) && v.startsWith('#') && ![4, 7, 9].includes(v.length))
            throw new Error("Invalid colour for property '" + String(prop) + "': " + v);
          target[prop] = v;
          return true;
        }
      }),
      id: '', text: '', visible: true, hittest: true, hittestchildren: true, enabled: true,
      value: 0, checked: false, mousedown: false, busy: false,
      actuallayoutwidth: 442, actuallayoutheight: 76, actualuiscale_x: 1, actualuiscale_y: 1,
      actualxoffset: 0, actualyoffset: 0, scrolloffset_y: 0,
      children: [],
      IsValid: () => true,
      GetParent: () => null,
      GetChildCount() { return this.children.length; },
      GetChild(i) { return this.children[i]; },
      RemoveAndDeleteChildren() { this.children = []; },
      DeleteAsync() {}, SetPanelEvent() {}, AddClass() {}, RemoveClass() {}, SetImage() {},
      SetScrollParentToFitWhenFocused() {}, ScrollParentToMakePanelFit() {},
      GetPositionWithinWindow: () => ({ x: 0, y: 0 }),
      FindChildTraverse: () => null, FindChild: () => null,
      // EVERY panel serves the state feed, not just the context panel. Each surface reads
      // its state off its OWN root panel (root.GetAttributeString('state','')), so patching
      // ctx alone left `st` null and every render() bailed on its first line -- which is how
      // `render=ok` got reported for a panel whose layout never ran once.
      SetAttributeString() {}, GetAttributeString: (k, d) => (k === 'state' && stateJson !== null ? stateJson : d),
      SetAttributeInt() {}, GetAttributeInt: (k, d) => d,
      BLoadLayoutSnippet() {}
    };
  }

  // THE CONTEXT AND EVERY *Root PANEL REPORT A REAL SCREEN. Every other panel keeps the small
  // default, which is fine for a card. The root is not: a workspace's layout starts with
  // `if (!finitePositive(areaW) || !finitePositive(areaH)) { hide(); return; }`, and with a
  // 442x76 root that guard fired and the whole letterbox/preview/dock placement path was never
  // executed. `render=ok` therefore meant "render returned", not "layout ran" -- which is how a
  // ReferenceError inside the layout block (natH) survived a green check and reached the game.
  const SCREEN_W = 1920, SCREEN_H = 1080;
  const ctx = panel();
  ctx.actuallayoutwidth = SCREEN_W;
  ctx.actuallayoutheight = SCREEN_H;
  const api = {
    GetContextPanel: () => ctx,
    CreatePanel(type, parent, id) {
      const p = panel();
      p.id = id || '';
      if (/Root$/.test(p.id)) {
        p.actuallayoutwidth = SCREEN_W;
        p.actuallayoutheight = SCREEN_H;
      }
      p.GetParent = () => parent || null;
      if (parent && parent.children) parent.children.push(p);
      return p;
    },
    Msg: (s) => msgs.push(String(s)),
    Schedule: () => {},
    RegisterEventHandler: () => {},
    DispatchEvent: () => {},
    Localize: (s) => s,
    GetContextPanelSafe: () => ctx
  };
  const dollarFn = () => null;
  Object.assign(dollarFn, api);
  globalThis.$ = dollarFn;
  globalThis.GameInterfaceAPI = { ConsoleCommand: () => {}, GetSettingString: () => '0', GetSettingBool: () => false };
  globalThis.UiToolkitAPI = { AddDenyInputFlagsToGame: () => 1, ReleaseDenyInputFlagsToGame: () => {} };
  globalThis.GameStateAPI = {};

  try {
    (0, eval)(src);
  } catch (e) {
    msgs.push('THREW OUTSIDE THE SCRIPT: ' + e.message + ' | ' + String(e.stack || '').slice(0, 500));
  }

  // Building is only half of it: every panel's live values are written by render(), and a render
  // that throws leaves the panel built but frozen. Feed it the mockup's own state feed.
  const APIS = { confighud: 'ConfigHud', cameraeditor: 'CamEditor', cameratimeline: 'CameraTimeline',
                 grapheditor: 'GraphExp', markerhud: 'MarkerHud', moviehud: 'MovieHud' };
  let rendered = 'skipped';
  const apiName = APIS[name];
  if (apiName && dollarFn[apiName] && dollarFn[apiName].render) {
    try {
      globalThis.window = globalThis;
      // state.js reads the scene-key model at load time, so its shims go in first. Without them
      // every render() died on `MvmSceneKeys.baseline()` and the check reported that one throw
      // instead of whatever the surface actually does.
      (0, eval)(fs.readFileSync('UiMockup/shim/scene-catalog.js', 'utf8'));
      (0, eval)(fs.readFileSync('UiMockup/shim/scene-keys.js', 'utf8'));
      (0, eval)(fs.readFileSync('UiMockup/shim/state.js', 'utf8'));
      const state = globalThis.MvmState.make();
      stateJson = JSON.stringify(state);
      for (let i = 0; i < 3; i++) dollarFn[apiName].render();
      rendered = 'ok';
    } catch (e) {
      msgs.push('RENDER THREW: ' + e.message + ' | ' + String(e.stack || '').slice(0, 400));
      rendered = 'threw';
    }
  }

  const bad = msgs.filter((m) => /error|failed|not defined|not a function|undefined|threw/i.test(m));
  const built = ctx.children.length;
  if (bad.length) {
    failed++;
    console.log(name + ': BUILD ERRORS');
    for (const b of bad) console.log('   ' + b.trim());
  } else {
    console.log(name + ': builds clean (' + msgs.length + ' msg lines, ' + built + ' root panels, render=' + rendered + ')');
  }
}
process.exit(failed ? 1 : 0);
