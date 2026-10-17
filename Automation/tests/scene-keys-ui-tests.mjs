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
const cameraPath = read('AfxHookSource2/Filmmaker/Movie/CameraPath.cpp');

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

test('scene properties are layers on the one TIMELINE, next to the camera path', () => {
  assert.match(graph, /function sceneLayerTitle\(c\)/);
  // Keys are placed with the property row's diamond at the playhead, not by clicking empty track
  // space; the old "click the strip to create the first key" instruction is gone entirely.
  assert.match(graph, /function toggleKeyAt\(n\)/);
  assert.match(graph, /Add or remove a keyframe at the playhead/);
  assert.doesNotMatch(graph, /Click anywhere on this strip/);
  // The inspector's shortcut opens the dock; there is no scene mode to put it in any more.
  assert.match(inspector, /cmd\('mirv_filmmaker dock timeline'\)/);
  assert.doesNotMatch(inspector, /editor\.graph scene/);
});

test('the dock shows camera and scene keys together, never one model at a time', () => {
  // Keying the dock off the inspector (and later off a GRAPH / SCENE GRAPH switch) meant one model
  // was always hidden: a shot's camera curves vanished while its lighting was up, and back.
  assert.doesNotMatch(graph, /graphMode|sceneOn\(\)|camModeTab|sceneModeTab/);
  assert.doesNotMatch(graph, /uiEditorInspector/);
  assert.doesNotMatch(hud, /"editor\.graph"/);
  assert.match(hud, /editor\.timeline\.view/);
});

test('scene values are scrubbed, typed and right-clicked, not only typed', () => {
  assert.match(graph, /function valueHit\(lx, ly\)/);
  assert.match(graph, /gesture = \{ type: 'num', row: vh\.row, comp: vh\.comp/);
  assert.match(graph, /function openType\(r, comp\)/);
  assert.match(graph, /function showRowMenu\(lx, ly, r\)/);
});

test('the Camera Editor tab strip uses the Config panel metrics', () => {
  for (const key of ['UI.tabTop.font', 'UI.tabTop.ls', 'UI.tabTop.gap', 'UI.tabTop.padBottom']) {
    assert.ok(editor.includes(key), key + ' missing from the editor tab strip');
  }
});

test('Space plays the demo with the TIMELINE up; the path takes the view only with AUTO or J', () => {
  // User request 2026-10-17: Space in the keyframes no longer jumps into the camera path.
  assert.match(movie, /vkey == kVK_SPACE && CameraTimeline_Visible\(\) && !timelineDock && cpMode == CPMode::Editing/);
  assert.doesNotMatch(movie, /timelinePathOff/);
  // The toolbar's play button makes the same choice; a playing path pauses.
  assert.match(graph, /function togglePlay\(\) \{\s*if \(st\.pathPlaying\) rawCmd\('mirv_filmmaker camtl pause'\);\s*else rawCmd\(st\.playing \? 'demo_pause' : 'demo_resume'\);/);
  assert.doesNotMatch(graph, /'playtest'/);
  // AUTO is a button on the camera path layer, saved, and follows the path inside its keys.
  assert.match(graph, /if \(n\.dom === 'cam'\) \{[\s\S]{0,700}rawCmd\('mirv_filmmaker camtl autopath toggle'\)/);
  assert.match(graph, /if \(r\.ap && r\.apOn !== !!st\.autoPath\)/);
  assert.match(cameraPath, /bool CameraPath_AutoPath\(\) \{ return FilmmakerSettings_UiBool\("editor\.timeline\.autopath", false\); \}/);
  assert.match(cameraPath, /void CameraPath::RunAutoPath\(\) \{[\s\S]{0,500}GraphEditorExperiment_Enabled\(\) && m_timing == Timing::Live[\s\S]{0,1500}m_play\.StartPlay\(m_eval\.Duration\(\), \/\*rangeGated\*\/ true\);/);
  assert.match(cameraPath, /if \(playDone && m_autoPlaying\) \{\s*EndAutoPath\("the last key"\);/);
  assert.match(cameraPath, /!DemoIsPaused\(\) && !m_autoPlaying\)\s*DemoCmd\("demo_pause"\);/);
});

test('FIT frames the selected keys, else the selected layer, and a second press everything', () => {
  assert.match(graph, /var span = again \? null : keySpan\(function \(k\) \{ return !!k\.sel; \}\);/);
  assert.match(graph, /if \(!span && !again && selNode\) \{[\s\S]{0,200}allTimes\(nodeById\(selNode\)\)/);
  assert.match(graph, /var again = lastFit && lastFit\.t0 === view\.t0 && lastFit\.t1 === view\.t1;/);
});

test('keyframe shortcuts go to the dock, which knows which model holds the selection', () => {
  assert.match(movie, /EnqueueCmd\(redo \? "mirv_filmmaker grapheditor key redo" : "mirv_filmmaker grapheditor key undo"\)/);
  assert.match(movie, /EnqueueCmd\("mirv_filmmaker grapheditor key delete"\)/);
  assert.match(movie, /"mirv_filmmaker grapheditor key easeinout"/);
  assert.match(graph, /api\.key = function \(name\)/);
});
