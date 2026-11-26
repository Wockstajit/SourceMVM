import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const panoDir = path.join(repoRoot, 'AfxHookSource2', 'Filmmaker', 'Panorama');

function readJs(name) {
  const txt = fs.readFileSync(path.join(panoDir, name), 'utf8');
  return [...txt.matchAll(/R"CFJS\(([\s\S]*?)\)CFJS"/g)].map((m) => m[1]).join('\n');
}

function harness() {
  const sent = [];
  const sections = [];
  const renderHooks = [];
  const makePanel = () => ({
    style: {},
    visible: true,
    hittest: true,
    hittestchildren: true,
    actuallayoutwidth: 400,
    actuallayoutheight: 200,
    actualuiscale_x: 1,
    actualuiscale_y: 1,
    SetPanelEvent(ev, fn) { this[ev] = fn; },
    SetAttributeString(k, v) { this[k] = v; },
    GetPositionWithinWindow() { return { x: 0, y: 0 }; },
    GetParent() { return root; },
    DeleteAsync() {},
  });
  const root = { ...makePanel(), visible: true };
  const label = (text = '') => ({ ...makePanel(), text });
  const button = (text, onClick) => ({ ...makePanel(), __lbl: { style: {}, text }, click: onClick });
  const section = (parent, title, _col, _open, persistKey, edgeColor) => {
    const s = {
      ...makePanel(),
      title,
      persistKey,
      edgeColor,
      note: '',
      __note(t) { s.note = String(t); },
      __group() { return makePanel(); },
      __headerReset() {},
    };
    sections.push(s);
    return s;
  };
  const togglePill = (_parent, caption, _tip, onToggle) => {
    const row = { ...makePanel(), caption, value: false, set(v) { row.value = !!v; }, toggle: onToggle };
    return row;
  };
  const actionGroup = (_parent, _cap, items) => ({
    row: makePanel(),
    btns: items.map(([cap, fn]) => button(cap, fn)),
  });
  const dropdownRow = (_parent, cap, items, onPick) => {
    const api = {
      row: makePanel(),
      label: cap,
      items,
      value: items && items.length ? items[0][0] : null,
      sync(v) { api.value = v; },
      pick(v) { api.value = v; if (onPick) onPick(v); },
    };
    return api;
  };
  const sliderRow = (_parent, opts) => {
    const s = {
      row: makePanel(),
      value: opts ? opts.min : 0,
      sync(v) { s.value = v; },
      change(v) { s.value = v; if (opts && opts.onChange) opts.onChange(v); },
    };
    return s;
  };

  const i18n = fs.readFileSync(path.join(panoDir, 'LocalizationJs.h'), 'utf8')
    .split('R"MVMI18N(')[1].split(')MVMI18N"')[0];

  let blurred = 0;
  const ctx = vm.createContext({
    $: {
      CreatePanel() { return { ...makePanel(), SetPlayerModel() {}, SetReadyForDisplay() {}, IsValid: () => true }; },
      Schedule() {},
    },
    root,
    pageBots: makePanel(),
    activeTab: 'bots',
    cfgSearchActive: false,
    cfgRails: { bots: { sel: 'bots.anims' } },
    cfgTextOwner: null,
    configWindowBlur() { blurred++; },
    configWindowChars() {},
    applyTabs() {},
    cfgSelectCard(_tab, key) { ctx.cfgRails.bots.sel = key; },
    S: { value: '#fff', label: '#ccc', muted: '#999', hint: '#aaa', clear: 'rgba(0,0,0,0)', tileBorder: '#333', fontMonodigit: 'mono' },
    mk: () => makePanel(),
    lbl: (_p, t) => label(t),
    btn: (_p, t, fn) => button(t, fn),
    setBtnOn() {},
    section,
    togglePill,
    actionGroup,
    dropdownRow,
    sliderRow,
    hoverable() {},
    makeInteractive(b, l, fn) { b.click = fn; b.__lbl = l; },
    endTileLine() {},
    btOpen: -1,
    renderHooks,
    cmd(c) { sent.push(c); },
  });

  vm.runInContext(i18n, ctx);
  vm.runInContext(readJs('ConfigHudActorAnimPickerJs.h'), ctx);
  return { ctx, sent, sections, renderHooks, root, getBlurred: () => blurred };
}

