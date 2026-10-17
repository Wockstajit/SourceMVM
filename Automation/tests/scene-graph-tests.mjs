// The TIMELINE dock's scene-key editing contracts (Docs/features/camera-editor-feature.md, "The
// TIMELINE dock"). Scene properties are layers next to the camera path, edited through
// `scenekeys graph ...`; camera channels go through `grapheditor ...`.
// Static checks over the shipped sources, so they run without CS2.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const graph = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h');
const picker = read('AfxHookSource2/Filmmaker/Panorama/ConfigColorPickerJs.h');
const colorMath = read('AfxHookSource2/Filmmaker/Panorama/ColorMathJs.h');
const inspector = read('AfxHookSource2/Filmmaker/Panorama/CameraEditorSceneKeysJs.h');
const sceneKeys = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
const catalog = read('AfxHookSource2/Filmmaker/Movie/SceneKeyframeCatalog.cpp');
const track = read('AfxHookSource2/Filmmaker/Movie/SceneKeyframeTrackMath.h');
const playhead = read('AfxHookSource2/Filmmaker/Movie/TimelinePlayhead.cpp');
const hud = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorExperimentHud.cpp');

const defined = (src) => new Set([...src.matchAll(/function\s+([A-Za-z_$][\w$]*)\s*\(/g)].map((m) => m[1]));
// The JS inside a header's raw strings, with comments and string literals blanked out.
function jsCode(src, tag) {
  const body = src.split('R"' + tag + '(').slice(1).map((c) => c.slice(0, c.indexOf(')' + tag + '"'))).join('\n');
  return body.replace(/\/\/[^\n]*/g, '').replace(/'(?:\\.|[^'\\\n])*'/g, "''").replace(/"(?:\\.|[^"\\\n])*"/g, '""');
}

