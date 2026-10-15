import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import vm from 'node:vm';
import { flatten } from '../../UiMockup/lib/panorama-headers.mjs';

// Execute the actual preview fragment, including pointer ownership across delayed native state.
function preview(card = 'env.sky') {
  function panel() {
    return { style: {}, visible: false, text: '', SetPanelEvent() {}, RemoveAndDeleteChildren() {} };
  }
  const root = { actualuiscale_x: 1, actuallayoutwidth: 1600, actuallayoutheight: 900,
    GetAttributeString: () => '0 0 0.8 0.8', GetPositionWithinWindow: () => ({x: 0, y: 0}) };
  const commands = [];
  const context = vm.createContext({ root, $: { CreatePanel: panel }, mk: panel,
    lbl: () => panel(), S: {label: '#ffffff'}, ENV_ACCENT: '#7fe0d0',
    envAxisColors: ['red', 'green', 'blue'], envAxisNames: ['X', 'Y', 'Z'],
    gizmoTag: panel, gizmoTagAt() {}, envDrawLine() {}, envAxisHit: () => -1,
    renderHooks: [], activeTab: 'environment', INSPECTOR_W: 300,
    settingsPopup: { isOpen: () => false }, cfgDragging: () => false, cfgDropOpen: false,
    cfgRails: {environment: {sel: card}}, propsCommand: c => commands.push(c),
    olSelect() {}, olMenu() {}, olFind() {} });
  vm.runInContext(flatten(path.resolve('AfxHookSource2/Filmmaker/Panorama'), 'ConfigHudPropsGizmoJs.h'), context);
  const state = {enabled: true, cursor: true, mx: 512, my: 14.4, lmb: false, rmb: false,
    props: {selected: 0, grab: '', markers: []}};
  const frame = patch => { Object.assign(state, patch); context.renderHooks[0](state); return context.propsHint; };
  return {context, state, commands, frame};
}

test('empty preview press never claims a move while held, including delayed picking', () => {
  const p = preview(); p.frame(); p.frame({lmb: true});
  for (let i = 0; i < 12; i++) {
    const hint = p.frame();
    assert.equal(hint.visible, false); assert.doesNotMatch(hint.text, /Moving|put it back/);
  }
  p.frame({lmb: false});
  assert.equal(p.commands.filter(c => c.startsWith('pickgrab ')).length, 1);
  assert.equal(p.commands.filter(c => c === 'release').length, 1);
});

test('Scene Objects keeps its idle guidance during an empty click', () => {
  const p = preview('env.scene'); p.frame({lmb: true});
  const hint = p.frame(); assert.equal(hint.visible, true);
  assert.match(hint.text, /Click an asset/); assert.doesNotMatch(hint.text, /Moving/);
});

test('selected but locked or rejected pick never claims a move', () => {
  const p = preview(); p.state.props.selected = 7;
  p.frame({lmb: true}); const hint = p.frame();
  assert.match(hint.text, /Drag it to move/); assert.doesNotMatch(hint.text, /^Moving/);
});

test('confirmed native grabs retain movement, rotation, snapping and release', () => {
  for (const [grab, label] of [['move', 'Moving over the ground'], ['axis', 'Moving'],
    ['turn', 'Turning'], ['tilt', 'Tilting'], ['roll', 'Rolling']]) {
    const p = preview(); p.frame({lmb: true});
    p.state.props.selected = 7; p.state.props.grab = grab;
    assert.match(p.frame().text, new RegExp('^' + label));
    p.frame({shift: true, mx: 520});
    assert.ok(p.commands.some(c => /^drag .* 1$/.test(c)));
    p.frame({lmb: false}); assert.ok(p.commands.includes('release'));
  }
});

test('a fast click queues release even before native confirmation; closing UI releases too', () => {
  for (const patch of [{lmb: false}, {cursor: false}, {enabled: false}]) {
    const p = preview(); p.frame({lmb: true}); p.frame(patch);
    assert.equal(p.commands.filter(c => c === 'release').length, 1);
  }
});
