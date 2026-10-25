import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

// Customize Player: one target source for CONFIG and the Camera Editor, every player (dead ones too)
// in a title-bar menu, and a window that moves and shrinks instead of covering the game.
const read = (p) => fs.readFileSync(new URL('../../' + p, import.meta.url), 'utf8');
const P = 'AfxHookSource2/Filmmaker/';
const customize = read(P + 'Panorama/CameraEditorCustomizeJs.h');
const players = read(P + 'Panorama/CameraEditorCustomizePlayersJs.h');
const windowJs = read(P + 'Panorama/CameraEditorCustomizeWindowJs.h');
const preview = read(P + 'Panorama/CameraEditorCustomizePreviewJs.h');
const editorJs = read(P + 'Panorama/CameraEditorJs.h');
const editorHud = read(P + 'Panorama/CameraEditorHud.cpp');
const configJs = read(P + 'Panorama/ConfigHudJs.h');
const configHud = read(P + 'Panorama/ConfigHud.cpp');
const timelineJs = read(P + 'Panorama/CameraTimelineJs.h');
const timelineHud = read(P + 'Panorama/CameraTimelineHud.cpp');
const target = read(P + 'Cosmetics/CustomizeTarget.cpp');
const uiQueries = read(P + 'Cosmetics/CosmeticUiQueries.cpp');
const inputHooks = read('AfxHookSource2/WindowInputHooks.cpp');

