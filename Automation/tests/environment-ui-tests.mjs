// Contract tests for the CONFIG panel's ENVIRONMENT tab.
//
// The four JS fragments are run against a throwaway Panorama/widget stub, so every assertion is
// about BEHAVIOUR -- which console command a control sends, what gates it, whether a stale
// selection can still edit an object -- rather than about markup.
//
// The 2026-09 handoff redesign replaced the tab's six-button sub-navigation with the card grammar
// shared with the rest of the panel: seven pass cards plus SCENE OBJECTS, each with a header
// (accent edge, live summary, Reset, on/off switch) and text-tab sub-pages. The stub therefore has
// to implement that widget surface, and the labels below are the redesigned ones. The behavioural
// contracts are unchanged and are what these tests still hold.
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

// The JS of a fragment, with any fragment it #includes spliced in where the include sits -- the
// same order the compiler concatenates the raw string literals in.
const read = (name) => {
  const text = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');
  const parts = [];
  const token = /R"CFJS\(([\s\S]*?)\)CFJS"|#include "(ConfigHud[A-Za-z]*Js\.h)"/g;
  let m;
  // The view overlays (environment and CS2-asset gizmos) build Panorama panels at load; they are
  // checked by their own tests, not loaded here.
  while ((m = token.exec(text)))
    if (m[1] !== undefined) parts.push(m[1]);
    else if (!/^ConfigHud(Environment|Props)GizmoJs\.h$/.test(m[2])) parts.push(read(m[2]));
  return parts.join('\n');
};

// A fragment's own JS only, without what it #includes.
const readOwn = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8')
    .split('R"CFJS(').slice(1).map((chunk) => chunk.split(')CFJS"')[0]).join('\n');

const nav = read('ConfigHudEnvironmentNavJs.h');
const climate = read('ConfigHudClimateJs.h');
const fragment = read('ConfigHudEnvironmentTabJs.h');

// Geometry only; the stub never lays anything out, but the fragments read these while building.
const UI = {
  col: { w: 640, pad: 22, gap: 10 },
  line: { h: 32, label: 130, value: 66, gap: 5, padH: 10, colGap: 12 },
  card: { padV: 14, padH: 16, radius: 5, gap: 10, edgeW: 3, edgeH: 32, titleFont: 16, subFont: 13, inner: 442 },
  tile: { h: 76, padTop: 11, padBottom: 12, padH: 13, radius: 4, labelFont: 13, valueFont: 22, barH: 3, perRow: 2 },
  tab: { font: 12, ls: 1.5, gap: 22, padBottom: 10, underline: 2 },
  tabTop: { font: 15, ls: 1.4, gap: 20, padBottom: 13 },
  chip: { font: 13, padV: 8, padH: 13, radius: 3 },
  pill: { w: 44, h: 24, knob: 18, travel: 20 },
  pillSm: { w: 40, h: 22, knob: 16, travel: 18 },
  mo: { arrive: 'ease', tab: 0.16, panel: 0.16, hover: 0.12, rise: 4 }
};

