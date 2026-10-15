import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const editor = read('AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h');
const inspector = read('AfxHookSource2/Filmmaker/Panorama/CameraEditorSceneKeysJs.h');
const timeline = read('AfxHookSource2/Filmmaker/Panorama/CameraTimelineJs.h');
const graph = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h');
const hud = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorExperimentHud.cpp');
const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');

test('Scene Keys is the third Camera Editor inspector tab', () => {
  const path = editor.indexOf("'PATH CAMERA'");
  const follow = editor.indexOf("'FOLLOW CAMERA'");
  const scene = editor.indexOf("'SCENE KEYS'");
  assert.ok(path >= 0 && path < follow && follow < scene);
});

test('inspector is a strip selector rather than a duplicate value editor', () => {
  // One arm row per property, and NO second editor for a value the graph already owns.
  assert.match(inspector, /function skRowPanel/);
  assert.match(inspector, /function skToggle/);
  assert.doesNotMatch(inspector, /edValueTile|edSwitchTile/);
});

test('the Scene Keys tab opens on controls, not on a page of prose', () => {
  // It used to stack five blocks of text between the tab strip and the first thing you could
  // click: an intro paragraph, a note under the buttons, a save line, a summary line and a
  // redundant rule above the family rules. One status line survives.
  assert.doesNotMatch(inspector, /var skIntro/);
  assert.doesNotMatch(inspector, /Key every strip drops one key/);
  assert.doesNotMatch(inspector, /edSubHead\(skBrowser, 'ADD GRAPH STRIPS'\)/);
  assert.match(inspector, /var skStatus = edNote/);
  assert.doesNotMatch(inspector, /var skSave|var skSummary/);
  // What the prose said now arrives on hover instead.
  assert.match(inspector, /skActions\.btns\[1\]\.__tip/);
});

test('the two workspace headers line up: same head, same tab metrics', () => {
  // Config's tabify uses 2px of top padding and the editor's uses 8px, so the editor's top tabs
  // restate it rather than inheriting -- that 6px is what put the whole editor column low.
  assert.match(editor, /b\.style\.paddingTop = '2px';/);
  assert.match(editor, /var head = mk\('Panel', inspector\); head\.hittest = false; head\.id = 'EdHead';/);
  assert.doesNotMatch(editor, /var head = row\(inspector\)/);
});

test('Scene Keys timeline has separate camera and scene lanes', () => {
  assert.match(timeline, /var camCY = sceneMode \? 9 : 11/);
  assert.match(timeline, /var sceneCY = 29/);
  assert.match(timeline, /st\.uiEditorInspector === 'scene'/);
});

test('Graph owns full-width scene property strips', () => {
  assert.match(graph, /SCENE KEYS uses the Graph dock as a stacked property editor/);
  assert.match(graph, /sceneStrips/);
  // Keys are placed with the property row's diamond at the playhead, not by clicking empty canvas
  // space; the old "click the strip to create the first key" instruction is gone entirely.
  assert.match(graph, /function toggleSceneKey/);
  assert.match(graph, /Add or remove a keyframe at the playhead/);
  assert.doesNotMatch(graph, /Click anywhere on this strip/);
});

test('the Graph dock picks its model itself, not from the inspector tab', () => {
  // Keying the dock off the inspector made opening SCENE KEYS take the camera graph away, with
  // no way back to the curves of a path that was already placed.
  assert.match(graph, /function sceneOn\(\) \{ return !!\(st && st\.graphMode === 'scene'\); \}/);
  assert.doesNotMatch(graph, /uiEditorInspector/);
  assert.match(graph, /camModeTab/);
  assert.match(graph, /sceneModeTab/);
  assert.match(hud, /editor\.graph/);
});

test('scene strip values are scrubbed, typed and right-clicked, not only typed', () => {
  assert.match(graph, /function scrubbable\(ch\)/);
  assert.match(graph, /type: 'snum'/);
  assert.match(graph, /function openSceneType/);
  assert.match(graph, /function showStripMenu/);
});

test('the Camera Editor tab strip uses the Config panel metrics', () => {
  for (const key of ['UI.tabTop.font', 'UI.tabTop.ls', 'UI.tabTop.gap', 'UI.tabTop.padBottom']) {
    assert.ok(editor.includes(key), key + ' missing from the editor tab strip');
  }
});

test('Space is plain demo play while the dock shows scene strips', () => {
  assert.match(movie, /const bool graphScene = GraphEditorExperiment_Enabled\(\)/);
  assert.match(movie, /vkey == kVK_SPACE && !graphScene/);
  assert.match(hud, /if \(FilmmakerSettings_UiString\("editor\.graph", "camera"\) == "scene"\)/);
});
