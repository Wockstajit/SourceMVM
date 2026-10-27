// Every CS2 module that reads or writes a project.mvm section must key it by the ORIGINAL demo.
// PlayingDemoPath() maps a keyframed or repaired copy back to the demo it was made from;
// ResolvePlayingDemoPath() is the raw file the engine has open. Keying a section off the raw
// file put scene keys edited on keyframed.dem into a project folder named `keyframed.dem`, lost
// to the original demo.
//
// Run: node --test Automation/tests/demo-project-keying-tests.mjs

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const filmmakerDirectory = path.join(repositoryRoot, "AfxHookSource2", "Filmmaker");

function sourceFiles(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const full = path.join(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(full);
    return entry.name.endsWith(".cpp") ? [full] : [];
  });
}

const sectionUsers = sourceFiles(filmmakerDirectory)
  .map((file) => ({ file: path.relative(repositoryRoot, file), text: fs.readFileSync(file, "utf8") }))
  .filter(({ text }) => /DemoProjectStore_(Read|Write)Section\(\s*ProjectGame::Cs2/.test(text));

test("the CS2 project sections are found", () => {
  const names = sectionUsers.map(({ file }) => path.basename(file));
  for (const expected of ["CameraPath.cpp", "PlaybackLoop.cpp", "SceneKeys.cpp", "CosmeticDemoProfiles.cpp"])
    assert.ok(names.includes(expected), `${expected} writes a project section`);
});

// Path shake is shared with CS:S, so it asks its game host for the store and the key
// (CamPathShakeHost.h). CS2's host must hand it CS2's store, keyed by the original demo.
test("path shake keys its section through its host, and CS2's host keys it by the original demo", () => {
  const shake = fs.readFileSync(path.join(filmmakerDirectory, "Movie", "CamPathShake.cpp"), "utf8");
  const host = fs.readFileSync(path.join(filmmakerDirectory, "Movie", "CamPathShakeHostSource2.cpp"), "utf8");
  assert.match(shake, /DemoProjectStore_WriteSection\(PathShakeHost_Game\(\), s\.demoPath,/);
  assert.match(shake, /DemoProjectStore_ReadSection\(PathShakeHost_Game\(\), s\.demoPath,/);
  assert.match(shake, /std::wstring cur = PathShakeHost_DemoPath\(\);/);
  assert.match(host, /std::wstring PathShakeHost_DemoPath\(\) \{ return PlayingDemoPath\(\); \}/);
  assert.match(host, /ProjectGame PathShakeHost_Game\(\) \{ return ProjectGame::Cs2; \}/);
  assert.ok(!/ResolvePlayingDemoPath\(/.test(host), "CS2's path shake host must not key by the raw playing file");
});

test("every CS2 project section is keyed by the original demo, never a copy", () => {
  for (const { file, text } of sectionUsers) {
    assert.ok(/[^A-Za-z_]PlayingDemoPath\(\)/.test(text), `${file} keys its section by PlayingDemoPath()`);
    assert.ok(!/ResolvePlayingDemoPath\(/.test(text),
      `${file} must not key a project section by the raw playing file`);
  }
});

// The copy's .src beside it is what maps it back to its demo. CS2 opens a demo with
// FILE_SHARE_DELETE, so deleting the copy it plays succeeds and takes the .src with it: the camera
// path then took the demo for a new one and dropped every marker (2026-10-17). The keyframe builder
// must leave the slot the engine plays alone, at the start of a build, at its end and in KeepOnly.
test("the keyframe builder never deletes or overwrites the copy the engine is playing", () => {
  const text = fs.readFileSync(path.join(filmmakerDirectory, "Demo", "DemoKeyframes.cpp"), "utf8");
  assert.match(text, /if \(SamePath\(SlotPath\(demoPath, target, false\), PlayingFile\(\)\)\)/,
    "a build whose target slot is being played does not start");
  assert.match(text, /if \(!SamePath\(superseded, PlayingFile\(\)\)\) DeleteSlot\(superseded\);/,
    "the superseded slot stays while it is played");
  assert.match(text, /DemoKeyframes_KeepOnly\(demoPath, PlayingFile\(\)\);/,
    "the build's tidy-up knows which file is playing");
  assert.doesNotMatch(text, /DemoKeyframes_KeepOnly\(demoPath, std::wstring\(\)\)/);
  assert.match(text, /if \(SamePath\(copy, playingFile\)\) continue;/,
    "another demo's slot that is being played is not deleted either");
});

// An alternative-environment copy plays in place of the demo, so it must map back the same way: its
// .src marker starts with the ORIGINAL demo's size:mtime and a '|', which is all
// DemoProjectPath.cpp MarkerMatchesSource reads.
test("an environment copy's marker maps it back to the original demo", () => {
  const text = fs.readFileSync(path.join(filmmakerDirectory, "AltEnvironment", "AltEnvironmentCopies.cpp"), "utf8");
  assert.match(text, /out << FileStamp\(original\) << "\|" << kMarkerVersion/,
    "the marker's first field is the original's stamp");
  const lookup = fs.readFileSync(path.join(filmmakerDirectory, "Demo", "DemoProjectStoreMath.h"), "utf8");
  assert.match(lookup, /IsEnvironmentCopyFileName\(fileName\)/, "environment slots are project-folder copies");
});
