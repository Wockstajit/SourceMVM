// Scene object rigs (Docs/features/scene-object-rigs.md): the RIG card, the TIMELINE dock's object
// layers and Hold, and the native command surface they drive must agree. Static checks over the
// shared Panorama scripts and the C++, plus the browser mock's sampling of hold keys.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const rigJs = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudRigJs.h');
const envTab = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentTabJs.h');
const envState = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudEnvironmentStateJs.h');
const outliner = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudOutlinerJs.h');
const graph = read('AfxHookSource2/Filmmaker/Panorama/GraphEditorJs.h');
const rigCpp = read('AfxHookSource2/Filmmaker/Movie/SceneRig.cpp');
const rigMath = read('AfxHookSource2/Filmmaker/Movie/SceneRigMath.h');
const keysCpp = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
const command = read('AfxHookSource2/Filmmaker/FilmmakerCommand.cpp');
const mainCpp = read('AfxHookSource2/main.cpp');
const owner = read('AfxHookSource2/Filmmaker/Movie/CameraOwnership.h');
const envScene = read('AfxHookSource2/Filmmaker/Environment/EnvironmentScene.cpp');
const mockKeys = read('UiMockup/shim/scene-keys.js');
const mockCmd = read('UiMockup/shim/commands.js');
const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');
const configJs = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudJs.h');

