import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';

const root = path.resolve(import.meta.dirname, '..', '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

const launcher = read('SourceMovieMakerLauncher/src/App.tsx');
const launcherMain = read('SourceMovieMakerLauncher/electron/main.cjs');
const launcherTypes = read('SourceMovieMakerLauncher/src/vite-env.d.ts');
const configCamera = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudCameraTabJs.h');
const videoJs = read('AfxHookSource2/Filmmaker/Panorama/VideoSettingsResolutionJs.h');
const videoCpp = read('AfxHookSource2/Filmmaker/Panorama/VideoSettingsResolution.cpp');
const uiLiveReload = read('AfxHookSource2/Filmmaker/Panorama/UiLiveReload.cpp');
const filmmaker = read('AfxHookSource2/Filmmaker/Filmmaker.cpp');
const cmake = read('AfxHookSource2/CMakeLists.txt');
const embeddedVideoJs = [...videoJs.matchAll(/R"MVMRES\(([\s\S]*?)\)MVMRES"/g)]
  .map((match) => match[1])
  .join('');

test('resolution controls no longer live in launcher or Filmmaker Config', () => {
  assert.doesNotMatch(launcher, /gfxWidth|gfxHeight|>WIDTH<|>HEIGHT</);
  assert.doesNotMatch(launcherMain, /gfxWidth|gfxHeight|\s-w\s|\s-h\s/);
  assert.doesNotMatch(launcherTypes, /gfxWidth|gfxHeight/);
  assert.doesNotMatch(configCamera, /WINDOW SIZE|ConfigWindowWidth|mirv_filmmaker window size/);
});

test('CS2 Video Resolution dropdown stays native and custom editing has a stable action', () => {
  assert.match(videoJs, /find\('ResolutionEnum'\)/);
  assert.match(videoJs, /MvmResolutionActionButton/);
  assert.match(videoJs, /SET CUSTOM…/);
  // Buttons layer the native PopupButton class (hover wash + sounds) over explicit styling;
  // the popup card must NOT depend on popups_shared.css classes — they are absent in the
  // settings context and render as an unstyled scatter.
  assert.match(videoJs, /AddClass\('PopupButton'\)/);
  assert.doesNotMatch(videoJs, /AddClass\('PopupPanel'\)/);
  assert.doesNotMatch(videoJs, /AddClass\('PopupTitle'\)/);
  assert.doesNotMatch(videoJs, /resolution\.AddOption\(/);
  // The native handler is a Panorama EVENT. As a JS function it is undefined in the settings
  // context, so calling it throws and the surrounding catch hides that the engine never heard.
  assert.match(videoJs, /\$\.DispatchEvent\('CSGOResolutionSelectionChanged'\)/);
  assert.doesNotMatch(videoJs, /^[^/\n]*[;{]\s*CSGO(Resolution|AspectRatio)SelectionChanged\(\)/m);
});

test('custom action leaves the native selection alone and remains usable after save or cancel', () => {
  class Panel {
    constructor(type, parent, id = '') {
      this.paneltype = type;
      this.parent = parent;
      this.id = id;
      this.children = [];
      this.options = [];
      this.events = {};
      this.style = {};
      if (parent) parent.children.push(this);
    }
    Children() { return this.children; }
    GetParent() { return this.parent; }
    FindChildTraverse(id) {
      if (this.id === id) return this;
      for (const child of this.children) {
        const found = child.FindChildTraverse(id);
        if (found) return found;
      }
      return null;
    }
    SetAttributeString(name, value) { this[name] = value; }
    SetPanelEvent(name, action) { this.events[name] = action; }
    AddClass(name) { (this.classes ||= new Set()).add(name); }
    RemoveClass(name) { if (this.classes) this.classes.delete(name); }
    AddOption() { throw new Error('native ResolutionEnum must not receive custom options'); }
    RemoveAllOptions() { this.options = []; }
    RefreshDisplay() { this.refreshed = true; }
    GetSelected() { return this.selected || null; }
    MoveChildAfter() {}
    DeleteAsync() {
      if (this.parent) this.parent.children = this.parent.children.filter((child) => child !== this);
    }
    SetFocus() { this.focused = true; }
  }

  const rootPanel = new Panel('Panel', null, 'MainMenu');
  const list = new Panel('Panel', rootPanel, 'VideoSettingsList');
  const row = new Panel('Panel', list, 'ResolutionRow');
  const resolution = new Panel('CSGOSettingsEnumDropDown', row, 'ResolutionEnum');
  const nativeResolution = new Panel('Label', resolution, 'resolution0');
  resolution.selected = nativeResolution;
  const commands = [];
  let nativeChanges = 0;
  const panorama = {
    GetContextPanel: () => rootPanel,
    CreatePanel: (type, parent, id) => new Panel(type, parent, id),
    Schedule: () => {},
    DispatchEvent: (name) => { if (name === 'CSGOResolutionSelectionChanged') nativeChanges += 1; },
  };
  assert.ok(embeddedVideoJs, 'embedded Panorama script was not found');
  // No CSGO*SelectionChanged globals: the real settings context does not define them either.
  vm.runInNewContext(embeddedVideoJs, {
    $: panorama,
    GameInterfaceAPI: { ConsoleCommand: (command) => commands.push(command) },
  });

  const action = rootPanel.FindChildTraverse('MvmResolutionActionButton');
  assert.ok(action, 'stable Custom Res action was not created');
  assert.equal(resolution.selected, nativeResolution);
  action.events.onactivate();
  assert.ok(rootPanel.FindChildTraverse('MvmCustomResolutionPopup'));
  assert.equal(resolution.selected, nativeResolution);

  rootPanel.FindChildTraverse('MvmCustomResolutionCancel').events.onactivate();
  assert.equal(rootPanel.FindChildTraverse('MvmCustomResolutionPopup'), null);
  assert.equal(resolution.selected, nativeResolution);

  action.events.onactivate();
  rootPanel.FindChildTraverse('MvmCustomWidth').text = '1556';
  rootPanel.FindChildTraverse('MvmCustomHeight').text = '1088';
  // A size that is not saved yet: one button, captioned as a save, and it saves.
  assert.equal(rootPanel.FindChildTraverse('MvmCustomResolutionSaveApply'), null);
  const confirm = rootPanel.FindChildTraverse('MvmCustomResolutionApply');
  assert.equal(confirm.__label.text, 'Save & Apply');
  confirm.events.onactivate();
  assert.ok(commands.includes('mirv_filmmaker window size 1556 1088'));
  assert.ok(commands.includes('mirv_filmmaker settings ui video.customResolutions 1556x1088'));
  assert.equal(rootPanel.FindChildTraverse('MvmCustomResolutionPopup'), null);
  assert.equal(resolution.selected, nativeResolution);

  // The same size, now saved: the caption drops to Apply and nothing is written again.
  action.events.onactivate();
  assert.ok(rootPanel.FindChildTraverse('MvmSavedResolution0'));
  rootPanel.FindChildTraverse('MvmCustomWidth').text = '1556';
  rootPanel.FindChildTraverse('MvmCustomHeight').text = '1088';
  rootPanel.FindChildTraverse('MvmCustomWidth').events.ontextchanged();
  const savedConfirm = rootPanel.FindChildTraverse('MvmCustomResolutionApply');
  assert.equal(savedConfirm.__label.text, 'Apply');
  const writes = commands.filter((c) => c.includes('video.customResolutions')).length;
  savedConfirm.events.onactivate();
  assert.equal(commands.filter((c) => c.includes('video.customResolutions')).length, writes);
  action.events.onactivate();
  assert.equal(resolution.selected, nativeResolution);

  resolution.events.oninputsubmit();
  assert.equal(nativeChanges, 1);
});

test('custom popup supports exact and nearest-aspect fitting and saved presets', () => {
  assert.match(videoJs, /FIT TO CUSTOM/);
  assert.match(videoJs, /FIT TO NEAREST ASPECT/);
  assert.match(videoJs, /MvmCustomWidth/);
  assert.match(videoJs, /MvmCustomHeight/);
  assert.match(videoJs, /MvmSavedResolution/);
  assert.match(videoJs, /Save & Apply/);
  assert.match(videoJs, /video\.customResolutions/);
  assert.match(videoJs, /mirv_filmmaker window size/);
  assert.match(videoJs, /Widescreen HD/);
  assert.match(videoJs, /Anamorphic scope/);
});

test('custom Video settings integration has an independent main-thread module', () => {
  assert.match(videoCpp, /FilmmakerSettings_UiString\("video\.customResolutions"/);
  assert.match(videoCpp, /kVideoSettingsResolutionJs/);
  assert.match(filmmaker, /VideoSettingsResolution_RunMainThreadFrame\(\)/);
  assert.match(cmake, /Panorama\/VideoSettingsResolution\.cpp/);
  assert.match(cmake, /Panorama\/VideoSettingsResolution\.h/);
});

test('livejs prefers the main-menu context so native Settings pages can be edited live', () => {
  assert.match(uiLiveReload, /AfxHookSource2_GetPanoramaMainMenuPanel\(\)/);
  assert.match(uiLiveReload, /if \(!context\)[\s\S]*AfxHookSource2_GetPanoramaHudPanel\(\)/);
});