function harness(withLighting = false) {
  const buttons = [], sliders = [], toggles = [], modes = [], sent = [], cards = [], groups = [], pickers = [], colorFields = [];
  const panel = () => ({
    style: {}, visible: true, hittest: true, hittestchildren: true,
    SetPanelEvent(event, handler) { this[event] = handler; },
    SetAttributeString(key, value) { this[key] = value; },
    RemoveAndDeleteChildren() {},
    GetChild() { return { style: {}, text: '' }; }
  });
  const label = (text) => ({ ...panel(), text: text === undefined ? '' : text });
  const button = (text, onClick) => {
    const b = { ...panel(), __lbl: { style: {}, text }, click: onClick, __off: false };
    buttons.push(b);
    return b;
  };
  const grid = (parent, caption, items, onPick) => {
    const btns = {};
    items.forEach(([key, cap]) => { btns[key] = button(cap, () => onPick(key)); });
    const g = { row: panel(), caption, btns, label: label(caption) };
    groups.push(g);
    return g;
  };

  function section(parent, title, collapsible, startOpen, persistKey, edgeColor) {
    const card = {
      ...panel(), title, persistKey, edgeColor, note: '', resets: [], pages: [], groups: [],
      __card: panel(),
      __page(head) { const pg = panel(); pg.head = head; pg.__note = () => {}; card.pages.push(pg); return pg; },
      // A group is a divider plus a panel on the card's one page (2026-10 sidebar cleanup).
      __group(head) { const g = panel(); g.head = head; g.__note = () => {}; card.groups.push(g); return g; },
      __setPage(i) { card.selectedPage = i; }, __pageCount() { return card.pages.length; },
      __note(text) { card.note = text === undefined || text === null ? '' : String(text); },
      __setOpen() {}, __getOpen() { return true; }, __headerHost() { return panel(); },
      __headerAction(cap, onClick) { const b = button(cap, onClick); card.action = b; return b; },
      __headerReset(cmds) {
        // A reset is a SPEC now -- { off, cmds, after } -- so that it goes OFF before it goes
        // back to defaults (a value already at its default was otherwise a no-op write and
        // nothing re-applied). Keep `resets` as the flat default-writes the assertions read,
        // and record the off phase separately so it can be asserted on its own.
        card.resets = (cmds && cmds.cmds) ? cmds.cmds : cmds;
        card.resetsOff = (cmds && cmds.off) ? cmds.off : [];
        card.resetAfter = (cmds && cmds.after) || null;
        const b = button('Reset', () => [...card.resetsOff, ...card.resets].forEach((c) => sent.push(c)));
        b.setDisabled = (off) => { b.__off = !!off; };
        card.resetBtn = b;
        return b;
      },
      __headerPill(onClick, tip) {
        const p = { row: panel(), pill: panel(), on: false, tip, click: onClick, set(v) { this.on = !!v; } };
        card.pill = p;
        return p;
      }
    };
    cards.push(card);
    return card;
  }

  const context = {
    UI,
    pageEnvironment: panel(), pageEffects: panel(), root: panel(), inspector: panel(), cfgScrollY: 0, renderHooks: [], st: null,
    envPrepBanner: panel(), envPrepSpin: label(''), envPrepText: label(''), envPrepTick: 0,
    S: new Proxy({}, { get: (_, key) => (typeof key === 'string' ? '#000000ff' : undefined) }),
    mk: panel, row: panel, footerCard: panel, subHead: panel, endTileLine() {}, attachTip() {},
    section,
    lbl: (parent, text) => label(text),
    btn: (parent, text, onClick) => button(text, onClick),
    wideBtn: (parent, text, onClick) => button(text, onClick),
    tabify: (b) => b, styleTab() {}, setBtnOn() {},
    setBtnDisabled(b, off) { if (b) b.__off = !!off; },
    actionRowEnd: () => panel(), actionBtn: (parent, text, onClick) => button(text, onClick),
    segmented: (parent, items, width, onChange) => ({ sync() {}, choose: onChange }),
    modeRow(parent, label_, items, width, onPick, tip) {
      const btns = {};
      items.forEach(([key, cap]) => { btns[key] = button(cap, () => onPick(key)); });
      const m = { label: label_, tip, row: panel(), btns, value: null, sync(v) { this.value = v; }, choose: onPick };
      modes.push(m);
      return m;
    },
    optionGrid: (parent, caption, items, perRow, tip, onPick) =>
      ({ ...grid(parent, caption, items, onPick), sync() {} }),
    actionGroup(parent, caption, items, perRow, note) {
      const btns = items.map(([cap, onClick]) => button(cap, onClick));
      const g = { row: panel(), caption, btns, note: label(note === undefined ? '' : note) };
      groups.push(g);
      return g;
    },
    sliderRow(parent, opts) { const s = { opts, parent, row: panel(), slot: panel(), sync(value) { this.value = value; } }; sliders.push(s); return s; },
    swatchGrid() { return {select() {}, paintCustom() {}}; },
    // Lives in ConfigColorPickerJs.h, which this harness does not load; the climate fragment
    // calls it for scene tint, sun colour and fog colour.
    colorField(parent, title, readColor, writeColor) {
      colorFields.push({ parent, title, read: readColor, write: writeColor });
    },
    // `visible()` is part of the contract, not a convenience: every custom-colour sync guards on
    // it so a render hook cannot overwrite the buffer the open picker is editing.
    colorPicker() {
      const p = { shown: false, hide() { p.shown = false; }, show() { p.shown = true; },
                  visible() { return p.shown; }, sync() {} };
      pickers.push(p);
      return p;
    },
    togglePill(parent, label_, hint, onClick, tip) {
      const t = { row: panel(), label: label_, hint, tip, parent, on: false, click: onClick, set(v) { this.on = !!v; } };
      toggles.push(t);
      return t;
    },
    keepToggle: () => ({ sync() {} }),
    // The redesigned Scene Objects pane: its inner lists, the preset popover and the danger button.
    scrollWell: () => ({ inner: panel(), clip: panel(), reset() {}, apply() {}, wheel() { return false; } }),
    paintBtn() {}, cfgDropLayer: panel, cfgDropdownClose() {}, cfgValid: () => true, cfgConfirm() {},
    cfgSelectCard() {}, cfgDropOpen: null,
    // The outliner's right-click menu, its Delete/Ctrl+D hook and its group dropdown.
    keyHooks: [], cfgMenu(items) { context.__menu = items; }, cfgRails: { environment: { sel: 'env.scene' } },
    dropdownRow: () => ({ row: panel(), setItems() {}, sync() {}, close() {} }),
    resetFooter(card, resolve) { context.__footerResolve = resolve; return { paint() {}, disarm() {}, btn: panel() }; },
    configWindowBlur() { context.root.SetAttributeString('textcapture', '0'); },
    configWindowChars() {},
    cmd: (s) => sent.push(s),
    // Panorama globals the CS2-asset preview touches: it registers $.ConfigHudProps and builds its
    // 3D panel only when a model is shown, which these tests never do.
    $: { Schedule() {}, Msg() {}, CreatePanel: () => panel() }
  };
  vm.createContext(context);
  vm.runInContext(nav + climate + fragment + (withLighting ? read('ConfigHudLightingTabJs.h') : ''), context);

  const scene = {
    climate: {
      enabled: 0, relighting: 0, snow: 0.4, clouds: 1, storm: 0, stormInterval: 9, stormStrength: 2, fogStart: 0,
      sky: 1, coverage: 0.55, density: 0.65, cloudBase: 1800, cloudDepth: 1200, cloudScale: 0.0008,
      windSpeed: 25, sunAzimuth: 135, sunElevation: 35, sunPower: 1, haze: 0.4, stars: 0.3,
      fogDensity: 0, fogHeight: 320, fogBase: 0, fogNoise: 0.4, fogRange: 4000, scatter: 0.45,
      lightShafts: 1, localFogRadius: 0, wet: 0, puddles: 0, puddleHeight: 128, puddleDepth: 24,
      surfaceRadius: 0, maskScale: 0.02, ripple: 0.35, reflectionRoughness: 0.35,
      relight: 0, ambient: 0.3, sunRelight: 0.6, bakedSuppressed: 0, bakedFill: 0.6, contactShadows: 1,
      shadowBias: 2, worldGi: 0, bounce: 1, bounceSteps: 1, bounceSize: 2048, bounceX: 0, bounceY: 0, bounceZ: 0,
      bounceAlbedo: 0.55, sceneReflections: 0, particleDetail: 1, turbulence: 1,
      particleOpacity: 0.65, softness: 8, streak: 12, particleLight: 0.4,
      bloom: 0, bloomThreshold: 0.8, saturation: 1, contrast: 1, vignette: 0, grain: 0, chromatic: 0
    },
    enabled: true, map: 'de_inferno', fog: 0, exposure: 1, ambient: 0.6,
    fogColor: [0.1, 0.2, 0.3], tint: [1, 1, 1],
    weather: { type: 1, count: 2400, follow: true, shelter: false, radius: 900, height: 900, size: 1, speed: 700, windX: 40, windY: 0 },
    objects: []
  };
  const render = (objects = [], selected = 0, extra = {}) => {
    context.st = {
      environment: { ...scene, objects }, envSelected: selected,
      envAssets: ['Lantern'], envPresets: ['Night Shoot'], envMap: 'de_inferno',
      envOcclusionReady: false, envTextures: ['brick.png'], ...extra
    };
    context.renderHooks.forEach((fn) => fn(context.st));
  };
  const slider = (name) => {
    const s = sliders.find((x) => x.opts.label === name);
    assert.ok(s, 'expected a value tile labelled ' + name);
    return s;
  };
  const press = (caption) => {
    const b = buttons.find((x) => x.__lbl.text === caption);
    assert.ok(b, 'expected a button captioned ' + caption);
    b.click();
    return b;
  };
  const toggle = (name) => {
    const t = toggles.find((x) => x.label === name);
    assert.ok(t, 'expected a switch tile labelled ' + name);
    return t;
  };
  const card = (title) => {
    const c = cards.find((x) => x.title === title);
    assert.ok(c, 'expected a card titled ' + title);
    return c;
  };
  return { context, buttons, sliders, toggles, modes, sent, cards, groups, pickers, colorFields, render, slider, press, toggle, card };
}


