// The scrub frame cache must never show a frame that no longer matches what the camera renders.
// These pin the three invalidation paths review found missing (#36).
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

const read = (p) => fs.readFileSync(new URL(`../../${p}`, import.meta.url), 'utf8');
const hooks = read('AfxHookSource2/RenderSystemDX11Hooks.cpp');
const main = read('AfxHookSource2/main.cpp');
// CS2's answers to the cache (ScrubFrameCacheHost.h), the picture revision among them.
const cacheHost = read('AfxHookSource2/Filmmaker/Movie/ScrubFrameCacheHostSource2.cpp');
const shake = read('AfxHookSource2/Filmmaker/Movie/CamPathShake.cpp');

test('the pose check compares the FOV the frame rendered with, before the shake layers', () => {
  const take = hooks.slice(hooks.indexOf('static ScrubCacheFrame EngineThread_TakeScrubFrame()'));
  const actual = take.slice(0, take.indexOf('PoseNear'));
  assert.match(actual, /g_CurrentGameCamera\.fov \};/);
  assert.doesNotMatch(actual, /frame\.id\.pose\[6\] \}/, 'copying the published FOV makes the check pass by definition');
  assert.match(main, /const float preShakeFov = Fov;/);
  assert.ok(main.indexOf('const float preShakeFov = Fov;') < main.indexOf('CamPathShake_ApplyToView('),
    'captured before the per-key shake adds its FOV');
  assert.match(main, /g_CurrentGameCamera\.fov = preShakeFov;/);
});

test('turning the cache off frees its frames even while no preview is active', () => {
  const hold = hooks.slice(hooks.indexOf('static bool HoldOrCapturePreview('));
  const inactive = hold.slice(0, hold.indexOf('GetBuffer('));
  assert.match(inactive, /if \(!active \|\| recording \|\| !g_pSwapChain \|\| !g_pDevice\) \{[\s\S]*?lock_guard<std::mutex> lock\(s_CacheMutex\);\s*ReleaseCacheIfDisabled\(\);\s*return false;/);
  assert.match(hooks, /if \(!s_CacheEnabled\) \{\s*ReleaseCacheIfDisabled\(\);\s*return false;/);
});

test('a camera-shake edit changes the picture revision', () => {
  const revision = cacheHost.slice(cacheHost.indexOf('uint64_t ScrubCacheHost_PictureRevision()'), cacheHost.indexOf('return h ? h : 1;'));
  assert.match(revision, /Mix\(h, CamPathShake_Revision\(\)\)/);
  const publish = shake.slice(shake.indexOf('void PublishSnapshot()'), shake.indexOf('bool ReadSnapshot('));
  for (const field of ['k.tick', 'k.style', 'k.intensity', 'k.speed']) assert.ok(publish.includes(field), field);
  assert.match(publish, /if \(s\.enabled && any\)/, 'the master switch and an empty shake also change it');
  assert.match(publish, /g_snapRevision = rev;/);
});
