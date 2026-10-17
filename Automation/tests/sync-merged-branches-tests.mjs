import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';

const read = path => readFileSync(new URL(`../../${path}`, import.meta.url), 'utf8');
const script = read('Automation/tools/sync-merged-branches.ps1');

test('a branch is retired only for a merged PR head or with nothing outside main', () => {
  assert.match(script, /--repo \$repo --state \$state/);
  assert.match(script, /\$tip -eq \$pr\.headRefOid/);
  assert.match(script, /'merge-base', '--is-ancestor', \$tip, \$pr\.headRefOid/);
  assert.match(script, /if \(\$open\[\$branch\]\) \{ Write-Result 'kept'/, 'an open PR keeps its branch');
  assert.match(script, /if \(\$ahead -gt 0\) \{[\s\S]*?continue/, 'unmerged commits keep the branch');
  assert.match(script, /no commits yet, but a worktree uses it/, 'a fresh worktree branch is not retired');
});

test('a worktree with any unsaved work is kept, and ignored notes are moved out first', () => {
  const remove = script.indexOf("'worktree', 'remove'");
  for (const guard of ["'status', '--porcelain', '--ignore-submodules=none'", "'submodule', 'foreach'", 'Save-Leftover $path']) {
    const at = script.indexOf(guard);
    assert.ok(at > 0 && at < remove, `${guard} must run before the worktree is removed`);
  }
  assert.match(script, /Automation\\runs\\worktree-leftovers\\/);
  assert.match(script, /if \(Get-Worktrees \$mainRoot \| Where-Object \{ \$_\.Path -eq \$path \}\)/,
    'a folder left open by another program does not stop the run once git unregistered the worktree');
  assert.match(script, /if \(\$entry -notmatch \$rebuildable\)/);
  assert.doesNotMatch(script, /\$rebuildable = .*(handoff|Automation\/(runs|output)|generated|\\\.claude)/,
    'notes, run output, generated UI drafts and agent configs are never treated as rebuildable');
});

test('main only fast-forwards and nothing is pushed', () => {
  assert.match(script, /'merge', '--ff-only'/);
  assert.match(script, /'fetch', '--quiet', 'origin', 'main:main'/);
  assert.doesNotMatch(script, /'push'|reset --hard|'reset'/);
});

test('the workflow docs tell whoever merges to run it', () => {
  for (const doc of ['CLAUDE.md', 'Docs/setup/git-workflow.md', 'Docs/setup/finishing-work.md']) {
    assert.match(read(doc), /sync-merged-branches\.ps1/, doc);
    assert.match(read(doc), /stacked-pull-requests/, doc);
  }
});