test('seven task cards, one page each with groups as dividers', () => {
  const h=harness();h.render();
  assert.deepEqual(h.cards.map(c=>c.title), ['LOOKS','SKY','LIGHTING','FOG','WEATHER','SCENE OBJECTS','ROUND HEAT MAP']);
  // 2026-10 sidebar cleanup: a card is ONE page with group headings; sub-tabs stay only where a
  // page would not fit the pane (Scene Objects).
  assert.deepEqual(h.card('LOOKS').groups.map(p=>p.head),['Presets','Color','Lens']);
  assert.equal(h.card('LOOKS').pages.length,0);
  assert.deepEqual(h.card('WEATHER').groups.map(p=>p.head),['Weather','Ground','Lightning']);
  assert.deepEqual(h.card('SCENE OBJECTS').pages.map(p=>p.head),['Outliner','Library']);
  // Lights and cameras are added from ONE header menu; the four add buttons are gone.
  assert.equal(h.card('SCENE OBJECTS').action.__lbl.text,'+ Add');
  for(const kind of ['+ Point','+ Spot','+ Area']) assert.ok(!h.buttons.some(b=>b.__lbl.text===kind),kind+' is gone');
  // The scene preset menu lives in the LOOKS header now.
  assert.ok(h.context.envPresetBtn,'preset button exists');
  assert.equal(h.sent.length,0,'rendering alone changes no state');
});
test('Sun page sends independent light and shadow commands and respects takeover',()=>{
  const h=harness(true);h.render([],0,{engineLights:true,bakedLighting:true,bakedShadows:true,dynamicShadows:false});
  assert.equal(h.sent.length,0);
  h.context.st.environment.enabled=false;
  h.toggle('Engine lights').click();assert.equal(h.sent.at(-1),'mirv_filmmaker world light enabled toggle');
  h.toggle('Engine shadows').click();assert.equal(h.sent.at(-1),'mirv_filmmaker world shadows off');
  h.toggle('Baked shadows').click();assert.equal(h.sent.at(-1),'mirv_filmmaker world bakedshadows toggle');
  h.toggle('Live engine shadows').click();assert.equal(h.sent.at(-1),'mirv_filmmaker world dynamicshadows toggle');
  const s=h.context.st;s.environment.enabled=true;Object.assign(s.environment.climate,{enabled:1,relighting:1});
  h.context.renderHooks.forEach(fn=>fn(s));
  const before=h.sent.length;h.toggle('Engine lights').click();h.toggle('Baked shadows').click();
  assert.equal(h.sent.length,before,'owned switches cannot fight the Environment frame pump');
});
test('lighting controls expose real sources without baked fill or contrast flattening',()=>{
  const h=harness(true);h.render();
  for(const label of ['Map lights','Ambient light','Sun brightness','Sun size','Light source angle','Shadow softness'])h.slider(label);
  for(const label of ['Unbaked fill','Flatten contrast','Surface sunlight'])
    assert.ok(!h.sliders.some(s=>s.opts.label===label),label);
  assert.ok(!h.toggles.some(t=>t.label==='Baked lighting'||t.label==='Remove baked lighting'));
  h.toggle('Cast shadows').click();assert.match(h.sent.at(-1),/climate contactShadows [01]$/);
});
test('creative controls replace technical and quality controls',()=>{
  const h=harness();h.render();
  for(const name of ['Exposure','Saturation','Contrast','Bloom','Vignette','Film grain','Chromatic shift',
    'Density','Start distance','Height','Variation','Fall speed','Wind direction']) h.slider(name);
  for(const name of ['Ray distance','Forward scattering','Surface offset','Volume size','Particle size','Field radius'])
    assert.ok(!h.sliders.some(s=>s.opts.label===name),name);
  assert.doesNotMatch(nav+climate+fragment,/exportQuality|ray-preview|environment set fog /);
});
test('weather percent and wind translate to atomic backend edits',()=>{
  const h=harness();h.render();
  assert.equal(h.slider('Intensity').value,40);
  h.slider('Intensity').opts.onChange(55);
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment weather set count 3300');
  h.slider('Wind direction').opts.onChange(90);
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment weather wind 40 90');
  h.card('SCENE OBJECTS').action.click();
  const pointItem=h.context.__menu.find(i=>i&&i[0]==='Point light');
  assert.ok(pointItem,'the + Add menu offers a point light');
  assert.equal(JSON.stringify(h.context.__menu.filter(Boolean).map(i=>i[0])),JSON.stringify(['Point light','Spot light','Area light','Camera']));
  pointItem[1]();assert.equal(h.sent.at(-1),'mirv_filmmaker environment add point');
  assert.equal(h.card('SCENE OBJECTS').selectedPage,0,'a new light opens on the Outliner');
});
const prop={id:14,kind:'box',asset:'',enabled:true,position:[100,200,300],angles:[10,20,30],
  scale:[16,24,32],color:[.3,.5,.7],materialEnabled:true};
