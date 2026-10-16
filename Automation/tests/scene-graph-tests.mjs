// The SCENE GRAPH dock's editing contracts (Docs/features/camera-editor-feature.md, "Scene keys
// and the Scene Graph").
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

const defined = (src) => new Set([...src.matchAll(/function\s+([A-Za-z_$][\w$]*)\s*\(/g)].map((m) => m[1]));
// The JS inside a header's raw strings, with comments and string literals blanked out.
function jsCode(src, tag) {
  const body = src.split('R"' + tag + '(').slice(1).map((c) => c.slice(0, c.indexOf(')' + tag + '"'))).join('\n');
  return body.replace(/\/\/[^\n]*/g, '').replace(/'(?:\\.|[^'\\\n])*'/g, "''").replace(/"(?:\\.|[^"\\\n])*"/g, '""');
}

test('every helper the shared colour picker calls exists in the graph dock', () => {
  // The picker is a CHUNK spliced into its host. A call the graph does not define is a
  // ReferenceError on the first colour strip, which aborted the strip list there and left every
  // property after it missing. colorField is Config's own wrapper and never runs in the graph.
  const code = jsCode(picker, 'CLRJS');
  const used = code.slice(0, code.indexOf('function colorField'));
  const have = new Set([...defined(graph), ...defined(picker), ...defined(colorMath)]);
  // Language keywords, globals, and the picker's own callback parameter.
  const builtins = new Set(['if', 'for', 'while', 'switch', 'return', 'function', 'catch', 'typeof',
    'parseInt', 'parseFloat', 'isFinite', 'onChange']);
  const missing = new Set();
  for (const m of used.matchAll(/(^|[^.\w$])([A-Za-z_$][\w$]*)\s*\(/g)) {
    if (!builtins.has(m[2]) && !have.has(m[2])) missing.add(m[2]);
  }
  assert.deepEqual([...missing], []);
  for (const token of ['drawer:', 'clear:']) assert.ok(graph.includes(token), 'S.' + token + ' missing');
});

test('one strip that fails to build cannot take the rest of the list with it', () => {
  assert.match(graph, /scene row ' \+ \(ch && ch\.fieldId\) \+ ' failed: /);
  assert.match(graph, /colorPicker\(pickBody, ch\.name, swRgb/);
  assert.doesNotMatch(graph, /colorPicker\(root,/);
});

test('every raw-string chunk of the graph script stays under the MSVC literal limit', () => {
  // C2026 "string too big": MSVC caps one string literal piece at 16380 bytes.
  const chunks = graph.split('R"GEJS(').slice(1).map((c) => c.slice(0, c.indexOf(')GEJS"')));
  for (const c of chunks) assert.ok(Buffer.byteLength(c, 'utf8') < 16000, 'chunk of ' + Buffer.byteLength(c, 'utf8') + ' bytes');
});

test('both graph modes move the playhead with the verb that also moves the world', () => {
  assert.match(graph, /function playheadTo\(t\) \{ rawCmd\('mirv_filmmaker grapheditor playhead '/);
  assert.doesNotMatch(graph, /cmd\('playhead /);
  assert.doesNotMatch(sceneKeys, /action=="playhead"/);
});

test('clicking a scene key goes to it, and a dragged key carries the playhead', () => {
  assert.match(graph, /best = \{ ch: ch\.ch, id: k\.id, t: k\.t \}/);
  assert.match(graph, /if \(sceneOn\(\)\) \{ gesture\.seek = true; gesture\.ph = Math\.round\(k\.t\); playheadTo\(gesture\.ph\); \}/);
  assert.match(graph, /else if \(gesture\.type === 'kf' && gesture\.seek\)/);
  // The jump lands and is held there while the demo catches up (TimelinePlayhead.h, "Held").
  assert.match(playhead, /DemoScrub_Target\(target\)/);
  assert.match(playhead, /curTick == s\.landedTick && std::fabs\(curTick - s\.heldTick\) <= kLandTolerance/);
});

test('a scene key drag sends its total offset, so refused steps are not lost', () => {
  assert.match(graph, /cmd\('movesel ' \+ dt\.toFixed\(3\) \+ ' ' \+ dv2\.toFixed\(3\)\)/);
  assert.match(track, /TrackData pending = editing \? before : data;/);
});

test('scene curves are fitted to what they draw, never while a gesture holds them', () => {
  assert.match(graph, /function fitSceneRange\(ch\)/);
  assert.match(graph, /if \(sceneMode && !gesture\) refitSceneRanges\(\);/);
  // Angles are drawn the way they are sampled: the short way round, wrapped.
  assert.match(graph, /function scenePolyline\(ch, a, b\)/);
  // Handles stay inside the property's range.
  assert.match(sceneKeys, /else dv=std::max\(spec->min,std::min\(spec->max,k->value\.numbers\[0\]\+dv\)\)-k->value\.numbers\[0\];/);
});

test('FIT frames every visible key, and new keys off screen are brought into view', () => {
  assert.match(graph, /var fitBtn = zoomBtn\('FIT'/);
  assert.match(graph, /function revealNewKeys\(mode\)/);
  assert.match(graph, /releasePlayhead\(t\);\n      revealTicks\(t, t\);/);
});

test('a property has ONE name, the CONFIG control\'s, in the graph and the inspector', () => {
  assert.doesNotMatch(inspector, /skFieldLabel/);
  assert.match(inspector, /var name = f\.label;/);
  assert.match(catalog, /\{"env\.sunAzimuth", "Sun azimuth"\}/);
  assert.match(catalog, /\{"env\.sunElevation", "Sun elevation"\}/);
  assert.doesNotMatch(catalog, /std::string Label\(/);
  // Group bars say which CONFIG tab a card belongs to: both tabs have a Lighting card.
  assert.match(graph, /function sceneGroupTitle\(ch\)/);
});
