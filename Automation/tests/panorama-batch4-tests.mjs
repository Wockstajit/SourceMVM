// Audit batch 4: Panorama per-frame cost, command-line safety and camera-path undo.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = (rel) => fs.readFileSync(new URL(`../../AfxHookSource2/Filmmaker/${rel}`, import.meta.url), 'utf8');

test('the scene-key catalog travels on its own attribute and is parsed only when it changes', () => {
  const hud = read('Panorama/CameraEditorHud.cpp');
  const js = read('Panorama/CameraEditorJs.h');
  // The per-frame state carries the keys without the ~35 KB catalog.
  assert.match(hud, /SceneKeys_StateJson\(false\)/);
  assert.match(hud, /m_symSceneCatalog = m_bridge\.MakeSymbol\("skcatalog"\);/);
  assert.match(hud, /if \(catalog != m_lastSceneCatalog\) \{\s*m_bridge\.SetAttributeString\(m_root, m_symSceneCatalog/);
  assert.match(hud, /\+\+m_sceneCatalogFrames >= 60/, 'the string is rebuilt at most every 60 frames');
  // The panel re-parses neither an unchanged state nor an unchanged catalog.
  assert.match(js, /if \(raw !== lastStateRaw\) \{ try \{ st = JSON\.parse\(raw\); lastStateRaw = raw;/);
  assert.match(js, /if \(catalogRaw !== lastCatalogRaw\)/);
  assert.match(js, /if \(st\.sceneKeys\) st\.sceneKeys\.catalog = sceneCatalog;/);
  const keys = read('Movie/SceneKeys.cpp');
  assert.match(keys, /if \(withCatalog\) \{ j\.Key\("catalog"\); CatalogJson\(j\); \}/);
});

test('a queued demo-list command is always one line', () => {
  const gui = read('Panorama/FilmmakerGuiJs.h');
  assert.match(gui, /var line = String\(command\)\.replace\(\/\[\\r\\n\]\+\/g, ' '\);/);
  assert.doesNotMatch(gui, /GetAttributeString\('cmd', ''\) \+ command \+/);
});

test('a skybox name is passed to the console as one quoted argument', () => {
  const sky = read('Panorama/ConfigHudWeatherSkyTabJs.h');
  assert.match(sky, /function skyNameArg\(name\) \{ return '"' \+ String\(name\)\.replace\(\/\["\\r\\n\]\/g, ''\) \+ '"'; \}/);
  assert.match(sky, /world sky apply ' \+ skyNameArg\(entry\.name\)/);
  assert.match(sky, /world sky delete ' \+ skyNameArg\(entry\.name\)/);
  assert.doesNotMatch(sky, /world sky (apply|delete) ' \+ entry\.name/);
});

test('every camera-path keyframe edit can be undone', () => {
  const path = read('Movie/CameraPath.cpp');
  const body = (signature) => {
    const start = path.indexOf(signature);
    assert.ok(start >= 0, signature);
    return path.slice(start, path.indexOf('\n}', start));
  };
  for (const fn of ['void CameraPath::PlaceMarker()', 'int CameraPath::PlaceMarkerAt(', 'void CameraPath::DeleteIndex(',
    'void CameraPath::DeleteAll(', 'void CameraPath::SetEaseIndex(', 'void CameraPath::SetSpeedMulIndex(',
    'void CameraPath::SetSelectedSpeedMul(']) {
    assert.match(body(fn), /PushCurveUndo\(\);/, `${fn} records an undo step`);
  }
  // Re-posing a marker on an occupied tick is an edit too, not only adding a new one.
  assert.match(body('void CameraPath::PlaceMarker()'), /if \(existing\.tick != m\.tick\) continue;\s*PushCurveUndo\(\);/);
});
