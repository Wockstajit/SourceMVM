// Work asked for one demo or one replacement map must not land on the next (reviews on #105): a copy
// that finishes after another demo was opened, an AUTO ALIGN result after the demo or the picked map
// changed, and a PLAY still waiting for the previous map's picture. Static checks against
// AltEnvironment.cpp, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const src = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironment.cpp', import.meta.url), 'utf8');
const fn = (name) => {
  const start = src.indexOf('\n' + name);
  assert.ok(start >= 0, name + ' is defined');
  return src.slice(start, src.indexOf('\n}\n', start));
};

test('a finished copy plays only while its demo is still the one last opened', () => {
  assert.match(fn('std::wstring ResolvePlayable('), /s\.lastOpen = original;/);
  assert.match(fn('void OpenDemo('), /s\.lastOpen = original;/);
  const finish = fn('void FinishBuild(');
  assert.match(finish, /const bool playNow = b->playWhenDone && Copies::SameFile\(s\.lastOpen, b->original\);/);
  assert.doesNotMatch(finish, /if \(b->playWhenDone\) PlayResolvedDemo|else if \(b->playWhenDone\)/);
  assert.equal((finish.match(/PlayResolvedDemo\(/g) || []).length, 2);
  assert.equal((finish.match(/if \(playNow\) PlayResolvedDemo\(|else if \(playNow\) \{\s*PlayResolvedDemo\(/g) || []).length, 2);
});

test('opening a demo or picking another map voids AUTO ALIGN and a waiting PLAY', () => {
  assert.match(fn('void OpenDemo('), /\+\+s\.selection;/);
  const changed = fn('void SelectionChanged(');
  assert.match(changed, /\+\+s\.selection;/);
  assert.match(changed, /s\.applyPending = false;/);
  assert.match(changed, /s\.autoPending = false;/);
  const pick = fn('void Pick(');
  assert.match(pick, /if \(!s\.pick\.map\.empty\(\)\) SelectionChanged\(\);/);
  assert.match(pick, /if \(m\.map != s\.pick\.map \|\| m\.addon != s\.pick\.addon\) \{\s*SelectionChanged\(\);/);
  assert.match(fn('void StartAutoAlign('), /s\.autoJobSelection = s\.selection;/);
  assert.match(src, /const bool stale = s\.autoJobSelection != s\.selection;/);
});