test('inline bots.anims card replaces fullscreen modal, cleans filter rows to dropdowns, and keeps Bots Blend toggle', () => {
  const src = readJs('ConfigHudActorAnimPickerJs.h');
  assert.doesNotMatch(src, /aapOverlay|CreatePanel\('TextEntry'|aapChipRow/, 'no fullscreen modal overlay, TextEntry, or cluttered chip strips');
  assert.match(readJs('ConfigHudWidgetsJs.h'), /bots:\s*\[\[/, 'CFG_RAIL registers bots tab cards including bots.anims');
  assert.match(readJs('ConfigHudBotsTabJs.h'), /btWalkBlend\s*=\s*togglePill\([\s\S]*Blend with CS walk/, 'Bots RECORD AND PLAY includes Blend with CS walk toggle');
  const h = harness();
  assert.equal(h.sections.length, 1);
  assert.equal(h.sections[0].persistKey, 'bots.anims');
  assert.equal(h.ctx.aapAct1.btns.length, 2);
  assert.equal(h.ctx.aapAct2.btns.length, 2);
  assert.equal(h.ctx.aapCatDrop.value, -1);
  assert.equal(h.ctx.aapSrcDrop.value, 'all');
  assert.equal(h.ctx.aapWpnDrop.value, 'auto');
});

test('keyboard-bridge search and Speed slider filter catalog clips and drive live speed', () => {
  const h = harness();
  const c = h.ctx;
  c.$.ConfigHudAap.setCatalog([
    ['hla_walk_fwd', 'Walk Forward', 0, 1.0, 1, 1, 1, 68.1],
    ['hla_explosion_wince_s', 'Explosion Wince (Back)', 5, 0.9, 0, 0, 1, 27.8],
    ['hla_shoot_burst', 'Shoot Burst', 2, 1.63, 0, 0, 1, 0.0],
  ]);
  assert.equal(c.aapFiltered.length, 3);
  c.aapSearchField.onactivate();
  assert.equal(h.root.textcapture, '1', 'clicking search claims window text capture');
  c.configWindowChars('101,120,112,108');
  assert.equal(c.aapQuery, 'expl');
  assert.equal(c.aapFiltered.length, 1);
  assert.equal(c.aapFiltered[0][0], 'hla_explosion_wince_s');
  c.configWindowChars('8,8,27');
  assert.equal(c.aapQuery, 'ex');
  assert.ok(h.getBlurred() >= 1);
  c.aapSearchClear.onactivate();
  assert.equal(c.aapFiltered.length, 3);
  c.aapCatDrop.pick(5);
  assert.equal(c.aapFiltered.length, 1);
  c.aapCatDrop.pick(-1);
  assert.equal(c.aapFiltered.length, 3);
  c.aapSpeedRow.change(1.85);
  assert.ok(h.sent.includes('mirv_filmmaker takes anim speed 1.85'), 'Speed slider sends live speed command');
  assert.equal(c.$.ConfigHudAap.focusSearch(), 'ok');
  c.configWindowChars('69,120,112,108');
  assert.equal(c.$.ConfigHudAap.pick(0), 'hla_explosion_wince_s loop=0');
  assert.equal(c.aapLoop, false);
});

test('selecting a one-shot clip defaults Loop to false, 50/50 split Place/Play buttons work', () => {
  const h = harness();
  const c = h.ctx;
  c.$.ConfigHudAap.setCatalog([
    ['hla_walk_fwd', 'Walk Forward', 0, 1.0, 1, 1, 1, 68.1],
    ['hla_explosion_wince_s', 'Explosion Wince (Back)', 5, 0.9, 0, 0, 1, 27.8],
  ]);
  assert.equal(c.aapLoop, true, 'first looping clip defaults Loop ON');
  c.aapCards[1].onactivate();
  assert.equal(c.aapClipId, 'hla_explosion_wince_s');
  assert.equal(c.aapLoop, false, 'selecting one-shot automatically sets Loop OFF');
  c.aapAct2.btns[0].click();
  assert.ok(h.sent.some((s) => s.startsWith('mirv_filmmaker takes actor place hla_explosion_wince_s ')));
  c.aapUpperPill.toggle();
  assert.ok(h.sent.includes('mirv_filmmaker takes anim walkblend on'));
});

test('double-clicking a clip row plays or assigns immediately and Bots right-click menu deletes a take', () => {
  const h = harness();
  const c = h.ctx;
  c.$.ConfigHudAap.setCatalog([
    ['hla_walk_fwd', 'Walk Forward', 0, 1.0, 1, 1, 1, 68.1],
    ['hla_shoot_burst', 'Shoot Burst', 2, 1.63, 0, 0, 1, 0.0],
  ]);
  h.sent.length = 0;
  c.aapCards[1].onactivate();
  c.aapCards[1].onactivate();
  assert.ok(h.sent.some((s) => s.startsWith('mirv_filmmaker takes anim play hla_shoot_burst ')),
    'double-clicking clip row immediately replaces active player animation');

  const botsSrc = readJs('ConfigHudBotsTabJs.h');
  assert.match(botsSrc, /SetPanelEvent\('oncontextmenu'[\s\S]*cfgMenu\([\s\S]*Delete take[\s\S]*delete /,
    'Take row defines right-click context menu with Delete take action');
});