test('stable-ID selection opens the mesh inspector without camera commands or page jumps',()=>{
  const h=harness();h.render([prop],14);
  assert.ok(h.context.envInspectors.props.visible);
  assert.equal(h.card('SCENE OBJECTS').selectedPage,undefined,'selecting never switches the page');h.slider('Half width').opts.onChange(80);
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment object 14 sx 80');
  h.press('Edit position');assert.equal(h.sent.at(-1),'mirv_filmmaker environment edit start 14');
  // While its arrows are armed the same button says Done moving and puts them away.
  h.render([prop],14,{envEditing:14});
  h.press('Done moving');assert.equal(h.sent.at(-1),'mirv_filmmaker environment edit stop');
  h.render([],14);const n=h.sent.length;h.press('Duplicate');assert.equal(h.sent.length,n);
  assert.ok(h.sent.every(c=>!c.includes('camera')));
});
test('automatic selection and light-specific controls',()=>{
  const h=harness();h.render([prop],14);
  assert.equal(h.card('SCENE OBJECTS').selectedPage,undefined);
  const point={...prop,id:22,kind:'point',intensity:3,range:900};
  h.render([point],22,{envEditing:22});
  assert.ok(h.context.envInspectors.lights.visible);
  assert.ok(!h.context.envInspectors.props.visible);
  for(const row of h.context.envObjectControls.lights)
    if(['cone','width','height'].includes(row[1])) assert.equal(row[2].visible,false);
  assert.equal(h.context.envInspectors.lights.__place.__lbl.text,'Done moving');
});
test('scoped resets keep unrelated cards intact',()=>{
  const h=harness();h.render();
  const fog=h.card('FOG').resets.join(' ');
  assert.match(fog,/climate fogStart 0/);assert.match(fog,/climate fogDensity 0/);
  assert.match(fog,/set fogR 0.45/);assert.doesNotMatch(fog,/climate clouds|climate bloom/);
  const looks=h.card('LOOKS').resets.join(' ');
  assert.match(looks,/climate saturation 1/);assert.match(looks,/set exposure 1/);
  assert.match(looks,/set tintB 1/);assert.doesNotMatch(looks,/climate fogDensity/);
  assert.match(h.card('WEATHER').resets.join(' '),/climate storm 0/);
  assert.equal(h.context.__footerResolve().cmds[0],'mirv_filmmaker environment reset all');
});
test('weather switch includes standalone lightning',()=>{
  const h=harness();h.render();
  const s=h.context.st;s.environment.weather.type=0;
  Object.assign(s.environment.climate,{enabled:1,snow:0,wet:0,puddles:0,storm:1});
  h.context.renderHooks.forEach(fn=>fn(s));
  assert.equal(h.card('WEATHER').pill.on,true);
  h.card('WEATHER').pill.click();
  assert.ok(h.sent.includes('mirv_filmmaker environment climate storm 0'));
});

