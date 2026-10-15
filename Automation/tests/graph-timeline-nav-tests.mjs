// The Scene Graph timeline's navigation model.
//
// The redesign moved the visible time window OUT of C++ and into the graph JS, added an AE-style
// overview navigator above the ruler and a bottom scrollbar, and gave every property row its own
// keyframe navigator (<< diamond >>). These are the contracts that must not silently regress:
// the playhead may never move the view, and the only ways to navigate time are the navigator, the
// scrollbar, the zoom buttons and Ctrl/Shift+wheel.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const graph = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h');
const hud = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorExperimentHud.cpp');
const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');

// Execute the shipped navigation functions, with no engine or duplicate implementation.
function navigation(playing) {
  const ctx = vm.createContext({ st: { playhead: 1500, playing, domainT0: 0, domainT1: 10000 },
    num: Number, lastSig: '', playWasInView: false, wasPlaying: false, manualView: false });
  const viewCode = graph.slice(graph.indexOf('    var view ='), graph.indexOf('    // While the demo PLAYS'));
  const followCode = graph.slice(graph.indexOf('    function followPlayback()'), graph.indexOf('    // ---- coordinate maps'));
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

test('the graph JS owns the visible window; C++ only suggests it once', () => {
  assert.match(graph, /var view = \{ t0: 0, t1: 1, mode: '', epoch: null \}/);
  assert.match(graph, /function seedView\(mode\)/);
  assert.match(graph, /function normalizeView\(\)/);
  // Every coordinate map reads the JS window, not the state feed. st.viewT0/viewT1 must appear in
  // exactly ONE place -- the seed.
  const stateViewRefs = [...graph.matchAll(/st\.viewT[01]/g)].length;
  assert.equal(stateViewRefs, 2, 'st.viewT0/viewT1 may only be read by seedView');
  assert.match(graph, /tickToX\(t\) \{ return \(t - view\.t0\) \/ \(view\.t1 - view\.t0\) \* G\.cw; \}/);
});

test('C++ publishes a suggestion plus an epoch, and never re-derives it from the playhead', () => {
  assert.match(hud, /,\\"viewT0\\":/);
  assert.match(hud, /,\\"viewEpoch\\":/);
  assert.match(hud, /if \(!m_viewInit\) \{/);
  assert.match(hud, /m_viewInit = true;/);
  // The age-out: re-seeding is requested by bumping the epoch, never by following the playhead.
  assert.ok((hud.match(/\+\+m_viewEpoch;/g) || []).length >= 3,
    'enter / reseed / clear / demo-change must each bump the epoch');
});

test('the overview navigator sits above the ruler and is the primary time navigation', () => {
  assert.match(graph, /var LEFTW = 320, HEADER_H = 64, OVERVIEW_H = 30, RULER_H = 26, FOOTER_H = 30;/);
  assert.match(graph, /var ovY = HEADER_H, rulerY = HEADER_H \+ OVERVIEW_H/);
  assert.match(graph, /var overview = mk\('Panel', dock\)/);
  assert.match(graph, /function ovViewRect\(\)/);
  // Drag the body / either edge, and click the empty track to jump.
  for (const g of ['ovMove', 'ovL', 'ovR']) assert.ok(graph.includes(g), g + ' gesture missing');
});

test('the bottom bar is a scrollbar whose thumb width is the zoom level, plus -/+', () => {
  assert.match(graph, /var sbThumb = mk\('Panel', sbTrack\)/);
  assert.match(graph, /thumbW = Math\.max\(22, Math\.min\(trackW, \(view\.t1 - view\.t0\) \/ domSpan \* trackW\)\)/);
  assert.match(graph, /zoomBtn\('−'/);
  assert.match(graph, /zoomBtn\('\+'/);
  assert.match(graph, /function zoomStep\(f\)/);
});

test('every property row carries << diamond >> keyframe controls', () => {
  assert.match(graph, /function navBtn\(parent, glyph, tip, fn\)/);
  assert.match(graph, /function toggleChannelKey\(c\)/);
  assert.match(graph, /function jumpChannelKey\(c, dir\)/);
  assert.match(graph, /function toggleSceneKey\(ch\)/);
  assert.match(graph, /function jumpSceneKey\(ch, dir\)/);
  // The diamond is filled when the playhead is on a key.
  assert.match(graph, /r\.dia\.__lbl\.text = at \? '◆' : '◇';/);
  assert.match(graph, /n\.dia\.__lbl\.text = at \? '◆' : '◇';/);
  // Keying uses the value you are looking at (camera) or the strip's own editable value (scene).
  assert.match(hud, /,\\"cur\\":/);
  assert.match(graph, /rawCmd\('mirv_filmmaker scenekeys graphadd ' \+ ch\.fieldId/);
});

test('the visible window is clamped to the demo: no ticks before 0 or past the end', () => {
  assert.match(graph, /var minS = Math\.min\(MIN_SPAN, domSpan\), maxS = domSpan;/);
  assert.match(graph, /if \(view\.t0 < d\.t0\) \{ view\.t1 \+= d\.t0 - view\.t0; view\.t0 = d\.t0; \}/);
  assert.match(graph, /if \(view\.t1 > d\.t1\) \{ view\.t0 -= view\.t1 - d\.t1; view\.t1 = d\.t1; \}/);
});

test('the playhead disappears when it is outside the visible window', () => {
  assert.match(graph, /if \(phx >= 0 && phx <= G\.cw\) \{/);
  assert.match(graph, /phLine\.visible = false; phHandle\.visible = false;/);
});

test('playback pages the window to keep the playhead on screen; scrubbing does not', () => {
  assert.match(graph, /function followPlayback\(\)/);
  assert.match(graph, /if \(!st\.playing \|\| st\.scrubbing\) \{ playWasInView = inside; wasPlaying = false; return; \}/);
  assert.match(hud, /,\\"playing\\":/);
  assert.match(hud, /bool DemoPlaying\(\)/);
});

test('numbers are typed INSIDE their cell, and the editor is destroyed on commit', () => {
  assert.match(graph, /function beginCellEdit\(host, label, initial, rect, onCommit\)/);
  assert.match(graph, /function commitEdit\(\)/);
  // Deletion is what drops the keyboard focus grab that was swallowing Space.
  assert.match(graph, /e\.DeleteAsync\(0\)/);
  assert.match(graph, /beginCellEdit\(cell\.box, rec && rec\.cell/);
  assert.match(graph, /beginCellEdit\(rows\[c\]\.valBox, rows\[c\]\.val/);
  assert.doesNotMatch(graph, /\btypeBox\b/);
  // Values are clamped to the property's coded range before they are sent.
  assert.match(graph, /function clampToSpec\(ch, v\)/);
  assert.match(graph, /clampToSpec\(ch, v\)\.toFixed\(3\)/);
});

test('the strip value axis is the coded min/max, with no padding past it', () => {
  const sceneKeys = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
  assert.match(sceneKeys, /double lo=s\.min, hi=s\.max;/);
  assert.match(sceneKeys, /j\.DoubleField\("min",lo\); j\.DoubleField\("max",hi\);/);
  assert.doesNotMatch(sceneKeys, /double pad=std::max/);
});

test('scene properties are grouped under collapsible group headers', () => {
  assert.match(graph, /var SCENE_ROW_H = 26, SCENE_GROUP_H = 24;/);
  assert.match(graph, /var sceneStrips = \[\], sceneGroups = \[\], sceneScroll = 0, sceneContentH = 0, sceneCollapsed = \{\}, sceneCollapsedCh = \{\};/);
  // Groups come from the catalog's own family+group, in catalog order.
  assert.match(graph, /gid = \(c\.family \|\| ''\) \+ '\.' \+ \(c\.group \|\| ''\)/);
  // A collapsed group emits no strips at all -- its labels AND its canvas bands go away.
  assert.match(graph, /grp\.collapsed = !!sceneCollapsed\[grp\.id\];/);
  assert.match(graph, /if \(grp\.collapsed\) continue;/);
  // The group bar carries a disclosure, the name, a count and a show/hide-all eye.
  assert.match(graph, /gchev = lbl\(ghot, grp\.collapsed \? '▶' : '▼'/);
  assert.match(graph, /geye = btn\(gh, allVis \? '◉' : '○'/);
  assert.match(graph, /sceneCollapsed\[gid\] = !sceneCollapsed\[gid\]/);
  // A channel with no strip (collapsed group) is never drawn or hit-tested.
  assert.match(graph, /function sceneStripped\(ch\)/);
  assert.match(graph, /!ch\.vis \|\| !sceneStripped\(ch\)\) continue;/);
});

test('scene curves overlay ONE shared canvas, not a band per property', () => {
  assert.match(graph, /function chBand\(ch\) \{ return \{ y0: 0, h: G\.ch \}; \}/);
  // Collapsing a group takes its curves out of the shared canvas (there is no band to remove).
  assert.match(graph, /sceneCollapsedCh\[model\[grp\.items\[h\]\]\.ch\] = true;/);
  assert.match(graph, /return !sceneOn\(\) \|\| !sceneCollapsedCh\[ch\.ch\];/);
  // No per-strip bands or per-strip axes remain.
  assert.doesNotMatch(graph, /SCENE_BAND_PAD/);
  assert.doesNotMatch(graph, /sceneStrips\[si\]/);
});

test('time snap: one shared step quantises the playhead, key placement and key drags', () => {
  const bridge = read('AfxHookSource2/Filmmaker/Movie/CameraBridge.cpp');
  const bridgeH = read('AfxHookSource2/Filmmaker/Movie/CameraBridge.h');
  assert.match(graph, /function snapStep\(\)/);
  assert.match(graph, /function niceSnap\(v\)/);
  assert.match(graph, /function snapTick\(t\)/);
  assert.match(graph, /function scrubTick\(clx\) \{ return snapTick\(xToTick\(clx\)\); \}/);
  // Alt bypasses snap for one gesture.
  assert.match(graph, /if \(step <= 0 \|\| \(st && st\.alt\)\) return t;/);
  assert.match(bridge, /bool CameraBridge_GetAltDown\(\)/);
  assert.match(bridgeH, /bool CameraBridge_GetAltDown\(\);/);
  // The playhead scrub uses the snapped tick everywhere it is read.
  assert.doesNotMatch(graph, /xToTick\(cc\.clx\)\.toFixed\(2\)/);
  assert.match(graph, /scrubTick\(cc\.clx\)\.toFixed\(2\)/);
  // Key placement and the lead-key drag snap.
  assert.match(graph, /snapTick\(st\.playhead\)\.toFixed\(2\)/);
  assert.match(graph, /Math\.round\(snapTick\(st\.playhead\)\)/);
  assert.match(graph, /leadT0: k\.t/);
  assert.match(graph, /dt = snapTick\(gesture\.leadT0 \+ dt\) - gesture\.leadT0;/);
  // C++ publishes the shared value and the Alt modifier.
  assert.match(hud, /,\\"snap\\":/);
  assert.match(hud, /editor\.graph\.snap/);
  assert.match(hud, /,\\"alt\\":/);
});

test('the snap dropdown offers Auto, presets, Custom and Off', () => {
  assert.match(graph, /var items = \[\['auto', 'Auto'\], \['1', '1'\], \['10', '10'\], \['100', '100'\], \['1000', '1000'\], \['custom', 'Custom…'\], \['off', 'Off'\]\];/);
  assert.match(graph, /function showSnapMenu\(\)/);
  assert.match(graph, /rawCmd\('mirv_filmmaker settings ui editor\.graph\.snap ' \+ v\)/);
  // The grid follows the snap step when on, and the automatic step when off.
  assert.match(graph, /if \(snap > 0 && snap \* pxPerTick >= 4 && span \/ snap <= 140\)/);
  assert.match(graph, /var major = snap > 0 \? snap \* Math\.max\(1, Math\.round\(labelStep \/ snap\)\) : labelStep;/);
});

test('the editing area carries no instructions', () => {
  assert.doesNotMatch(graph, /Click anywhere on this strip/);
  assert.doesNotMatch(graph, /click an empty strip = first key/);
  assert.doesNotMatch(graph, /var footerHint/);
  // Empty canvas is a box-select target, never a create target.
  assert.doesNotMatch(graph, /ec && !ec\.keys\.length/);
});

test('the wheel is contextual and Ctrl/Shift never reaches the camera FOV', () => {
  assert.match(graph, /if \(st\.ctrl\) \{/);
  assert.match(graph, /if \(st\.shift\) \{/);
  assert.match(graph, /return true;   \/\/ consumed: over the dock the wheel is ours/);
  // MovieMode hands the modifier notch to the editor while the graph dock is up, before its own
  // free-cam FOV branch.
  assert.match(movie,
    /if \(CameraBridge_GetFreeCamEnabled\(\) && \(ctrlDown \|\| shiftDown\)\) \{\s*if \(GraphEditorExperiment_Enabled\(\)\) \{\s*CameraEditor_PushWheel\(delta > 0 \? \+1 : -1\);\s*return true;\s*\}/);
});
