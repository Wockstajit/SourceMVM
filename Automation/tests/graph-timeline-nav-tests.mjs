// The TIMELINE dock's navigation model.
//
// The visible time window lives in the dock's JS, not in C++: C++ suggests it once (plus an epoch
// to re-apply it), and after that only the overview navigator, the footer scrollbar, the zoom
// buttons, FIT, Ctrl/Shift+wheel and playback paging move it. Every property row carries its own
// keyframe navigator (<< diamond >>). These are the contracts that must not silently regress.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const graph = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h');
const hud = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorExperimentHud.cpp');
const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');
const cam = read('AfxHookSource2/Filmmaker/Movie/CamPathShake.cpp');
const main = read('AfxHookSource2/main.cpp');

// Execute the shipped navigation functions, with no engine or duplicate implementation.
function navigation(playing) {
  const ctx = vm.createContext({ st: { playhead: 1500, playing, domainT0: 0, domainT1: 10000 },
    num: Number, trackSig: '', graphSig: '', playWasInView: false, wasPlaying: false, manualView: false });
  const viewCode = graph.slice(graph.indexOf('    var view ='), graph.indexOf('    // ---- time snap'));
  const followCode = graph.slice(graph.indexOf('    var playWasInView'), graph.indexOf('    // ---- coordinate maps'));
  vm.runInContext(viewCode + followCode, ctx);
  ctx.view.t0 = 1000; ctx.view.t1 = 2000;
  return ctx;
}

for (const playing of [false, true]) {
  test(`manual navigation passes the playhead without snapping back (playing=${playing})`, () => {
    const n = navigation(playing);
    n.followPlayback();
    n.panTicks(3000);
    for (let frame = 0; frame < 10; frame++) n.followPlayback();
    assert.equal(n.view.t0, 4000);
    assert.equal(n.view.t1, 5000);
    assert.equal(n.st.playhead, 1500);
    n.zoomAt(0.5, 4500);
    n.followPlayback();
    assert.equal(n.view.t0, 4250);
    assert.equal(n.view.t1, 4750);
    assert.equal(n.st.playhead, 1500);
  });
}

test('manual navigation wins on the first playback frame and following resumes on restart', () => {
  const n = navigation(true);
  n.panTicks(3000);
  n.followPlayback();
  assert.equal(n.view.t0, 4000);
  n.st.playing = false; n.followPlayback();
  n.st.playing = true; n.followPlayback();
  assert.ok(n.view.t0 <= 1500 && n.view.t1 >= 1500);
  assert.equal(n.st.playhead, 1500);
  n.st.playhead = n.view.t1 + 100;
  n.followPlayback();
  assert.ok(n.view.t0 <= n.st.playhead && n.view.t1 >= n.st.playhead);
});

test('playback pages AE-style: past the right edge the next page starts at the playhead', () => {
  const n = navigation(true);
  n.followPlayback();                     // the playhead is in view: nothing moves
  assert.equal(n.view.t0, 1000);
  n.st.playhead = 2100;
  n.followPlayback();
  assert.equal(n.view.t0, 2100);
  assert.equal(n.view.t1 - n.view.t0, 1000);
});

test('a jump to an off-screen key lands it 2% inside the view, without zooming', () => {
  const n = navigation(false);
  n.revealTicks(3000, 3000);
  assert.equal(n.view.t1 - n.view.t0, 1000);
  assert.equal(n.view.t1, 3020);
  n.revealTicks(2500, 2500);              // already on screen: no move
  assert.equal(n.view.t1, 3020);
  n.revealTicks(1500, 1500);              // off the left edge: lands 2% inside it
  assert.equal(n.view.t0, 1480);
});

test('the timeline JS owns the visible window; C++ only suggests it once', () => {
  assert.match(graph, /var view = \{ t0: 0, t1: 1, mode: '', epoch: null \}/);
  assert.match(graph, /function seedView\(mode\)/);
  assert.match(graph, /function normalizeView\(\)/);
  // st.viewT0/viewT1 appear in exactly ONE place -- the seed.
  const stateViewRefs = [...graph.matchAll(/st\.viewT[01]/g)].length;
  assert.equal(stateViewRefs, 2, 'st.viewT0/viewT1 may only be read by seedView');
  assert.match(graph, /function tickToX\(t\) \{ return PAD \+ \(t - view\.t0\) \/ \(view\.t1 - view\.t0\) \* G\.cw; \}/);
});