test('switching Sky off releases an otherwise idle renderer despite stale native state',()=>{
  const h=harness();h.render();const s=h.context.st.environment;
  Object.assign(s.climate,{enabled:1,sky:1,clouds:1,relighting:1,snow:0,wet:0,puddles:0});
  s.weather.type=0;
  h.card('SKY').pill.click();
  assert.ok(h.sent.includes('mirv_filmmaker environment climate sky 0'));
  assert.ok(h.sent.includes('mirv_filmmaker environment climate clouds 0'));
  assert.equal(h.sent.at(-2),'mirv_filmmaker environment climate enabled 0');
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment off');
});

test('switching Sky off keeps resources for unrelated active passes and objects',()=>{
  for(const other of ['fog','weather','object','grade']) {
    const h=harness();h.render();const s=h.context.st.environment;
    Object.assign(s.climate,{enabled:1,sky:1,clouds:1,relighting:1,snow:0,wet:0,puddles:0});
    s.weather.type=0;
    if(other==='fog')s.climate.fogDensity=.4;
    if(other==='weather')s.weather.type=2;
    if(other==='object')s.objects=[{id:4,enabled:true}];
    if(other==='grade')s.exposure=2;
    h.card('SKY').pill.click();
    assert.ok(!h.sent.includes('mirv_filmmaker environment off'),other);
    assert.ok(!h.sent.includes('mirv_filmmaker environment climate enabled 0'),other);
  }
});
test('editing lighting activates surface relighting',()=>{
  const h=harness();h.render();h.slider('Map lights').opts.onChange(.8);
  assert.ok(h.sent.includes('mirv_filmmaker environment climate relighting 1'));
});
test('bounce intensity supports fractional strength and zero without disabling lighting',()=>{
  const h=harness();h.render();const slider=h.slider('Bounce intensity');
  for(const value of [0,.35,2.5]) {
    slider.opts.onChange(value);
    assert.ok(h.sent.includes('mirv_filmmaker environment climate bounce '+value));
  }
  assert.ok(!h.sent.some(s=>/environment off|climate gi 0|climate relighting 0/.test(s)));
});
test('bounce steps support one through three updates per frame',()=>{
  const h=harness();h.render();const slider=h.slider('Bounce steps/frame');
  assert.equal(slider.opts.min,1);assert.equal(slider.opts.max,3);assert.equal(slider.opts.decimals,0);
  slider.opts.onChange(3);
  assert.ok(h.sent.includes('mirv_filmmaker environment climate bounceSteps 3'));
});
test('lighting reset restores independent source defaults without enabling baked fill',()=>{
  const h=harness();h.render();h.card('LIGHTING').resetBtn.click();
  assert.ok(h.sent.some(s=>s==='mirv_filmmaker environment climate mapLightPower 1'));
  assert.ok(h.sent.some(s=>s==='mirv_filmmaker environment climate ambient 1'));
  for(const [key,value] of [['sunAngle',.53],['lampAngle',1],['shadowSoftness',1],['bounceSteps',1]])
    assert.ok(h.sent.includes(`mirv_filmmaker environment climate ${key} ${value}`));
  // The reset RELEASES every lighting switch (2026-10-05): relighting off, baked lighting back.
  // The unbaked fill's default may be restored, but it only acts while baked lighting is
  // suppressed, so nothing here can leave the scene on fill -- and nothing switches relighting ON.
  assert.ok(h.sent.includes('mirv_filmmaker environment climate bakedSuppressed 0'));
  assert.ok(h.sent.includes('mirv_filmmaker environment climate relighting 0'));
  assert.ok(!h.sent.some(s=>/climate (?:bakedSuppressed|relighting|relight) (?!0(?:\b|$))/.test(s)),
    'the reset must never enable relighting or baked-light suppression');
});
test('build banner lights up while map cache is preparing',()=>{
  const h=harness();h.render();
  assert.equal(h.context.envPrepBanner.visible,false);
  h.context.st.envBuildBusy=true;h.context.st.envBuildStatus='Preparing map: outdoor weather and shadows (12s elapsed)';
  h.context.renderHooks.forEach(fn=>fn(h.context.st));
  assert.equal(h.context.envPrepBanner.visible,true);
  assert.match(String(h.context.envPrepText.text),/Preparing map/);
});
test('map data reload sends only the reload command',()=>{
  const h=harness();h.render();
  h.context.envPresetOpen(); // the scene preset menu carries it now
  h.press('Reload map geometry');
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment reload-geometry');
  assert.doesNotMatch(fragment,/prepare-geometry|recache-geometry/);
});
test('map prep stays on the Environment tab',()=>{
  const js=readOwn('ConfigHudJs.h');
  assert.doesNotMatch(js,/envPrepHud/,'viewport overlay must stay gone');
  assert.doesNotMatch(js,/envPrepBanner/,'all-tab inspector banner must stay gone');
  assert.match(nav,/mk\('Panel', pageEnvironment\)/);
});
test('scene names use keyboard bridge and preserve spaces',()=>{
  const h=harness();h.render();h.context.envPresetOpen();h.context.envNameField.onactivate();
  h.context.configWindowChars([...Buffer.from('Night Shoot')].join(','));
  h.context.configWindowChars('13');
  assert.equal(h.sent.at(-1),'mirv_filmmaker environment save "Night Shoot"');
  assert.equal(h.context.root.textcapture,'0');
});
test('material fields reject edits while simple shading is selected',()=>{
  const h=harness();h.render([{...prop,materialEnabled:false}],14);
  const n=h.sent.length;h.slider('Roughness').opts.onChange(.7);assert.equal(h.sent.length,n);
});

