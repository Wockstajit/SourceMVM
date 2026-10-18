// A launch from a git worktree has every private local asset the main checkout has.
//
// FX\compiled (the VFX packs), FX\death-animations (the ragdoll clips) and a stage's ffmpeg setup are
// git-ignored, so a worktree has none of them. Test launches from one started CS2 with no VFX mounted,
// no ragdoll clips and no encoder ("refusing a completed manifest" on every render), 2026-10-17.
// Static checks against the launcher, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) => fs.readFileSync(new URL('../' + name, import.meta.url), 'utf8');
const common = read('lib/AutomationCommon.ps1');
const launch = read('launch/launch-cs2-netcon.ps1');

test('the main checkout is found from a worktree', () => {
  assert.match(common, /function Get-PrivateAssetRoots/);
  assert.match(common, /rev-parse --path-format=absolute --git-common-dir/);
  assert.match(common, /function Find-PrivateAsset/);
});

test('the VFX pack comes from whichever checkout has it', () => {
  assert.match(launch, /Find-PrivateAsset -RepositoryRoot \$root -RelativePath 'FX\\compiled\\source_mvm_fx\\particles'/);
  assert.match(launch, /Seeding this checkout's FX stage from/);
  assert.doesNotMatch(launch, /\$committedFx = Join-Path \$root 'FX\\compiled\\source_mvm_fx'/);
});

test('the ragdoll library is staged next to the hook', () => {
  assert.match(launch, /Find-PrivateAsset -RepositoryRoot \$root -RelativePath 'FX\\death-animations\\ragdoll_animations\.mvmanim'/);
  assert.match(launch, /resources\\ragdoll\\ragdoll_animations\.mvmanim/);
});

test('a stage without a reachable FFmpeg borrows one', () => {
  assert.match(launch, /function Test-StagedFfmpeg/);
  assert.match(launch, /if \(-not \(Test-StagedFfmpeg \$bin\)\)/);
  assert.match(launch, /Staged the FFmpeg setup from/);
});