test('the RIG card is part of the Scene Objects card, under the light and mesh inspectors', () => {
  assert.match(envTab, /#include "ConfigHudRigJs.h"\s*\n#include "ConfigHudEnvironmentStateJs.h"/);
  const meshHost = outliner.indexOf("var olMeshHost = mk('Panel', olCard);");
  const rigHost = outliner.indexOf("var olRigHost = mk('Panel', olCard);");
  const groupDrop = outliner.indexOf("var olGroupDrop = dropdownRow(olCard, 'Group'");
  assert.ok(meshHost >= 0 && meshHost < rigHost && rigHost < groupDrop, 'RIG sits after the inspectors, before Group');
  assert.match(rigJs, /var rigBox = mk\('Panel', olRigHost\);/);
});

test('every rig verb the card sends is one the native command handles', () => {
  const sent = new Set([...rigJs.matchAll(/rigCommand\('([a-z]+) /g)].map((m) => m[1]));
  for (const verb of ['parent', 'mount', 'missing', 'animate', 'place', 'set', 'lookthrough']) assert.ok(sent.has(verb), `card sends ${verb}`);
  for (const verb of sent) assert.match(rigCpp, new RegExp(`op == "${verb}"`), `SceneRig handles ${verb}`);
  assert.match(command, /_stricmp\(sub, "rig"\)\) \{[\s\S]{0,200}SceneRig_RunCommand\(argc, args, cmd\);/);
  // The mock answers the same verbs, so the browser review drives the same card paths.
  for (const verb of sent) assert.match(mockCmd, new RegExp(`'${verb}'`), `mock handles ${verb}`);
});

test('attaching picks a kind, then which one; the parent kinds match the native parser', () => {
  const kinds = [...rigJs.matchAll(/\['(none|player|held|dropped|grenade|object)', /g)].map((m) => m[1]);
  assert.deepEqual(kinds, ['none', 'player', 'held', 'dropped', 'grenade', 'object']);
  for (const k of kinds) assert.match(rigMath, new RegExp(`return "${k}";`), `ParentKindName knows ${k}`);
  assert.match(rigJs, /rigCommand\('parent ' \+ o\.id \+ ' ' \+ k \+ ' ' \+ v \+ mount\);/);
  assert.match(rigJs, /rigCommand\('parent ' \+ o\.id \+ ' none'\);/);
});

test('switching Animate off with keys asks first; a camera can be looked through', () => {
  assert.match(rigJs, /if \(r\.keys > 1\) cfgConfirm\('Stop animating\?'/);
  // Go to camera and Look through share a row shown for cameras; both land on the camera's tick.
  assert.match(rigJs, /rigViewActs\.visible = camera;/);
  assert.match(rigJs, /rigBtn\(rigViewActs, 'Go to camera', function \(\) \{ var o = envSelected\(\); if \(o\) rigCommand\('goto ' \+ o\.id\); \}\);/);
  assert.match(rigCpp, /if \(op == "goto"\) \{[\s\S]{0,300}SeekForCamera\(id, \*l, "go to"\);/);
  assert.match(rigCpp, /if \(on\) SeekForCamera\(id, \*l, "look through"\);/);
  assert.match(rigCpp, /if \(first >= 0\) return \(now >= first - 0\.5 && now <= last \+ 0\.5\) \? -1 : first;/);
  assert.match(rigCpp, /if \(on && !l->Camera\(\)\) \{ advancedfx::Warning\("rig: only camera objects can be looked through\\n"\); return; \}/);
});

test('a rigged light keeps its turn rows at the top of its card, showing the rig\'s angles', () => {
  // Hiding them put rotation below the fold of a long card (reported: "can't rotate the light").
  assert.match(envState, /var rigNow = st\.rig && st\.rig\.objects && st\.rig\.objects\[String\(selected\.id\)\];/);
  assert.match(envState, /c\[0\]\.sync\(rigged && turn >= 0 && rigNow\.local \? rigNow\.local\[3 \+ turn\] : envObjectValue\(selected, c\[1\]\)\);/);
  // Every light shows them, a point light too: they are keyed with its pose and show in the timeline.
  assert.match(envState, /c\[2\]\.visible = \(c\[1\] === 'width' \|\| c\[1\] === 'height'\) \? selected\.kind === 'area'/);
  assert.doesNotMatch(envState, /selected\.kind === 'point' && turn >= 0/);
  // The RIG card's own turn rows are only for cameras, which have no other card.
  assert.match(rigJs, /var visible = row\.key === 'fov' \? camera : row\.index <= 2 \? r\.attached : camera;/);
  // The Environment routes pose edits and gizmo drags of a rigged object to the rig.
  assert.match(envScene, /g_rigDrag=SceneRig_Rigged\(g_editing\);/);
  assert.match(envScene, /SceneRig_SetWorld\(g_drag\.objectId,at,nullptr\);/);
  assert.match(envScene, /if \(!SceneRig_SetLocal\(id, channel, Number\(args->ArgV\(5\)\)\)\)/);
});

test('the dock groups an object into Position / Rotation / Field of view and offers Hold', () => {
  assert.match(graph, /c\.family === 'objects' \? 'OBJECTS'/);
  assert.match(graph, /\['pos', 'Position', \['x', 'y', 'z'\]/);
  assert.match(graph, /\['rot', 'Rotation', \['pitch', 'yaw', 'roll'\]/);
  assert.match(graph, /return stepped\(ch\) \? \['hold', 'hold'\] : \[k\.hin \? 'hold' : sideOf\(k\.cl\), k\.hold \? 'hold' : sideOf\(k\.cr\)\];/);
  assert.match(graph, /if \(stepped\(c\) \|\| a\.hold\) \{/, 'a hold segment draws as a step in GRAPH');
  assert.match(graph, /if \(mode === 'hold'\) domCmd\(dom, 'hold'\);/);
  assert.match(keysCpp, /\} else if\(action=="hold"\) \{/);
  // Linear / Smooth / Ease turn a held segment back into a curve on the side they name.
  assert.match(keysCpp, /if\(how!="in"\)k\.hold=false;\s*\n\s*if\(how!="out"&&i>0\)f\.second\[i-1\]\.hold=false;/);
  // The dock learns about holds from the channel JSON.
  assert.match(keysCpp, /j\.BoolField\("hold",true\)/);
  assert.match(keysCpp, /j\.BoolField\("hin",true\)/);
});

test('with the TIMELINE dock up, a notch over a panel still reaches it (dock first, then the column)', () => {
  // It used to be swallowed outright, so CONFIG could not scroll while TIMELINE was open.
  assert.doesNotMatch(movie, /if \(GraphEditorExperiment_Enabled\(\)\)\s*\n\s*return true;/);
  assert.match(movie, /if \(dockIgnores\) \{\s*\n\s*if \(!cycle\) PushWorkspaceWheel\(delta > 0 \? \+1 : -1\);\s*\n\s*return true;/);
  // Over the preview it steps the camera once you have clicked into it (GAME mode), not before.
  assert.match(movie, /const bool dockIgnores = GraphEditorExperiment_Enabled\(\) && !\(cycle && where\.workspace && !where\.uiModeOn\);/);
  assert.match(movie, /if \(ConfigHud_Enabled\(\)\) ConfigHud_PushWheel\(notches\);\s*\n\s*else CameraEditor_PushWheel\(notches\);/);
  const dockFirst = configJs.indexOf('if ($.GraphExp && $.GraphExp.wheel && $.GraphExp.wheel(n)) return;');
  const column = configJs.indexOf('cfgScrollY -= n * 90;');
  assert.ok(dockFirst > 0 && dockFirst < column, 'CONFIG offers the notch to the dock before scrolling its column');
});

test('object channel numbers agree between C++, the dock state and the mock', () => {
  assert.match(rigMath, /constexpr int kObjectChannelBase = 1000000;/);
  assert.match(rigMath, /constexpr int kChannelStride = 16;/);
  assert.match(rigMath, /inline int ChannelIndex\(int objectId, int c\) \{ return kObjectChannelBase \+ objectId \* kChannelStride \+ c; \}/);
  assert.match(mockKeys, /OBJECT_BASE = 1000000, OBJECT_STRIDE = 16, OBJECT_CHANNELS = 13/);
  assert.match(mockKeys, /ch: OBJECT_BASE \+ object \* OBJECT_STRIDE \+ c/);
});

test('a looked-through camera owns the view after every other stage, and the shake follows it', () => {
  const follow = mainCpp.indexOf('ViewSetupAttachOverride(curTime');
  const rig = mainCpp.indexOf('Filmmaker::SceneRig_ViewSetup(');
  const rust = mainCpp.indexOf('AfxHookSource2Rs_OnCViewRenderSetupView(');
  assert.ok(follow > 0 && follow < rig && rig < rust, 'after the Follow Camera, before the Rust hook');
  assert.match(owner, /if \(sceneCamera\)\s+return sceneCameraAttached \? ShakeCamMode::Attach : ShakeCamMode::CameraPath;/);
});

test('the mock samples pose keys as the native track does: linear keys, clamped ends', () => {
  const context = { window: { MvmSceneCatalog: [] } };
  vm.createContext(context);
  vm.runInContext(mockKeys, context);
  const K = context.window.MvmSceneKeys;
  const sk = K.create();
  const x = K.objectSpec(4, 0);
  assert.equal(x.id, 'obj.4.x');
  assert.equal(K.meta('obj.4.yaw').type, 'angle');
  K.put(sk, x, 100, 10);
  K.put(sk, x, 200, 50);
  const keys = sk.byField['obj.4.x'];
  assert.ok(keys.every((k) => !k.hold), 'placed pose keys are linear');
  assert.equal(K.sample(x, keys, 50, 0), 10, 'before the first key: the first key');
  assert.equal(K.sample(x, keys, 150, 0), 30, 'blends across the segment');
  assert.equal(K.sample(x, keys, 300, 0), 50, 'after the last key: the last key');
  keys[0].hold = true;
  assert.equal(K.sample(x, keys, 150, 0), 10, 'a held key stays until the next');
  K.put(sk, x, 200, 60);
  assert.ok(!keys[0].hold, 're-posing the next key makes the held segment linear again');
  assert.equal(K.sample(x, keys, 150, 0), 35, 'and it blends');
  K.sync(sk, 150);
  const ch = sk.sceneChannels.find((c) => c.fieldId === 'obj.4.x');
  assert.equal(ch.ch, 1000064);
  assert.equal(ch.family, 'objects');
});

test('the in-world markers and Config\'s icons take one palette, one colour per kind', () => {
  const palette = read('AfxHookSource2/SceneMarkerMath.h');
  const props = read('AfxHookSource2/Filmmaker/Props/Props.cpp');
  const gizmo = read('AfxHookSource2/Filmmaker/Panorama/ConfigHudPropsGizmoJs.h');
  const mockState = read('UiMockup/shim/state.js');
  const hex = (name) => {
    const m = palette.match(new RegExp(String.raw`constexpr Rgb ${name}\{(\d+), (\d+), (\d+)\}`));
    assert.ok(m, `${name} is in the palette`);
    return '#' + m.slice(1, 4).map((v) => Number(v).toString(16).padStart(2, '0')).join('');
  };
  const kinds = { camera: 'kSceneCamera', point: 'kPointLight', spot: 'kSpotLight', area: 'kAreaLight' };
  for (const [kind, name] of Object.entries(kinds)) {
    assert.match(rigCpp, new RegExp(`Marker::${name}`), `the overlay colours a ${kind} from the palette`);
    assert.match(props, new RegExp(String.raw`"${kind}" \? &Marker::${name}`), `Config's ${kind} icon takes the same colour`);
    const mock = mockState.match(new RegExp(`kind: '${kind}'[^}]*accent: '(#[0-9a-f]{6})'`));
    assert.ok(mock && mock[1] === hex(name), `the mock's ${kind} icon matches the palette`);
  }
  assert.doesNotMatch(rigCpp, /SceneOverlayColour\{\d+, \d+, \d+, 255\}/, 'no colour of its own in the rig');
  assert.match(gizmo, /'2px solid ' \+ mark\.accent/, 'the icon ring is the kind colour');
  assert.match(gizmo, /m\.text\.style\.color = mark\.accent/, 'and so is its tag');
});

test('a removed object leaves the dock at once; its keys stay for an undo', () => {
  const keysCppNow = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
  assert.match(keysCppNow, /for \(int object : objects\) \{\s*if \(!SceneRig_ObjectExists\(object\)\) continue;/,
    'the dock lists only objects in the scene');
  assert.match(rigCpp, /bool SceneRig_ObjectExists\(int id\) \{ return Find\(id\) != nullptr; \}/);
  const context = { window: { MvmSceneCatalog: [] } };
  vm.createContext(context);
  vm.runInContext(mockKeys, context);
  const K = context.window.MvmSceneKeys;
  const sk = K.create();
  K.put(sk, K.objectSpec(4, 0), 100, 10);
  K.put(sk, K.objectSpec(5, 0), 100, 20);
  sk.objectNames = { 4: 'Camera 4' };
  K.sync(sk, 100);
  const objects = new Set(sk.sceneChannels.filter((c) => c.family === 'objects').map((c) => c.fieldId.split('.')[1]));
  assert.deepEqual([...objects], ['4'], 'object 5 is gone from the scene, so from the dock');
  assert.ok(sk.byField['obj.5.x'].length === 1, 'its keys are kept');
});

test('every scene camera and light has its TIMELINE layer with all its settings, not animated until asked', () => {
  const keysCppNow = read('AfxHookSource2/Filmmaker/Movie/SceneKeys.cpp');
  assert.match(keysCppNow, /for \(int id : SceneRig_TimelineObjects\(\)\) objects\.insert\(id\);/);
  assert.match(keysCppNow, /if \(!live && !always\) return;/);
  assert.match(rigMath, /enum LightChannel : int \{ Intensity = kPoseChannels, Range, Cone, Radius, Width, Height, kObjectChannels \};/);
  assert.match(graph, /\['intensity', 'Brightness'/);
  assert.match(graph, /else if \(oid && !objectLive\(L\)\) rawCmd\('mirv_filmmaker rig animate ' \+ oid \+ ' on'\);/,
    'the switch of a layer that is not animated yet animates it');
  assert.match(graph, /\{ id: 'remove', label: 'Delete object'/, 'deleting an object layer removes the object');
  assert.match(graph, /rawCmd\('mirv_filmmaker environment add ' \+ id\.substring\(4\)\);/, 'right-click > Add');
  const context = { window: { MvmSceneCatalog: [] } };
  vm.createContext(context);
  vm.runInContext(mockKeys, context);
  const K = context.window.MvmSceneKeys;
  const sk = K.create();
  sk.objectNames = { 3: 'Spot light 3', 4: 'Camera 4' }; sk.objectKinds = { 3: 'spot', 4: 'camera' };
  K.sync(sk, 100);
  const rows = (id) => sk.sceneChannels.filter((c) => c.fieldId.indexOf('obj.' + id + '.') === 0).map((c) => c.name);
  assert.deepEqual(rows(3), ['X', 'Y', 'Z', 'Pitch', 'Yaw', 'Roll', 'Brightness', 'Range', 'Beam angle', 'Emitter size']);
  assert.deepEqual(rows(4), ['X', 'Y', 'Z', 'Pitch', 'Yaw', 'Roll', 'Field of view']);
  assert.ok(sk.sceneChannels.every((c) => c.live === false), 'listed, not animated');
});