// Weather, fog, sky or grading switch the Environment pass on, but only the Lighting switch
// (climate relighting) may take the map's own lighting away: rain must not relight the map.
test('only the Lighting switch takes over the map lighting', () => {
  const scene = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Environment/EnvironmentScene.cpp', import.meta.url), 'utf8');
  assert.match(scene, /const bool relightingOn=active && climate\[CL_relighting\]>\.5f;/);
  assert.match(scene, /const bool ownedLighting=\(lightingReady && \(relightingOn \|\| authoredLighting\.enabled\)\)/);
  assert.doesNotMatch(scene, /lightingReady && \(active \|\| authoredLighting\.enabled\)/);
  const climate = read('ConfigHudClimateJs.h');
  assert.match(climate, /envLightSec\.__note\(s\.enabled && c\.enabled && c\.relighting \?/);
});

test('round heat map switch drives the heatmap command and shows the round', () => {
  const h = harness();
  h.render();
  // Its own card now (2026-10 sidebar cleanup): the switch is the card's header master.
  const sw = h.card('ROUND HEAT MAP').pill;
  assert.equal(sw.on, false);
  assert.equal(h.sent.length, 0, 'rendering alone changes no state');
  sw.click();
  assert.deepEqual(h.sent, ['mirv_filmmaker heatmap on quiet']);

  h.render([], 0, { heatmap: { on: true, radius: 2000, teams: 'ct', state: 'ready', round: 4, rounds: 18, players: 0, error: '' } });
  assert.equal(sw.on, true);
  assert.equal(h.card('ROUND HEAT MAP').note, 'CT · 2000u');
  assert.equal(h.slider('Area').value, 2000);
  const teams = h.modes.find((m) => m.label === 'Players');
  assert.equal(teams.value, 'ct');
  assert.equal(h.context.hmStatus.text, 'Round 4 of 18. Nobody walked near the camera.');
  h.render([], 0, { heatmap: { on: true, radius: 2000, teams: 'ct', state: 'ready', round: 4, rounds: 18, players: -1, error: '' } });
  assert.equal(h.context.hmStatus.text, 'Round 4 of 18.', 'no "nobody" while the grid is still being built');
  h.render([], 0, { heatmap: { on: true, radius: 1500, teams: 'all', state: 'nodemo', round: 0, rounds: 0, players: -1, error: '' } });
  assert.equal(h.context.hmStatus.text, 'Open a demo to see where its players walked.');
  h.render([], 0, { heatmap: { on: true, radius: 1500, teams: 'all', state: 'warmup', round: 0, rounds: 18, players: 4, error: '' } });
  assert.equal(h.context.hmStatus.text, 'Warmup, before the match started.');
  assert.equal(h.sent.length, 1, 'rendering alone changes no state');

  h.slider('Area').opts.onChange(1200);
  teams.choose('t');
  sw.click();
  assert.deepEqual(h.sent.slice(1),
    ['mirv_filmmaker heatmap radius 1200 quiet', 'mirv_filmmaker heatmap teams t quiet', 'mirv_filmmaker heatmap off quiet']);
});
