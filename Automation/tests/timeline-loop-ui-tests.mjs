// Execute the real embedded UI, not a copy of its visibility expressions.
import assert from 'node:assert/strict';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';
import { flatten } from '../../UiMockup/lib/panorama-headers.mjs';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const pano = path.join(repo, 'AfxHookSource2/Filmmaker/Panorama');

function createUi() {
  const panels = [], commands = [], messages = [];
  const state = {
    open: false, hosted: true, graphExp: false, configOpen: false,
    cursor: true, hudView: 'full', hudScale: false,
    loopOn: true, loopReady: true, loopIn: 200, loopOut: 400,
    demoStartTick: 0, demoEndTick: 1000, tickMin: 100, tickMax: 500,
    tick: 300, time: 4.6875, playing: false, scrubbing: false,
    renderRangeMode: 4, renderInTick: 100, renderOutTickExclusive: 501,
    renderRangeActive: true, renderOpen: false,
    count: 0, selected: -1, markers: [], shakes: [], timescale: 1,
    interp: 'Linear', timing: 'Live', speedMode: 'Manual',
    mx: 0, my: 0, lmb: false, rmb: false,
    enabled: true, channels: [], viewT0: 100, viewT1: 500,
    playhead: 300, rev: 1, selCount: 0, selCh: -1,
  };
  function makePanel(type, parent, id = '') {
    const panel = {
      type, id, parent, children: [], events: {}, style: {},
      text: '', visible: true, hittest: true, hittestchildren: true,
      value: 0, mousedown: false,
      actuallayoutwidth: id.endsWith('Root') ? 1920 : 1000,
      actuallayoutheight: id.endsWith('Root') ? 1080 : 40,
      actualuiscale_x: 1, actualuiscale_y: 1, actualxoffset: 0, actualyoffset: 0,
      IsValid() { return true; },
      GetParent() { return this.parent; },
      GetChildCount() { return this.children.length; },
      GetChild(i) { return this.children[i]; },
      FindChildTraverse(wanted) {
        for (const child of this.children) {
          if (child.id === wanted) return child;
          const found = child.FindChildTraverse(wanted);
          if (found) return found;
        }
        return null;
      },
      SetPanelEvent(name, handler) { this.events[name] = handler; },
      GetAttributeString(key, fallback) { return key === 'state' ? JSON.stringify(state) : fallback; },
      GetAttributeInt(key, fallback) { return fallback; },
      GetPositionWithinWindow() { return { x: 0, y: 900 }; },
      RemoveAndDeleteChildren() { this.children = []; },
      DeleteAsync() { if (this.parent) this.parent.children = this.parent.children.filter(p => p !== this); },
      MoveChildAfter(child, after) {
        this.children = this.children.filter(p => p !== child);
        this.children.splice(this.children.indexOf(after) + 1, 0, child);
      },
      attrs: {},
      AddClass() {}, RemoveClass() {}, SetAttributeString(key, value) { this.attrs[key] = value; }, SetAttributeInt() {},
      SetImage() {}, SetFocus() {},
    };
    panels.push(panel);
    if (parent) parent.children.push(panel);
    return panel;
  }
  const ctx = makePanel('Panel', null, 'HudRoot');
  const native = makePanel('Panel', ctx, 'CSGOHudDemoController');
  const nativeRoot = makePanel('Panel', native, 'Root');
  const contents = makePanel('Panel', nativeRoot, 'Contents');
  const sliderRow = makePanel('Panel', contents, 'SliderRow');
  makePanel('Panel', sliderRow, 'RoundMarkers');
  makePanel('Panel', sliderRow, 'HighlightIcons');
  // CS2's ControlRow as of the 2026-10-14 build (panorama/layout/hud/huddemocontroller.xml).
  const controls = makePanel('Panel', contents, 'ControlRow');
  makePanel('Panel', controls, 'PlayButton');
  const speed = makePanel('Panel', controls, 'SpeedControls');
  makePanel('Panel', speed, 'SpeedSliderContainer');
  makePanel('Label', speed, 'TimeScale');
  makePanel('Panel', controls, 'TimeLabel');
  const timeControls = makePanel('Panel', controls, 'TimeControls');
  makePanel('Label', timeControls, 'TimeStepBackward');
  makePanel('Label', timeControls, 'TimeStepForward');
  makePanel('Panel', controls, 'RoundControls');
  makePanel('Panel', controls, 'PlayerControls');
  makePanel('Panel', controls, 'NextEventButton');
  makePanel('Panel', controls, 'ControlRowSpacer');
  const cameraButton = makePanel('Panel', controls, 'CameraButton');
  makePanel('Label', cameraButton, 'CameraModeLabel').text = 'First Person';
  makePanel('Panel', controls, 'HighlightsButton');
  makePanel('Panel', controls, 'SettingsButton');
  makePanel('Label', controls, 'EndPlayback');
  // CS2's spectator overview map (layout/hud/hud.xml: CSGOMapOverview id="MapOverview").
  makePanel('Panel', ctx, 'MapOverview');
  const dialogVars = {};
  native.SetDialogVariableInt = (name, value) => { dialogVars[name] = value; };
  const $ = selector => ctx.FindChildTraverse(selector.replace(/^#/, ''));
  Object.assign($, {
    GetContextPanel: () => ctx, CreatePanel: makePanel,
    Msg: msg => messages.push(String(msg)), Schedule() {},
    RegisterEventHandler() {}, DispatchEvent() {}, Localize: text => text,
  });
  const sandbox = vm.createContext({
    $, GameInterfaceAPI: { ConsoleCommand: cmd => commands.push(cmd) },
  });
  function checkErrors() {
    assert.deepEqual(messages.filter(m => /error|failed|not defined|not a function/i.test(m)), []);
  }
  function load(file) {
    vm.runInContext(flatten(pano, file), sandbox, { filename: file, timeout: 2000 });
    checkErrors();
  }
  load('CameraTimelineJs.h');
  assert.equal(typeof $.CamTimeline?.render, 'function');
  function render(patch = {}) {
    Object.assign(state, patch);
    $.CamTimeline.render();
    checkErrors();
  }
  return { $, state, panels, commands, contents, controls, dialogVars, render, load, checkErrors, makePanel, ctx,
    find: id => ctx.FindChildTraverse(id) };
}

test('regular timeline keeps its Loop toggle and flags in plain, editor, and Config views', () => {
  const ui = createUi();
  for (const view of [
    { hosted: false, configOpen: false },
    { hosted: true, configOpen: false },
    { hosted: false, configOpen: true },
  ]) {
    ui.render(view);
    assert.equal(ui.contents.visible, true);
    assert.equal(ui.find('LoopBtn').visible, true);
    assert.equal(ui.find('CamTimelineRangeRoot').visible, true);
    ui.commands.length = 0;
    ui.find('LoopBtn').events.onactivate();
    assert.deepEqual(ui.commands, ['mirv_filmmaker loop toggle quiet']);
  }
  ui.render({ loopOn: false });
  assert.equal(ui.find('LoopBtn').visible, true);
  assert.equal(ui.find('CamTimelineRangeRoot').visible, false);
  assert.deepEqual([ui.state.loopIn, ui.state.loopOut], [200, 400]);
});

test('camera and graph views hide loop UI without resetting the shared loop', () => {
  const ui = createUi();
  ui.commands.length = 0;
  for (const hosted of [true, false]) {
    for (const mode of [{ open: true, graphExp: false }, { open: false, graphExp: true }]) {
      ui.render({ hosted, ...mode });
      assert.equal(ui.find('LoopBtn').visible, false);
      assert.equal(ui.find('CamTimelineRangeRoot').visible, false);
      assert.equal(ui.contents.visible, false);
      assert.deepEqual([ui.state.loopOn, ui.state.loopIn, ui.state.loopOut], [true, 200, 400]);
    }
  }
  ui.render({ hosted: true, open: false, graphExp: false });
  assert.equal(ui.find('LoopBtn').visible, true);
  assert.equal(ui.find('CamTimelineRangeRoot').visible, true);
  assert.deepEqual(ui.commands, [], 'switching views must not mutate loop or playback state');
  assert.equal(ui.panels.filter(p => p.id === 'LoopBtn').length, 1,
    'the demo-bar LOOP icon must be the only loop control, including hidden panels');
  assert.equal(ui.panels.filter(p => /loop/i.test(p.text)).length, 0,
    'LOOP is an icon now, not a text label');
});

test('our bar buttons sit in one group at the right end of the native row', () => {
  const ui = createUi();
  ui.render({ hosted: false, configOpen: false, open: false, graphExp: false });
  const group = ui.find('MvmBarButtons');
  assert.ok(group, 'MvmBarButtons is created');
  assert.equal(group.parent, ui.controls);
  const rowIds = ui.controls.children.map(p => p.id);
  // The speed dial is centred between two flexible spacers; our group is last.
  assert.deepEqual(rowIds, ['PlayButton', 'TimeLabel', 'TimeControls', 'RoundControls',
    'MvmSpeedSpacer', 'SpeedControls', 'ControlRowSpacer', 'PlayerControls', 'NextEventButton',
    'CameraButton', 'HighlightsButton', 'SettingsButton', 'EndPlayback', 'MvmBarButtons']);
  ui.render({});
  assert.deepEqual(ui.controls.children.map(p => p.id), rowIds, 're-rendering leaves the order alone');
  assert.deepEqual(group.children.filter(p => p.visible).map(p => p.id),
    ['LoopBtn', 'CamEditorBtn', 'ConfigBtn']);
  for (const id of ['EndPlayback', 'SettingsButton', 'HighlightsButton']) {
    assert.equal(ui.find(id).visible, false, `${id} is hidden`);
  }
  for (const id of ['PlayerControls', 'NextEventButton', 'CameraButton']) {
    assert.equal(ui.find(id).visible, true, `${id} stays`);
  }
});

test('the spectator overview map never shows during a demo', () => {
  const ui = createUi();
  ui.render({});
  const overview = ui.find('MapOverview');
  assert.equal(overview.visible, false, 'hidden on the first frame');
  overview.visible = true; // CS2 opens it (Ctrl)
  ui.render({});
  assert.equal(overview.visible, false, 'hidden again the next frame');
});

test('the timeline publishes the camera CS2 camera button shows', () => {
  const ui = createUi();
  const label = ui.find('CameraModeLabel');
  const published = () => ui.panels.find(p => 'cammode' in p.attrs)?.attrs.cammode;
  for (const [text, mode] of [['First Person', '0'], ['Chase Cam', '1'], ['Free Cam', '2'], ['', '-1']]) {
    label.text = text;
    ui.render({});
    assert.equal(published(), mode, `"${text}" -> ${mode}`);
  }
});

test('the native -/+ time step buttons skip 5 seconds through mirv_filmmaker skip', () => {
  const ui = createUi();
  ui.render({ hosted: false });
  assert.equal(ui.dialogVars.timestep_value, 5);
  ui.commands.length = 0;
  ui.find('TimeStepBackward').events.onactivate();
  ui.find('TimeStepForward').events.onactivate();
  assert.deepEqual(ui.commands, ['mirv_filmmaker skip back', 'mirv_filmmaker skip forward']);
});

test('the graph builds and renders without adding a duplicate loop control', () => {
  const ui = createUi();
  ui.render({ open: false, graphExp: true });
  ui.load('GraphEditorJs.h');
  assert.equal(typeof ui.$.GraphExp?.render, 'function');
  for (let i = 0; i < 3; i++) ui.$.GraphExp.render();
  ui.checkErrors();
  assert.equal(ui.find('GraphExpRoot').visible, true);
  assert.equal(ui.find('LoopBtn').visible, false);
  assert.equal(ui.find('CamTimelineRangeRoot').visible, false);
  assert.equal(ui.panels.filter(p => p.id === 'LoopBtn').length, 1);
  assert.deepEqual(ui.commands.filter(c => /mirv_filmmaker loop/.test(c)), []);
});


test('undocked playback reads native marker geometry only when docking takes ownership', () => {
  const ui = createUi();
  ui.render({ hosted: false });
  // Finish the existing dock's restore/settle window.
  for (let i = 0; i < 5; ++i) ui.render();
  const markers = ui.find('HighlightIcons');
  const marker = ui.makePanel('Panel', markers, 'test-round');
  marker.actualxoffset = 120;
  marker.actuallayoutwidth = 80;
  let reads = 0;
  const readChild = markers.GetChild;
  markers.GetChild = function (i) { ++reads; return readChild.call(this, i); };
  for (let i = 0; i < 20; ++i) ui.render({ tick: 310 + i });
  assert.equal(reads, 0, 'plain playback must not scan every marker each frame');
  ui.render({ hosted: true });
  assert.ok(reads > 0, 'capture the live basis before taking ownership');
  assert.equal(marker.style.marginLeft, '120.0px');
  ui.render({ hosted: false });
  for (let i = 0; i < 5; ++i) ui.render();
  // The native script can replace/reposition markers while undocked.
  marker.actualxoffset = 250;
  ui.render({ hosted: true });
  assert.equal(marker.style.marginLeft, '250.0px');
});

test('unchanged state still follows layout changes and camera-mode readback', () => {
  const ui = createUi();
  ui.render({ hosted: true });
  const width = ui.contents.style.width;
  ui.ctx.actuallayoutwidth = 1440;
  ui.find('CameraModeLabel').text = 'Free Camera';
  ui.render();
  assert.notEqual(ui.contents.style.width, width);
  assert.equal(ui.find('CamTimelineRoot').attrs.cammode, '2');
});