test('every helper the shared colour picker calls exists in the timeline dock', () => {
  // The picker is a CHUNK spliced into its host. A call the dock does not define is a
  // ReferenceError on the first colour property, which aborted the row list there.
  const code = jsCode(picker, 'CLRJS');
  const used = code.slice(0, code.indexOf('function colorField'));
  const have = new Set([...defined(graph), ...defined(picker), ...defined(colorMath)]);
  const builtins = new Set(['if', 'for', 'while', 'switch', 'return', 'function', 'catch', 'typeof',
    'parseInt', 'parseFloat', 'isFinite', 'onChange']);
  const missing = new Set();
  for (const m of used.matchAll(/(^|[^.\w$])([A-Za-z_$][\w$]*)\s*\(/g)) {
    if (!builtins.has(m[2]) && !have.has(m[2])) missing.add(m[2]);
  }
  assert.deepEqual([...missing], []);
  for (const token of ['drawer:', 'clear:']) assert.ok(graph.includes(token), 'S.' + token + ' missing');
});

test('one row that fails to build cannot take the rest of the list with it', () => {
  assert.match(graph, /\$\.Msg\('\[timeline\] row ' \+ \(flat\[i\]\.node && flat\[i\]\.node\.id\) \+ ' failed: '/);
  assert.match(graph, /colorPicker\(pickBody, n\.name, swRgb/);
  assert.doesNotMatch(graph, /colorPicker\(root,/);
});

test('every raw-string chunk of the timeline script stays under the MSVC literal limit', () => {
  // C2026 "string too big": MSVC caps one string literal piece at 16380 bytes.
  const chunks = graph.split('R"GEJS(').slice(1).map((c) => c.slice(0, c.indexOf(')GEJS"')));
  for (const c of chunks) assert.ok(Buffer.byteLength(c, 'utf8') < 16000, 'chunk of ' + Buffer.byteLength(c, 'utf8') + ' bytes');
});

test('the playhead has one verb, the one that also moves the world', () => {
  assert.match(graph, /function playheadTo\(t\) \{ rawCmd\('mirv_filmmaker grapheditor playhead '/);
  assert.doesNotMatch(graph, /sceneCmd\('playhead /);
  assert.doesNotMatch(sceneKeys, /action=="playhead"/);
});

test('both models publish into ONE state: camera channels plus "scene"', () => {
  assert.match(hud, /SceneKeys_AppendTimelineState\(o\);/);
  assert.match(sceneKeys, /out << ",\\"scene\\":\{\\"rev\\":"/);
  assert.doesNotMatch(hud, /graphMode/);
  assert.match(graph, /scCh = \(st\.scene && st\.scene\.channels\) \|\| \[\]/);
  // Each channel is addressed by domain: c<index> for the camera, s<index> for the scene.
  assert.match(graph, /c\.dom = 'cam'; c\.uid = 'c' \+ c\.ch;/);
  assert.match(graph, /c\.dom = 'scene'; c\.uid = 's' \+ c\.ch;/);
  assert.match(graph, /function camCmd\(s\) \{ rawCmd\('mirv_filmmaker grapheditor ' \+ s\); \}/);
  assert.match(graph, /function sceneCmd\(s\) \{ rawCmd\('mirv_filmmaker scenekeys graph ' \+ s\); \}/);
});

test('clicking a scene key goes to it, and a dragged key carries the playhead', () => {
  assert.match(graph, /startKeyDrag\(cc, doms, t, domOf\(members\[0\]\.uid\) === 'scene', members\[0\]\)/);
  assert.match(graph, /if \(seek\) \{ gesture\.seek = true; gesture\.ph = Math\.round\(t\); playheadTo\(gesture\.ph\); \}/);
  assert.match(graph, /if \(g\.seek\) \{/);
  // The jump lands and is held there while the demo catches up (TimelinePlayhead.h, "Held").
  assert.match(playhead, /DemoScrub_Target\(target\)/);
  assert.match(playhead, /curTick == s\.landedTick && std::fabs\(curTick - s\.heldTick\) <= kLandTolerance/);
});

test('a scene key drag sends its total offset, a camera drag its per-frame step', () => {
  assert.match(graph, /sceneCmd\('movesel ' \+ dt\.toFixed\(3\) \+ ' ' \+ dvd\.toFixed\(3\)\)/);
  assert.match(graph, /camCmd\('movesel ' \+ \(dt - g\.ldt\)\.toFixed\(3\)/);
  assert.match(track, /TrackData pending = editing \? before : data;/);
  // The edit (one undo step) opens when the drag starts, not on a click that only selects.
  assert.match(graph, /g\.dragging = true;\n        g\.doms\.forEach\(function \(d\) \{ edit\(d\); domCmd\(d, 'editbegin'\); \}\);/);
});

test('selecting in one model clears the other, so Delete and eases act on what is lit', () => {
  assert.match(graph, /if \(!list\.length\) \{ if \(!add\) domCmd\(dom, 'selclear'\); return; \}/);
  assert.match(graph, /function deleteSelection\(\)/);
  assert.match(graph, /selectedDoms\(\)\.forEach\(function \(dom\) \{ edit\(dom\); domCmd\(dom, 'delsel'\); \}\);/);
});

test('curves are fitted to what they draw, never while a gesture holds them', () => {
  assert.match(graph, /function fitSceneRange\(c\)/);
  assert.match(graph, /function fitCamRange\(c\)/);
  assert.match(graph, /if \(!gesture\) refitRanges\(graphChans\(\)\);/);
  assert.match(graph, /function rng\(c\) \{ return ranges\[c\.uid\] \|\| \{ lo: c\.min, hi: c\.max \}; \}/);
  assert.match(graph, /ranges = \{\}; graphSig = '';/);
  // Angles are drawn the way they are sampled: the short way round, wrapped.
  assert.match(graph, /function scenePolyline\(c, a, b\)/);
  // Handles stay inside the property's range.
  assert.match(sceneKeys, /else dv=std::max\(spec->min,std::min\(spec->max,k->value\.numbers\[0\]\+dv\)\)-k->value\.numbers\[0\];/);
});

test('FIT frames every key, and new keys off screen are brought into view', () => {
  assert.match(graph, /var fitBtn = footBtn\('FIT'/);
  assert.match(graph, /function revealNewKeys\(\)/);
  assert.match(graph, /releasePlayhead\(t\);\n      revealTicks\(t, t\);/);
});

test('a property has ONE name, the CONFIG control\'s, in the timeline and the inspector', () => {
  assert.doesNotMatch(inspector, /skFieldLabel/);
  assert.match(inspector, /var name = f\.label;/);
  assert.match(catalog, /\{"env\.sunAzimuth", "Sun azimuth"\}/);
  assert.match(catalog, /\{"env\.sunElevation", "Sun elevation"\}/);
  assert.doesNotMatch(catalog, /std::string Label\(/);
  // Layers say which CONFIG tab a card belongs to: both tabs have a Lighting card.
  assert.match(graph, /function sceneLayerTitle\(c\)/);
});

test('scene property menus keep Disable / Delete, and a colour opens the shared wheel', () => {
  // Per property, or per channel of a scene object's grouped row (sceneEach).
  assert.match(graph, /sceneEach\(n, function \(f\) \{ return 'mute ' \+ f \+ mon; \}\)/);
  assert.match(graph, /if \(n\.kind === 'layer'\) rawCmd\('mirv_filmmaker scenekeys clear ' \+ n\.groupId\);/);
  assert.match(graph, /else sceneEach\(n, function \(f\) \{ return 'clear ' \+ f; \}\);/);
  assert.match(graph, /chip\.SetPanelEvent\('onactivate', function \(\) \{ crec\.picker\.show\(\); \}\);/);
});