test('C++ publishes a suggestion plus an epoch, and never re-derives it from the playhead', () => {
  assert.match(hud, /,\\"viewT0\\":/);
  assert.match(hud, /,\\"viewEpoch\\":/);
  assert.match(hud, /if \(!m_viewInit\) \{/);
  assert.match(hud, /m_viewInit = true;/);
  // The suggestion spans camera AND scene keys: one timeline.
  assert.match(hud, /SceneKeys_KeySpan\(slo, shi\)/);
  assert.ok((hud.match(/\+\+m_viewEpoch;/g) || []).length >= 3,
    'enter / reseed / clear / demo-change must each bump the epoch');
});

test('the dock follows the design: 440 tall, toolbar, overview over the ruler, rows, footer', () => {
  assert.match(graph, /var DOCK_H = 440, HEADER_H = 72, OVERVIEW_H = 14, RULER_H = 26, ROWS_Y = 112, FOOTER_H = 26;/);
  assert.match(graph, /var LEFTW = 296, PAD = 12, SB_W = 12, ROW_H = 22, VAL_W = 120, NAV_W = 54;/);
  assert.match(graph, /overview\.style\.position = LEFTW \+ 'px ' \+ HEADER_H \+ 'px 0px'/);
  assert.match(graph, /ruler\.style\.position = LEFTW \+ 'px ' \+ \(HEADER_H \+ OVERVIEW_H\) \+ 'px 0px'/);
  assert.match(graph, /function ovViewRect\(\)/);
  // Drag the body / either edge, and click the empty track to jump.
  for (const g of ['ovMove', 'ovL', 'ovR']) assert.ok(graph.includes(g), g + ' gesture missing');
});

test('the footer carries FIT, zoom and a scrollbar whose thumb width is the zoom level', () => {
  assert.match(graph, /var sbThumb = mk\('Panel', sbTrack\)/);
  assert.match(graph, /thumbW = Math\.max\(10, Math\.min\(trackW, \(view\.t1 - view\.t0\) \/ domSpan \* trackW\)\)/);
  assert.match(graph, /var fitBtn = footBtn\('FIT'/);
  assert.match(graph, /footBtn\('−'/);
  assert.match(graph, /footBtn\('\+'/);
  assert.match(graph, /function zoomStep\(f\)/);
});

test('every property row carries << diamond >> keyframe controls', () => {
  assert.match(graph, /function navBtn\(parent, glyph, tip, fn\)/);
  assert.match(graph, /function toggleKeyAt\(n\)/);
  assert.match(graph, /function jumpOn\(n, dir\)/);
  // The diamond is filled when the playhead is on a key.
  assert.match(graph, /setText\(r\.dia\.__lbl, at \? '◆' : '◇'\);/);
  // Keying uses the value you are looking at (camera) or the property's own editable value (scene).
  assert.match(hud, /,\\"cur\\":/);
  // A scene object's grouped row (Position: X/Y/Z) keys each of its channels in one undo step.
  assert.match(graph, /sceneEach\(n, function \(f\) \{ return 'graphadd ' \+ f \+ ' ' \+ at; \}\)/);
  assert.match(graph, /ids\.forEach\(function \(f\) \{ rawCmd\('mirv_filmmaker scenekeys ' \+ verb\(f\)\); \}\);/);
  assert.match(graph, /if \(ids\.length > 1\) rawCmd\('mirv_filmmaker scenekeys editbegin'\);/);
});

test('the visible window is clamped to the demo: no ticks before 0 or past the end', () => {
  assert.match(graph, /var minS = Math\.min\(MIN_SPAN, domSpan\), maxS = domSpan;/);
  assert.match(graph, /if \(view\.t0 < d\.t0\) \{ view\.t1 \+= d\.t0 - view\.t0; view\.t0 = d\.t0; \}/);
  assert.match(graph, /if \(view\.t1 > d\.t1\) \{ view\.t0 -= view\.t1 - d\.t1; view\.t1 = d\.t1; \}/);
});

test('the playhead disappears when it is outside the visible window', () => {
  assert.match(graph, /if \(x >= 0 && x <= G\.tw\) \{/);
  assert.match(graph, /phLine\.visible = false; phHandle\.visible = false;/);
});

test('playback pages the window to keep the playhead on screen; scrubbing does not', () => {
  assert.match(graph, /function followPlayback\(\)/);
  assert.match(graph, /if \(!st\.playing \|\| st\.scrubbing\) \{ playWasInView = inside; wasPlaying = false; return; \}/);
  assert.match(hud, /,\\"playing\\":/);
  assert.match(hud, /bool DemoPlaying\(\)/);
});

test('numbers are typed INSIDE their cell, and the editor is destroyed on commit', () => {
  assert.match(graph, /function beginCellEdit\(r, comp, initial\)/);
  assert.match(graph, /\$\.CreatePanel\('TextEntry', comp\.wrap, 'TimelineType', \{\}\)/);
  assert.match(graph, /function commitEdit\(\)/);
  // Deletion is what drops the keyboard focus grab that was swallowing Space.
  assert.match(graph, /es\.entry\.DeleteAsync\(0\)/);
  assert.doesNotMatch(graph, /\btypeBox\b/);
  // Values are clamped to the property's coded range before they are sent.
  assert.match(graph, /function clampToSpec\(ch, v\)/);
  assert.match(graph, /clampToSpec\(c, v\)\.toFixed\(3\)/);
});

test('the strip value axis is the coded min/max, with no padding past it', () => {
  const sceneKeys = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
  assert.match(sceneKeys, /double lo=s\.min, hi=s\.max;/);
  assert.match(sceneKeys, /j\.DoubleField\("min",lo\); j\.DoubleField\("max",hi\);/);
  assert.doesNotMatch(sceneKeys, /double pad=std::max/);
});

test('scene properties are layers, one per CONFIG card, collapsible', () => {
  // Layers come from the catalog's own family + group, in catalog order.
  assert.match(graph, /var gid = c\.groupId \|\| \(\(c\.family \|\| ''\) \+ '\.' \+ \(c\.group \|\| ''\)\);/);
  assert.match(graph, /function sceneLayerTitle\(c\)/);
  // A collapsed layer emits no rows below it, and shows summary marks instead of its keys.
  assert.match(graph, /if \(expandable\(n\) && isOpen\(n\)\) n\.children\.forEach/);
  assert.match(graph, /if \(!isOpen\(n\)\) drawSummary\(r, ks\);/);
  // Its switch disables (mutes) the layer: keys kept, nothing applied.
  assert.match(graph, /rawCmd\('mirv_filmmaker scenekeys mute ' \+ L\.groupId/);
});

test('GRAPH draws the selected layer, group or property on one shared canvas', () => {
  assert.match(graph, /function graphChans\(\)/);
  // One band for every curve: each scaled to its own range across the full height.
  assert.match(graph, /function band\(\) \{ return \{ y0: GRAPH_TOP, h: Math\.max\(20, ROWS_H - GRAPH_TOP - GRAPH_BOTTOM\) \}; \}/);
  assert.doesNotMatch(graph, /SCENE_BAND_PAD/);
  assert.doesNotMatch(graph, /sceneStrips/);
});

test('time snap: one shared step quantises the playhead, key placement and key drags', () => {
  const bridge = read('AfxHookSource2/Filmmaker/Movie/CameraBridge.cpp');
  const bridgeH = read('AfxHookSource2/Filmmaker/Movie/CameraBridge.h');
  assert.match(graph, /function snapStep\(\)/);
  assert.match(graph, /function niceSnap\(v\)/);
  assert.match(graph, /function snapTick\(t\)/);
  assert.match(graph, /function scrubTick\(tx\) \{ return snapTick\(xToTick\(tx\)\); \}/);
  // Alt bypasses snap for one gesture.
  assert.match(graph, /if \(step <= 0 \|\| \(st && st\.alt\)\) return t;/);
  assert.match(bridge, /bool CameraBridge_GetAltDown\(\)/);
  assert.match(bridgeH, /bool CameraBridge_GetAltDown\(\);/);
  // The playhead scrub uses the snapped tick everywhere it is read.
  assert.match(graph, /playheadTo\(scrubTick\(cc\.tx\)\)/);
  // Key placement and the lead-key drag snap.
  assert.match(graph, /var tick = snapTick\(st\.playhead\);/);
  assert.match(graph, /Math\.round\(snapTick\(st\.playhead\)\)/);
  assert.match(graph, /leadT0: t/);
  assert.match(graph, /dt = snapTick\(g\.leadT0 \+ dt\) - g\.leadT0;/);
  // C++ publishes the shared value and the Alt modifier.
  assert.match(hud, /,\\"snap\\":/);
  assert.match(hud, /editor\.graph\.snap/);
  assert.match(hud, /,\\"alt\\":/);
});

test('the snap dropdown offers Auto, presets, Custom and Off', () => {
  assert.match(graph, /var items = \[\['auto', 'Auto'\], \['1', '1'\], \['10', '10'\], \['100', '100'\], \['1000', '1000'\], \['custom', 'Custom…'\], \['off', 'Off'\]\];/);
  assert.match(graph, /function showSnapMenu\(\)/);
  assert.match(graph, /rawCmd\('mirv_filmmaker settings ui editor\.graph\.snap ' \+ v\)/);
  // GRAPH draws the snap grid while snapping.
  assert.match(graph, /if \(snap > 0 && snap \* pxPerTick >= 4 && \(view\.t1 - view\.t0\) \/ snap <= 140\)/);
});

test('the editing area carries no instructions', () => {
  assert.doesNotMatch(graph, /Click anywhere on this strip/);
  assert.doesNotMatch(graph, /click an empty strip = first key/);
  assert.doesNotMatch(graph, /var footerHint/);
});

test('the wheel is contextual and Ctrl/Shift never reaches the camera FOV', () => {
  assert.match(graph, /if \(st\.ctrl\) \{/);
  assert.match(graph, /if \(st\.shift\) \{ panByFrac/);
  assert.match(graph, /return true;   \/\/ consumed: over the dock the wheel is ours/);
  // MovieMode hands the modifier notch to the workspace hosting the dock (CONFIG or the editor)
  // while the dock is up, before its own free-cam FOV branch.
  assert.match(movie,
    /if \(CameraBridge_GetFreeCamEnabled\(\) && \(ctrlDown \|\| shiftDown\)\) \{\s*if \(GraphEditorExperiment_Enabled\(\)\) \{\s*PushWorkspaceWheel\(delta > 0 \? \+1 : -1\);\s*return true;\s*\}/);
});

// A key's glyph says the interpolation that is STORED on each side, never a guess from the slope:
// a flat Smooth handle and an Easy Ease are the same numbers, and a handle dragged in the graph
// is a plain Bezier from then on (AE's Auto Bezier -> Bezier).
test('key glyphs follow the stored side kind', () => {
  const ctx = vm.createContext({});
  vm.runInContext(graph.slice(graph.indexOf('    function sideOf(h)'), graph.indexOf('    function stepped(ch)')), ctx);
  assert.equal(ctx.sideOf(null), 'lin');
  assert.equal(ctx.sideOf({ a: 0, dv: 0 }), 'lin');
  assert.equal(ctx.sideOf({ a: 1, dv: 0 }), 'ease');           // Easy Ease
  assert.equal(ctx.sideOf({ a: 1, dv: 12 }), 'ease');          // a dragged Bezier is not Smooth
  assert.equal(ctx.sideOf({ a: 1, auto: 1, dv: 0 }), 'auto');  // a flat Smooth key is still Smooth
  assert.equal(ctx.sideOf({ a: 0, auto: 1 }), 'auto');         // an end key's outer side
  assert.match(hud, /\\"auto\\":" << \(k\.cpLeft\.autoTan \? 1 : 0\)/);
  assert.match(hud, /if \(m_model\.RefreshAutoTangents\(\)\) BumpRev\(\);/);
});

test('the transport keeps the Cache toggle from the camera timeline', () => {
  assert.match(graph, /var cacheBtn = btn\(rowTop, 'Cache', function \(\) \{\s*rawCmd\('mirv_filmmaker scrub cache ' \+ \(st && st\.scrubCache \? 'off' : 'on'\)\);/);
  assert.match(graph, /btnSel\(cacheBtn, !!st\.scrubCache\);/);
  assert.match(hud, /",\\"scrubCache\\":" << \(ScrubFrameCache_Enabled\(\) \? "true" : "false"\)/);
});

// Runs the dock's shake model (GraphEditorJs.h) in a sandbox: the core helpers, then the menus.
function shakeSandbox(st, sent) {
  const ctx = vm.createContext({ st, num: Number, snapTick: Math.round, fmtTime: (t) => 't' + t,
    shakeCmd: (owner, tail) => sent.push(owner + ':' + tail) });
  vm.runInContext(graph.slice(graph.indexOf('    var SHAKE_STOP = -2;'), graph.indexOf('    function shakeCmd(owner, tail)')), ctx);
  vm.runInContext(graph.slice(graph.indexOf('    var SHAKE_N0'), graph.indexOf('    function pickShake(')), ctx);
  return ctx;
}

test('Camera Shake lists every shake, each with its own strength and speed submenu', () => {
  const shakes = [{ i: 2, t: 300, s: 301, l: 'Drunk', n: 6, p: 40 }];
  const styles = [[300, 'Hand'], [301, 'Drunk'], [302, 'Explosion'], [303, 'Air Turbulence'], [304, 'Ground Vibration'], [310, 'Hand Soft'], [311, 'Jolt']];
  const ctx = shakeSandbox({ shakes, pathKeys: [100, 200, 300], shakeStyles: styles, shakeEssentials: 5 }, []);
  const items = JSON.parse(JSON.stringify(ctx.shakeItems(0, 300)));   // plain arrays, not the vm realm's
  const styleRows = items.filter((it) => it.key && it.key.indexOf('style:') === 0);
  assert.deepEqual(styleRows.map((it) => it.label), ['Hand', 'Drunk', 'Explosion', 'Air Turbulence', 'Ground Vibration']);
  const drunk = styleRows[1];
  assert.ok(drunk.check && drunk.pick && drunk.keep, 'the shake on the key is ticked, and clicking a shake applies it');
  assert.equal(drunk.id, 'shk:0|set @300 301 6 40');
  const steps = Object.fromEntries(drunk.sub.filter((it) => it.id).map((it) => [it.label, it.id]));
  assert.deepEqual(steps, { Stronger: 'shk:0|set @300 301 7 40', Weaker: 'shk:0|set @300 301 5 40', Faster: 'shk:0|set @300 301 6 50', Slower: 'shk:0|set @300 301 6 30' });
  assert.ok(drunk.sub.every((it) => !it.id || it.keep), 'stepping keeps the menu open on that shake');
  // A shake not on the key starts from the key's current strength and speed.
  assert.equal(styleRows[0].sub.filter((it) => it.label === 'Stronger')[0].id, 'shk:0|set @300 300 7 40');
  const more = items.filter((it) => it.id === 'shkmore')[0];
  assert.deepEqual(more.sub.map((it) => it.label), ['Hand Soft', 'Jolt']);
  assert.ok(more.sub.every((it) => it.sub && it.sub.length), 'the extra shakes nest the same way');
  // Stop, every camera key and remove, addressed by tick on the path's track.
  const ids = items.filter((it) => it.id).map((it) => it.id);
  assert.ok(ids.includes('shk:0|stop @300') && ids.includes('shk:0|applyall @300') && ids.includes('shk:0|clear @300'));
  // A camera object's menu is the same, on its own track, without "every camera key".
  const obj = JSON.parse(JSON.stringify(shakeSandbox({ objShakes: { 7: [] }, shakeStyles: styles, shakeEssentials: 5 }, []).shakeItems(7, 1500)));
  assert.equal(obj.filter((it) => it.key === 'style:301')[0].id, 'shk:7|set @1500 301 5 50');
  assert.ok(!obj.some((it) => it.id && it.id.indexOf('applyall') >= 0));
  assert.match(graph, /function pickShake\(id\) \{[\s\S]{0,200}shakeCmd\(\+id\.substring\(4, bar\), id\.substring\(bar \+ 1\)\);/);
});

test('a shaken Camera shake row twirls open to Intensity and Speed, on the path and on a camera object', () => {
  assert.match(graph, /\{ id: id \+ '\.n', kind: 'shakep', owner: owner, param: 'n', verb: 'intensity', name: 'Intensity', min: 1, max: 10,/);
  assert.match(graph, /\{ id: id \+ '\.p', kind: 'shakep', owner: owner, param: 'p', verb: 'speed', name: 'Speed', min: 10, max: 100,/);
  assert.match(graph, /pg\.children\.push\(shakeNode\('cam\.shake', 0, 'cam', cam\)\);/);
  // Every camera object gets an OBJECTS layer with the same row, before it has pose keys too.
  assert.match(graph, /\(st\.shakeCams \|\| \[\]\)\.forEach\(function \(sc\) \{[\s\S]{0,600}L\.children\.push\(shakeNode\('o' \+ sc\.id \+ '\.shake', sc\.id, 'scene', L\)\);/);
  assert.match(hud, /CamPathShake_AppendDockJson\(o\);/);
  assert.match(hud, /if \(obj\.kind != "camera"\) continue;/);
  assert.match(graph, /if \(n\.kind === 'shake'\) return n\.children\.length > 0 && openState\[n\.id\] !== false;/);
  assert.match(graph, /if \(k && k\.s >= 0\) shakeCmd\(owner, node\.verb \+ ' ' \+ shakeRef\(t\) \+ ' ' \+ v\);\s*else shakeKeyFrom\(owner, t, node\.param, v\);/);
  // The values redraw when a shake's numbers change, not only when one is added.
  assert.match(graph, /JSON\.stringify\(st\.shakes \|\| \[\]\) \+ JSON\.stringify\(st\.objShakes \|\| \{\}\) \+ Object\.keys\(shakeSel\)\.join\(','\)/);
});

// Shake strength and speed are keyframes: a shot can start slow and gentle and build up. Between
// two keys of the same shake the value glides along the SAME eased ramp the camera plays
// (CamPathShakeRampMath.h). A key runs until the next key of its track, or to the end of the shot:
// camera keys without a shake play no part (they used to end it, so a shake stopped at the path's
// second key however many keys were added, 2026-10-17).
test('shake values ramp between keys and run until the next key, or to the end', () => {
  const keys = [100, 200, 300, 400, 500, 600];
  const shakes = [{ i: 0, t: 100, s: 301, n: 2, p: 20 }, { i: 2, t: 300, s: 301, n: 8, p: 80 }, { i: 3, t: 400, s: 302, n: 5, p: 50, o: 1 }];
  const sent = [];
  const ctx = shakeSandbox({ shakes, pathKeys: keys, shakeStyles: [[301, 'Drunk']], playhead: 150 }, sent);
  ctx.SHAKE_N0 = 5; ctx.SHAKE_P0 = 50;
  assert.equal(ctx.shakeValueAt(0, 'n', 100), 2);
  assert.equal(ctx.shakeValueAt(0, 'n', 200), 5);              // half way, ACROSS the camera key with no shake
  assert.ok(ctx.shakeValueAt(0, 'p', 110) < 20 + 60 * 0.05);  // eased: it leaves the key gently
  assert.equal(ctx.shakeValueAt(0, 'n', 350), 8);              // the run's last key holds until the next key
  assert.equal(ctx.shakeValueAt(0, 'n', 450), 5);              // a one-shot runs from its key
  assert.equal(ctx.shakeValueAt(0, 'n', 590), 5);              // past camera keys without a shake: still running
  assert.equal(ctx.shakeValueAt(0, 'n', 50), null);            // nothing runs before the first key
  shakes.push({ i: 4, t: 500, s: -2, n: 5, p: 50 });           // a Stop ends it
  assert.equal(ctx.shakeValueAt(0, 'n', 520), null);
  ctx.shakeParamSet({ param: 'n', verb: 'intensity', min: 1, max: 10 }, 200, 6);   // camera key 1 has no shake yet
  assert.deepEqual(sent, ['0:set @200 301 6 50']);              // it joins the run, at the speed it reads there
  assert.equal(ctx.shakeCanKey(0, 150), false, 'a path shake key sits on a camera key');
  // A camera object's track: keys on any tick, the same rules.
  const objSent = [];
  const obj = shakeSandbox({ objShakes: { 7: [{ t: 1000, s: 301, n: 3, p: 30 }] }, shakeStyles: [[301, 'Drunk']], playhead: 2000 }, objSent);
  obj.SHAKE_N0 = 5; obj.SHAKE_P0 = 50;
  assert.equal(obj.shakeValueAt(7, 'n', 5000), 3);
  assert.equal(obj.shakeCanKey(7, 150), true);
  obj.shakeParamSet({ param: 'n', verb: 'intensity', owner: 7, min: 1, max: 10 }, 2000, 9);
  assert.deepEqual(objSent, ['7:set @2000 301 9 30']);
  // The native evaluator plays the same rule.
  assert.match(cam, /ShakeRamp::Value\(intensity, \(double\)keys\[\(size_t\)sk\[\(size_t\)j \+ 1\]\]\.intensity, u\)/);
  assert.match(cam, /clock\[\(size_t\)j\]\.rampsToNext = k\.Active\(\) && j \+ 1 < m && keys\[\(size_t\)sk\[\(size_t\)j \+ 1\]\]\.style == k\.style && !oneShot\(j\);/);
  assert.match(cam, /auto endOf = \[&\]\(int j\) \{ return j \+ 1 < m \? clock\[\(size_t\)j \+ 1\]\.tick : 1\.0e300; \};/);
  assert.match(cam, /ShakeRamp::ClockSeconds\(clock\.data\(\), m, runStartOf\(j\), lookupTick, spt\)/);
  // A looked-through camera object takes its own track at the scene keys' tick.
  assert.match(cam, /if \(owner\) lookupTick = SceneKeys_AppliedTick\(\);/);
  assert.match(main, /CamPathShake_ApplyToView\(Tx, Ty, Tz, Rx, Ry, Rz, Fov,\s*owner\.sceneCamera \? Filmmaker::SceneRig_LookThrough\(\) : 0\)/);
});

test('menus nest to any depth and a keep-open step reopens the same submenus', () => {
  assert.match(graph, /function openSub\(L, r\) \{/);
  assert.match(graph, /menu\.levels\.push\(buildMenuPanel\(x, r\.y0 - 6, W, r\.it\.sub, L \+ 1\)\);/);
  assert.match(graph, /reopenMenu = \{ frames: 3, x: x, y: y, w: w, rebuild: m\.rebuild, onPick: pick, path: path \};/);
  assert.match(graph, /if \(itemKey\(m\.rects\[i\]\.it\) === r\.path\[L\]\)/);
  assert.match(graph, /\}, build\);/);   // the key menu rebuilds after a shake step
});

// The ease half is real geometry. A notch painted in the background colour showed as a black
// cut-out on any highlighted row and under a box selection.
test('ease glyph halves are drawn without a background-coloured notch', () => {
  const glyphCode = graph.slice(graph.indexOf('    function triPiece('), graph.indexOf('    // A whole glyph.'));
  assert.doesNotMatch(glyphCode, /notch/);
  assert.doesNotMatch(glyphCode, /bg \|\| S\.panelHex/);
  assert.match(glyphCode, /triPiece\(hb, \[ox, 0\], \[ax, h\], \[ax, 0\], color\);/);
  // Run the triangle maths: the filled side must be the one holding the right angle.
  const made = [];
  const mk = () => { const p = { style: {} }; made.push(p); return p; };
  const ctx = vm.createContext({ mk, Math });
  vm.runInContext(glyphCode.slice(0, glyphCode.indexOf('    function glyphHalf(')), ctx);
  for (const [p1, p2, c] of [[[0, 0], [3, 5], [3, 0]], [[0, 10], [3, 5], [3, 10]], [[5, 0], [2, 5], [2, 0]], [[5, 10], [2, 5], [2, 10]]]) {
    made.length = 0;
    ctx.triPiece({}, p1, p2, c, '#fff');
    const f = made[1], deg = parseFloat(/rotateZ\((-?[\d.]+)deg\)/.exec(f.style.transform)[1]) * Math.PI / 180;
    const [ox, oy] = f.style.position.split(' ').map(parseFloat);
    const box = made[0].style.position.split(' ').map(parseFloat);
    // C relative to the panel's rotated top-left corner, in the panel's own axes: inside means y > 0.
    const vx = c[0] - box[0] - ox, vy = c[1] - box[1] - oy;
    assert.ok(-Math.sin(deg) * vx + Math.cos(deg) * vy > -1e-9, 'triangle ' + JSON.stringify([p1, p2, c]) + ' fills its right-angle side');
  }
});

test('shake keys select like keys: click, Shift, box, Delete; GRAPH drags the value', () => {
  assert.match(graph, /var shakeSel = \{\};/);
  assert.match(graph, /function shakeSelKey\(owner, t\) \{ return \(owner \|\| 0\) \+ ':' \+ Math\.round\(t\); \}/);
  assert.match(graph, /pickedShakes = g\.graph \? \[\] : shakesInRect\(g\)/);
  assert.match(graph, /Object\.keys\(shakeSel\)\.forEach\(function \(key\) \{ var p = key\.split\(':'\); shakeCmd\(\+p\[0\], 'clear @' \+ p\[1\]\); \}\);/);
  // The KEYFRAMES rows only mark the keys (no value lines inside them, user request 2026-10-17);
  // GRAPH draws Intensity and Speed as curves when a shake row or the Path group is selected.
  assert.doesNotMatch(graph, /'#80bdff99'/);
  assert.match(graph, /picked \? S\.accent : \(s\.s < 0 \? S\.dim : '#80bdffff'\)/);
  assert.match(graph, /if \(n\.id === 'cam\.path'\) return \{ owner: 0, only: null \};/);
  assert.match(graph, /var sg = shakeGraphOf\(\);\s*if \(sg\) drawShakeGraph\(sg\); else shakeGraphHits = \[\];/);
  assert.match(graph, /gesture = \{ type: 'shakeGraph', owner: sh\.owner, t: sh\.t, P: sh\.P,/);
  assert.match(graph, /if \(gv !== g\.last\) \{ g\.last = gv; shakeCmd\(g\.owner, gp\.verb \+ ' ' \+ shakeRef\(g\.t\) \+ ' ' \+ gv\); \}/);
});

// The strips above the rows and the GRAPH panel run to the dock's right edge, over the row
// scrollbar's column: stopping at the track width left a corner gap, and in GRAPH the keyframe
// rows showed through that column.
test('overview, ruler and graph span the scrollbar column', () => {
  assert.match(graph, /G = \{ rw: dockW, tw: tw, ow: tw \+ SB_W,/);
  assert.match(graph, /overview\.style\.width = G\.ow \+ 'px'/);
  assert.match(graph, /ruler\.style\.width = G\.ow \+ 'px'/);
  assert.match(graph, /graph\.style\.width = G\.ow \+ 'px'/);
  assert.match(graph, /function domToX\(t\) \{ var d = domainRange\(\); return \(t - d\.t0\) \/ Math\.max\(1, d\.t1 - d\.t0\) \* G\.ow; \}/);
  assert.match(graph, /rowsSbThumb\.style\.zIndex = '6';/);
});
