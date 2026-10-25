// The chime when a render ends (Render/RenderDoneSound.cpp) plays one buffer, g_pcm, on its own
// XAudio2 voice. A second play within the 0.95 s chime used to rewrite g_pcm while XAudio2 could still
// be reading it: Stop and FlushSourceBuffers do not wait, and the buffer being played stays in use
// (review on #103). Static checks against the source, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const src = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Render/RenderDoneSound.cpp', import.meta.url), 'utf8');
const playNow = src.slice(src.indexOf('void PlayNow('), src.indexOf('\n}\n', src.indexOf('void PlayNow(')));

test('a chime still queued is cut off by destroying its voice before the buffer is rewritten', () => {
  const destroy = playNow.search(/if \(state\.BuffersQueued > 0\) \{\s*g_voice->DestroyVoice\(\);\s*g_voice = nullptr;/);
  const synth = playNow.indexOf('Synthesize(finished, g_pcm);');
  assert.ok(destroy >= 0, 'a queued buffer destroys the voice');
  assert.ok(synth > destroy, 'g_pcm is only rewritten after that');
  assert.ok(playNow.indexOf('EnsureVoice()') > destroy && playNow.indexOf('EnsureVoice()') < synth, 'a new voice is made in between');
  assert.doesNotMatch(playNow, /FlushSourceBuffers\(\)/, 'no flush that leaves the playing buffer in use');
});