function functionSource(source, name) {
  const start = source.indexOf('function ' + name + '(');
  assert.ok(start >= 0, name + ' must exist');
  let index = source.indexOf('{', start), depth = 0;
  for (; index < source.length; index++) {
    if (source[index] === '{') depth++;
    if (source[index] === '}' && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error('Incomplete function ' + name);
}

test('CS2 own demo bar names the spectated player, in first and third person alike', () => {
  // GetDemoControllerState / GetDemoPlayers / SpectatePlayer live on the HudDemoController panel.
  assert.match(timelineJs, /findNative\('HudDemoController'\)/);
  assert.match(timelineJs, /dc\.GetDemoControllerState\(\)/);
  assert.match(timelineJs, /s\.nSpectatingPlayerId \| 0\) \+ ' ' \+ \(s\.nObserverMode \| 0\)/);
  assert.match(timelineJs, /p\.nEntIndex \| 0\) \+ ',' \+ \(p\.nPawnEntIndex \| 0\) \+ ',' \+ \(p\.bAlive \? 1 : 0\)/);
  assert.match(timelineJs, /root\.SetAttributeString\('specstate', spec\)/);
  assert.match(timelineJs, /root\.SetAttributeString\('demoplayers', players\)/);
  assert.match(timelineJs, /dc\.SpectatePlayer\(controller \| 0\)/);
  assert.match(timelineHud, /MakeSymbol\("specstate"\)/);
  assert.match(timelineHud, /MakeSymbol\("demoplayers"\)/);
  assert.match(timelineHud, /\$\.CamTimeline\.spectate\(/);
});

test('one target source feeds both hosts; no fuzzy nearest-player guess is left', () => {
  assert.match(target, /CameraTimeline_NativeDemoPlayers\(\)/);
  assert.match(target, /CameraTimeline_NativeSpectatedPawn\(\)/);
  // The eye match is only the fallback for a demo whose bar does not answer, and only onto a living
  // roster player: it also lands on dead bodies and on observer pawns at the spectated eye.
  assert.match(target, /No demo bar: the render-eye match, and only onto a LIVING roster player/);
  assert.match(target, /if \(p\.alive && eye >= 0 && p\.pawn == eye\) return p\.controller;/);
  assert.match(editorHud, /o << ",\\"customize\\":" << CustomizeTarget_StateJson\(\);/);
  assert.match(configHud, /customizeReady\\":" << \(CustomizeTarget_Available\(\)/);
  assert.doesNotMatch(configHud, /povReady/);
  assert.doesNotMatch(configJs, /povReady/);
  assert.doesNotMatch(editorHud, /BuildCustomizePlayersJson|customizeTarget\\"|obsTarget/);
  assert.doesNotMatch(uiQueries, /BuildCustomizePlayersJson/);
  assert.doesNotMatch(editorJs, /st\.customizeTarget|st\.customizePlayers|cand\[0\]\.index/);
});

test('CONFIG opens the same modal over itself, and both hosts run the same frame pass', () => {
  assert.match(configJs, /cmd\('mirv_filmmaker config customize'\)/);
  assert.match(configJs, /customizeAction\.hittest = !!st\.customizeReady;/);
  assert.match(configHud, /CameraEditor_OpenCustomizeOverConfig\(\);/);
  assert.match(editorJs, /var custOverConfig = st\.customizeHost === 'config' && !!st\.cursor;/);
  assert.match(editorJs, /if \(!custOverConfig\) closeCustomize\(\);/);
  // customizeFrame() is the one per-frame pass: once in the editor's render, once for CONFIG.
  assert.equal((editorJs.match(/customizeFrame\(\);/g) || []).length, 2);
  assert.match(players, /function customizeFrame\(\)/);
  // A press belongs to the modal while it is up: no CONFIG slider or gizmo underneath may claim it,
  // and typing goes to the modal before any CONFIG search box.
  assert.match(configHud, /if \(CameraEditor_CustomizeModalOpen\(\)\) clmb = false;/);
  const charAt = inputHooks.indexOf('case WM_CHAR:');
  const charCase = inputHooks.slice(charAt, inputHooks.indexOf('case WM_KEYDOWN:', charAt));
  assert.ok(charCase.indexOf('CameraEditor_CustomizeModalOpen()') < charCase.indexOf('ConfigHud_TextCaptureActive()'));
});

test('the CONFIG host never feeds a dying editor root', () => {
  // OnExit's Teardown deletes the old roots asynchronously; building in that frame found the old one.
  assert.match(editorHud, /else if \(!m_enabled && m_wasEnabled\) \{ OnExit\(\); m_wasEnabled = false; exitedThisFrame = true; \}/);
  assert.match(editorHud, /if \(!exitedThisFrame\) RunCustomizeOverConfig\(\);/);
  const run = editorHud.slice(editorHud.indexOf('void CameraEditorHud::RunCustomizeOverConfig()'));
  assert.match(run, /if \(root != m_root\) m_lastState\.clear\(\);/);
});

test('every player is in the title-bar menu; dead ones are dimmed, and picking switches the view', () => {
  assert.match(customize, /var custPlayerBtn = mk\('Panel', custTitleWrap\)/);
  assert.match(customize, /custPlayerBtn\.SetPanelEvent\('onactivate', function \(\) \{ custSetPlayerMenu\(!custPlayerMenuOpen\); \}\);/);
  assert.match(players, /var CUST_DEAD_OPACITY = '0\.45';/);
  assert.match(functionSource(players, 'paintPlayerRow'), /r\.style\.opacity = \(p && p\.alive\) \? '1' : CUST_DEAD_OPACITY;/);
  assert.match(functionSource(players, 'custPickPlayer'), /cmd\('mirv_filmmaker cosmetics pick ' \+ controller\)/);
  // Full names: the menu column grows instead of cutting a name off.
  assert.doesNotMatch(functionSource(players, 'custPlayerRow'), /textOverflow/);
  assert.match(target, /MovieModeRef\(\)\.SetMode\(MovieMode::Mode::Default\);/);
  assert.match(target, /CameraTimeline_RequestNativeSpectate\(controller\);/);
  // CS2 refuses to spectate the dead: their first person is the free cam on the body's eye.
  assert.match(target, /e->GetRenderEyeOrigin\(o\); e->GetRenderEyeAngles\(a\);/);
  assert.match(target, /CameraBridge_SetFreeCamEnabled\(true\);/);
});

test('a dead player keeps their place and their last loadout', () => {
  assert.match(target, /m_hPlayerPawn/);
  assert.match(target, /m_hOriginalController/);
  assert.match(target, /if \(hasGun && g_seekSettleFrames == 0\) g_lastAlive\[p\.key\] = json;/);
  assert.match(target, /else if \(p\.pawn >= 0\) json = BuildCustomizeTargetJson\(p\.pawn\); \/\/ the body: gloves and agent/);
});

test('a click anywhere on the window or the preview never closes it', () => {
  assert.doesNotMatch(customize, /custOverlay\.SetPanelEvent\('onactivate', function \(\) \{ closeCustomize\(\); \}\)/);
  assert.match(customize, /custOverlay\.style\.backgroundColor = 'rgba\(0,0,0,0\)';/);
  // Unhandled clicks bubble: the preview's catcher has to stop its own.
  assert.match(preview, /previewDragCatcher\.SetPanelEvent\('onactivate', function \(\) \{\}\);/);
});

test('the window keeps its title bar on screen wherever it is dragged', () => {
  const ctx = {};
  vm.runInNewContext(functionSource(windowJs, 'custClampPos') + '; out = custClampPos;', ctx);
  const clamp = ctx.out;
  assert.deepEqual({ ...clamp(100, 50, 800, 1920, 1080, 56) }, { x: 100, y: 50 });
  assert.deepEqual({ ...clamp(-40, -30, 800, 1920, 1080, 56) }, { x: 0, y: 0 });
  // Right edge: the whole header (Apply, close) stays visible.
  assert.deepEqual({ ...clamp(1500, 10, 800, 1920, 1080, 56) }, { x: 1120, y: 10 });
  // The body may hang below the screen, the 56px title bar may not.
  assert.deepEqual({ ...clamp(10, 1070, 1240, 1920, 1080, 56) }, { x: 10, y: 1024 });
  // Before the screen has a size there is nothing to clamp against.
  assert.deepEqual({ ...clamp(5, 6, 800, 0, 0, 56) }, { x: 5, y: 6 });
});

test('three window sizes, and only full size keeps the 3D preview', () => {
  assert.match(windowJs, /var CUST_FULL_W = 1240, CUST_COMPACT_W = 800, CUST_FULL_H = 720, CUST_HEAD_H = 56;/);
  const apply = functionSource(windowJs, 'custApplySize');
  assert.match(apply, /prevWrap\.visible = full;/);
  assert.match(apply, /if \(!full\) \{ destroyPreview3d\(\); custHoldChip\.visible = false; \}/);
  assert.match(functionSource(windowJs, 'custShowsPreview'), /custOverlay\.visible && custSize === 'full'/);
  for (const fn of ['maintainPreview', 'recreatePreview', 'positionPreview3d'])
    assert.match(functionSource(preview, fn), /custShowsPreview\(\)/, fn + ' must respect the window size');
  // Drag by the title, never by the player button inside it.
  assert.match(functionSource(windowJs, 'custWindowFrame'), /!custInRect\(custPlayerBtn, mx, my\)/);
});

test('position and size last the session, through rebuilds and either host', () => {
  assert.match(functionSource(windowJs, 'custPublishLayout'), /root\.SetAttributeString\('custlayout', v\)/);
  assert.match(editorHud, /m_symCustomizeLayout = m_bridge\.MakeSymbol\("custlayout"\);/);
  assert.match(editorHud, /o << ",\\"customizeLayout\\":\\"" << m_customizeLayout << "\\"";/);
  assert.match(customize, /custRestoreLayout\(\);/);
});

test('a dead player shows the loadout they last had alive, never one read off a corpse', () => {
  // Only a pawn with health keeps the last-alive copy: a body's weapons are already dropped.
  assert.match(target, /if \(p\.alive && p\.pawn >= 0 && IsAlivePawn\(Ent\(p\.pawn\)\)\) \{/);
  // The demo's first frames hand out no guns: a knife-only read never becomes the saved copy.
  assert.match(target, /const bool hasGun = json\.find\("\\"primary\\":\{"\) != std::string::npos/);
  // A seek back lands before the lives the copies came from; any jump waits for the pawns to settle.
  const seek = functionSource(target.replace(/void TrackSeeks\(\)/, 'function TrackSeeks()'), 'TrackSeeks');
  assert.match(seek, /const bool back = tick < g_lastTick;/);
  assert.match(seek, /if \(back\) g_lastAlive\.clear\(\);\s*g_seekSettleFrames = kSeekSettleFrames;/);
  assert.match(target, /TrackSeeks\(\);\s*BuildRoster\(\);/);
});

test('opened from CONFIG, the modal counts as open a frame before it draws', () => {
  // The paused 3D preview arms in the panel's ready hook, which reads CameraEditor_CustomizeModalOpen():
  // the flag has to be up the frame BEFORE openCustomize() creates the preview, or it renders black.
  const arm = editorHud.slice(editorHud.indexOf('void CameraEditorHud::OpenCustomizeWhenArmed()'));
  assert.match(arm, /if \(!m_customizeOpenArmed\) \{ m_customizeOpenArmed = true; return; \}[\s\S]*?openCustomize\(\);/);
  assert.match(editorHud, /m_customizeOpen = m_customizeOpenArmed\s*\|\|/);
});

test('decorating a slot list reads the selected option once, not once per option', () => {
  // Per option it re-filtered the whole catalog: quadratic, 77-120 ms for a knife list on a pick.
  const opts = [];
  for (let i = 0; i < 600; i++) opts.push(['Knife ' + i, i + ':0', { paint: i % 2 }]);
  const run = (sel) => {
    const ctx = { calls: 0, opts };
    vm.runInNewContext([
      'var custWear = { knife: { preset: "fn" } }, custSel = { knife: "' + sel + '" };',
      'function filteredOptions() { calls++; return opts; }',
      'function selOpt(slot, v) { var a = filteredOptions(slot); for (var i = 0; i < a.length; i++) if (a[i][1] === v) return a[i]; return a[0]; }',
      'function wearPresetLabel() { return "Factory New"; }',
      functionSource(customize, 'wearSuffix'),
      functionSource(customize, 'withWearLabel'),
      functionSource(customize, 'decoratedOptions'),
      'out = decoratedOptions("knife");'
    ].join('\n'), ctx);
    return ctx;
  };
  const painted = run('1:0');
  assert.equal(painted.out.length, 600);
  assert.equal(painted.out[3][0], 'Knife 3 · Factory New');
  assert.ok(painted.calls <= 2, 'filteredOptions ran ' + painted.calls + ' times for one list');
  // A plain (unpainted) selection adds no wear to any label.
  assert.equal(run('0:0').out[3][0], 'Knife 3');
});

test('a player switch spreads its work over frames instead of stalling one', () => {
  // The visible rows of tiles are built at once, the rest a few per frame.
  assert.match(customize, /var ROWS_FIRST = 9, ROWS_PER_FRAME = 6;/);
  assert.match(functionSource(customize, 'rebuildRows'), /pumpRows\(ROWS_FIRST\);/);
  assert.match(functionSource(customize, 'custScrollMaintain'), /pumpRec\.pumpRows\(\);/);
  // The 3D preview follows on the next frame, not on the frame that rebuilt the lists.
  const frame = functionSource(players, 'customizeFrame');
  const reseed = frame.slice(frame.indexOf("previewLog('reseed'"), frame.indexOf('custDisplayedLoadoutSig = nextLoadoutSig;'));
  assert.match(reseed, /custPreviewDue = true;/);
  assert.doesNotMatch(reseed, /updatePreview\(\)/);
  assert.match(frame, /if \(custPreviewDue\) \{\s*custPreviewDue = false;\s*if \(custOverlay\.visible\) \{ updatePreview\(\); pokePreviewSoon\(\); \}/);
});

test('every raw-string chunk of the Customize scripts stays under the MSVC literal limit', () => {
  for (const [name, src] of [['customize', customize], ['players', players], ['window', windowJs], ['preview', preview]]) {
    const chunks = src.split('R"EDJS(').slice(1).map((c) => c.slice(0, c.indexOf(')EDJS"')));
    assert.ok(chunks.length > 0, name);
    for (const c of chunks) assert.ok(Buffer.byteLength(c, 'utf8') < 16380, name + ' chunk of ' + Buffer.byteLength(c, 'utf8') + ' bytes');
  }
});
