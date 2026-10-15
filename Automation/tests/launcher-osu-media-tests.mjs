// The osu! easter egg's bundled songs and skin (bin\osu\) and the UwU sounds (bin\easter-eggs\) are
// read from beside the hook (GetHlaeFolder()), so every package that ships the hook must ship them
// too -- otherwise a player without osu! installed gets an empty song list and no skin.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (rel) => readFileSync(path.join(repo, rel), 'utf8');

for (const rel of [
  'SourceMovieMakerLauncher/scripts/build-bundle.ps1',
  'SourceMovieMakerLauncher/scripts/build-release-payload.ps1',
  'Automation/setup/package-portable.ps1',
]) {
  test(`${rel} ships osu\\ and easter-eggs\\ beside the hook`, () => {
    const src = read(rel);
    assert.match(src, /@\([^)]*'resources'[^)]*'osu'[^)]*'easter-eggs'[^)]*\)/);
  });
}

test('the hook looks for bundled osu! media beside its x64 folder', () => {
  assert.match(read('AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuPlatform.cpp'), /BundledRoot\(\) \{ return HlaeBin\(\) \/ L"osu"; \}/);
  assert.match(read('AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuSkin.cpp'), /BundledRoot\(\) \/ L"Skin"/);
  assert.match(read('AfxHookSource2/Filmmaker/EasterEggs/Osu/OsuLibrary.cpp'), /BundledRoot\(\) \/ L"Songs"/);
  assert.match(read('Automation/tools/stage-easter-egg-media.ps1'), /'bin\\osu\\Songs'/);
  assert.match(read('Automation/tools/stage-easter-egg-media.ps1'), /'bin\\osu\\Skin'/);
});
