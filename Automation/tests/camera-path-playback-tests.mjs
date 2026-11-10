// J and the armed path must land on the first key through the scrub controller and start only once
// the world is there. A bare demo_gototick plus the demo_pause toggle let a playing demo run on under
// a camera held at the first key, so the path then started mid-way or ended at once (user report
// 2026-11-08; measured: armed at 12000, world at 12281 five seconds later).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (rel) => fs.readFileSync(path.join(root, rel), 'utf8');
const camPath = read('AfxHookSource2/Filmmaker/Movie/CameraPath.cpp');
const movie = read('AfxHookSource2/Filmmaker/Movie/MovieMode.cpp');
const body = (name) => {
  const start = camPath.indexOf(`void CameraPath::${name}(`);
  assert.ok(start >= 0, `${name} exists`);
  return camPath.slice(start, camPath.indexOf('\nvoid CameraPath::', start + 10));
};

test('arming lands through the scrub controller and never toggles the pause itself', () => {
  const arm = body('ArmPreview');
  assert.match(arm, /DemoScrub_Release\(\(double\)m_data\.Front\(\)\.tick, \/\*resume\*\/ false\)/);
  assert.doesNotMatch(arm, /SeekDemoTick\(|DemoCmd\("demo_pause"\)/);
});

test('the editor play lands the same way on a demo that may be playing', () => {
  const play = body('PlayFromEditor');
  assert.match(play, /DemoScrub_Release\(tick, \/\*resume\*\/ false\)/);
  assert.doesNotMatch(play, /SeekDemoTick\(/);
});

test('a play that beats the landing waits for it', () => {
  const start = body('StartPreviewPlay');
  assert.match(start, /if \(DemoScrub_Active\(\)\) \{\s*m_timelinePlayPending = true;/);
  assert.match(camPath, /if \(DemoScrub_Active\(\)\) m_timelinePlayWaitFrames = 0;/, 'the fallback never fires mid-landing');
});

test('J plays from the first key in one press and stops on the next', () => {
  assert.match(movie, /marker playfromstart/);
  assert.match(movie, /cpMode == CPMode::PreviewPlaying && !CameraPathRef\(\)\.AutoPlaying\(\)\)\s*\|\| \(cpMode == CPMode::PreviewArmed && CameraPathRef\(\)\.PlaybackPending\(\)\)/);
  assert.match(read('AfxHookSource2/Filmmaker/MarkerCommand.cpp'), /"playfromstart"\)\) \{[\s\S]{0,260}FollowCameraRef\(\)\.StopPreview\("camera path started"\);\s*cp\.PlayFromFirstKey\(\);/,
    'J hands the view to the path: a Follow Camera preview stops, as it does for play');
  assert.match(read('AfxHookSource2/Filmmaker/Panorama/CameraEditorJs.h'), /\[K\('path\.play', 'J'\), T\('play path'\)\]/);
  assert.doesNotMatch(read('AfxHookSource2/Filmmaker/Panorama/MarkerHudJs.h'), /Arm the path/);
});

test('stepping the camera with the wheel ends a path instead of leaving it running unseen', () => {
  const wheel = movie.slice(movie.indexOf('bool MovieMode::OnMouseWheel('), movie.indexOf('void MovieMode::SetUiClickRegions('));
  assert.match(wheel, /PreviewPlaying \|\| cpMode == CameraPath::Mode::PreviewArmed\) \{\s*EnqueueCmd\("mirv_filmmaker marker previewstop"\)/);
});

test('a demo playing through a path with AUTO off says how to watch the path', () => {
  assert.match(body('HintWhenPathIsNotPlaying'), /Press J to play the path/);
});

test('AUTO is on until the user turns it off', () => {
  assert.match(camPath, /FilmmakerSettings_UiBool\("editor\.timeline\.autopath", true\)/);
});

test('AUTO follows with the Camera Editor open, starts inside the keys, and stays stopped when stopped', () => {
  const run = body('RunAutoPath');
  assert.match(run, /CameraPath_AutoPath\(\) && \(GraphEditorExperiment_Enabled\(\) \|\| CameraEditor_Active\(\)\)/);
  assert.match(run, /tick < FirstPathTick\(\) \|\| tick >= LastPathTick\(\)\) return;/);
  assert.match(run, /if \(m_autoStandDown\) \{/);
  assert.match(body('StopPreview'), /if \(was\) m_autoStandDown = true;/);
});

test('stopping a path that is still landing cancels the landing, and only then', () => {
  const stop = body('StopPreview');
  assert.match(stop, /if \(GetMode\(\) == Mode::PreviewArmed\) DemoScrub_Cancel\(\);/);
  assert.ok(stop.indexOf('DemoScrub_Cancel') < stop.indexOf('SetMode(Mode::Editing'), 'read the mode before it changes');
  assert.match(body('FinishPlaybackAtLastKey'), /DemoScrub_Release\(\(double\)lastTick, false\);\s*\}\s*StopPreview\(\);/);
});

test('a frame with no depth does not keep an older frame bound', () => {
  const addon = read('ReShadeStateAddon/mvm_reshade_state.cpp');
  assert.match(addon, /\} else if \(g_depthView\.handle != 0\) \{\s*\/\/ No depth this frame[^\n]*\s*runtime->update_texture_bindings\("DEPTH", resource_view\{ 0 \}, resource_view\{ 0 \}\);\s*ReleaseDepthView\(dev\);/);
});
