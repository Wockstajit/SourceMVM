// Unit tests for Automation/verify/ci-change-scope.mjs, which decides which CI jobs a change runs
// and whether ci-gate passes. Run: node --test Automation/tests/ci-change-scope-tests.mjs

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { classifyFiles, decideScope, evaluateGate, requiredJobs } from "../verify/ci-change-scope.mjs";

const repoRoot = fileURLToPath(new URL("../../", import.meta.url));
const none = { native: "none", launcher: false, panorama: false };

test("documentation, website and script-only changes run no component jobs", () => {
  assert.deepEqual(
    classifyFiles([
      "Docs/setup/git-workflow.md",
      "README.md",
      "Website/index.html",
      "FX/tools/port-pack.py",
      "Automation/tests/config-reset-tests.mjs",
      "Automation/launch/launch-demo.ps1",
      "UiMockup/index.html",
    ]),
    none,
  );
});

test("legacy HLAE trees are left to the nightly legacy workflow", () => {
  assert.deepEqual(
    classifyFiles(["AfxHookSource/main.cpp", "AfxHookGoldSrc/hooks.cpp", "HLAE/Program.cs", "Installer/setup.wxs"]),
    none,
  );
});

test("CS2 and shared native sources build both games", () => {
  for (const file of [
    "AfxHookSource2/main.cpp",
    "AfxHookSource2/Filmmaker/Movie/GraphExpModel.h",
    "Shared/RawOutput.cpp",
    "Lib/afx_hook_source2/src/lib.rs",
    "Deps/release/prop",
    "Shaders/afx_line_vs_5_0.fxc",
    "ShaderBuilder/Program.cs",
    "Misc/mirv-script/src/snippets/mirv_script_aim.ts",
    "FilmmakerDemoInfoGo/main.go",
    "FilmmakerDemoKeyframesGo/st2/parser.go",
    "CMake/Source2Tests.cmake",
    "Automation/tests/follow-camera-math-tests.cpp",
    "Automation/tools/prepare-environment-shaders.cpp",
    "CMakeLists.txt",
    "CMakePresets.json",
    "Cargo.lock",
  ]) {
    assert.equal(classifyFiles([file]).native, "all", file);
  }
});

test("Deadlock-only changes build and test only the Deadlock targets", () => {
  assert.equal(
    classifyFiles([
      "AfxHookSource2/Deadlock/DeadlockCommands.cpp",
      "AfxHookSource2/Deadlock/CMakeLists.txt",
      "AfxHookSource2/Filmmaker/Deadlock/DeadlockDock.cpp",
      "Automation/tests/deadlock-view-math-tests.cpp",
    ]).native,
    "deadlock",
  );
});

test("a Deadlock change together with any shared change builds both games", () => {
  assert.equal(
    classifyFiles(["AfxHookSource2/Deadlock/DeadlockCommands.cpp", "AfxHookSource2/main.cpp"]).native,
    "all",
  );
  assert.equal(
    classifyFiles(["AfxHookSource2/main.cpp", "AfxHookSource2/Deadlock/DeadlockCommands.cpp"]).native,
    "all",
  );
});

test("Deadlock JS tests and Python tools are not native inputs", () => {
  assert.equal(classifyFiles(["Automation/tests/deadlock-mockup-tests.mjs"]).native, "none");
  assert.equal(classifyFiles(["Automation/tests/deadlock-view-chain-tests.py"]).native, "none");
});

test("Panorama headers run the Panorama tests and the native build", () => {
  assert.deepEqual(classifyFiles(["AfxHookSource2/Filmmaker/Panorama/CameraEditorWidgetsJs.h"]), {
    native: "all",
    launcher: false,
    panorama: true,
  });
  assert.equal(classifyFiles(["Automation/tools/generate_cosmetics_catalog.py"]).panorama, true);
  assert.equal(classifyFiles(["Automation/tests/panorama-interaction-layout-tests.mjs"]).panorama, true);
});

test("launcher changes run only the launcher job", () => {
  for (const file of [
    "SourceMovieMakerLauncher/electron/main.cjs",
    "SourceMovieMakerLauncher/host/LaunchDiagnostics.cs",
    "Automation/tests/launcher-launch-host-tests.mjs",
    "Automation/tests/launcher-host-tests.ps1",
  ]) {
    assert.deepEqual(classifyFiles([file]), { native: "none", launcher: true, panorama: false }, file);
  }
});

test("the injector protocol submodule feeds the launcher and the native build", () => {
  assert.deepEqual(classifyFiles(["Deps/release/injector"]), { native: "all", launcher: true, panorama: false });
  assert.deepEqual(classifyFiles([".gitmodules"]), { native: "all", launcher: true, panorama: false });
});

test("changes to the CI definition run every job", () => {
  const everything = { native: "all", launcher: true, panorama: true };
  assert.deepEqual(classifyFiles([".github/workflows/ci.yml"]), everything);
  assert.deepEqual(classifyFiles(["Docs/README.md", "Automation/verify/ci-change-scope.mjs"]), everything);
});

test("Windows separators are normalized", () => {
  assert.equal(classifyFiles(["AfxHookSource2\\Deadlock\\DeadlockCommands.cpp"]).native, "deadlock");
});

test("nightly, manual and release runs use the full shipping build", () => {
  for (const scope of [
    decideScope({ event: "schedule", files: null }),
    decideScope({ event: "workflow_dispatch", files: null }),
    decideScope({ event: "push", ref: "refs/heads/release/0.2", files: ["Docs/README.md"] }),
    decideScope({ event: "pull_request", baseRef: "release/0.2", files: ["Docs/README.md"] }),
  ]) {
    assert.equal(scope.mode, "full", scope.reason);
    assert.equal(scope.native, "all");
    assert.equal(scope.launcher, true);
    assert.equal(scope.panorama, true);
  }
});

test("merge queue groups build and test everything on the cached path", () => {
  const scope = decideScope({ event: "merge_group", files: ["Docs/README.md"] });
  assert.equal(scope.mode, "fast");
  assert.deepEqual([scope.native, scope.launcher, scope.panorama], ["all", true, true]);
});

test("pull requests and main pushes are filtered by their changed files", () => {
  const pr = decideScope({ event: "pull_request", baseRef: "main", files: ["Docs/README.md"] });
  assert.deepEqual([pr.mode, pr.native, pr.launcher, pr.panorama], ["fast", "none", false, false]);
  const push = decideScope({
    event: "push",
    ref: "refs/heads/main",
    files: ["AfxHookSource2/Deadlock/DeadlockCommands.cpp"],
  });
  assert.deepEqual([push.mode, push.native], ["fast", "deadlock"]);
});

test("an unknown change list falls back to running everything", () => {
  for (const event of ["push", "pull_request", "repository_dispatch"]) {
    const scope = decideScope({ event, ref: "refs/heads/main", files: null });
    assert.deepEqual([scope.mode, scope.native, scope.launcher, scope.panorama], ["fast", "all", true, true], event);
  }
});

const outputs = (overrides = {}) => ({
  mode: "fast",
  native: "none",
  launcher: "false",
  panorama: "false",
  ...overrides,
});
const needsFor = (scope, results) => ({
  changes: { result: "success", outputs: scope },
  "docs-integrity": { result: "success" },
  "panorama-tests": { result: "skipped" },
  "launcher-build": { result: "skipped" },
  "native-build-windows": { result: "skipped" },
  "native-full-validation": { result: "skipped" },
  "security-license-scan": { result: "success" },
  "changed-file-policy": { result: "success" },
  ...Object.fromEntries(Object.entries(results).map(([job, result]) => [job, { result }])),
});

test("the gate passes a docs-only pull request with the component jobs skipped", () => {
  assert.deepEqual(evaluateGate(needsFor(outputs(), {}), "pull_request"), []);
});

test("the gate fails a native change whose native job did not run", () => {
  const failures = evaluateGate(needsFor(outputs({ native: "deadlock" }), {}), "pull_request");
  assert.deepEqual(failures, ["native-build-windows: skipped, but this change needs it"]);
});

test("the gate fails on any failed or cancelled job, needed or not", () => {
  assert.deepEqual(evaluateGate(needsFor(outputs(), { "panorama-tests": "failure" }), "pull_request"), [
    "panorama-tests: failure",
  ]);
  assert.deepEqual(evaluateGate(needsFor(outputs(), { "docs-integrity": "cancelled" }), "push"), [
    "docs-integrity: cancelled",
  ]);
});

test("full runs require the shipping build instead of the cached one", () => {
  const full = outputs({ mode: "full", native: "all", launcher: "true", panorama: "true" });
  const passing = { "panorama-tests": "success", "launcher-build": "success" };
  assert.deepEqual(
    evaluateGate(needsFor(full, { ...passing, "native-full-validation": "success" }), "schedule"),
    [],
  );
  assert.deepEqual(evaluateGate(needsFor(full, { ...passing, "native-build-windows": "success" }), "schedule"), [
    "native-full-validation: skipped, but this change needs it",
  ]);
});

test("changed-file-policy is required on pull requests only", () => {
  assert.deepEqual(evaluateGate(needsFor(outputs(), { "changed-file-policy": "skipped" }), "push"), []);
  assert.deepEqual(evaluateGate(needsFor(outputs(), { "changed-file-policy": "skipped" }), "pull_request"), [
    "changed-file-policy: skipped, but this change needs it",
  ]);
});

test("the gate fails when the scope job failed or reported nonsense", () => {
  const failed = needsFor(outputs(), {});
  failed.changes = { result: "failure", outputs: {} };
  assert.deepEqual(evaluateGate(failed, "pull_request"), ["changes: failure"]);
  const odd = needsFor(outputs({ native: "maybe" }), {});
  assert.equal(evaluateGate(odd, "pull_request").length, 1);
});

test("required jobs follow the scope", () => {
  assert.deepEqual(
    [...requiredJobs(outputs({ native: "all", launcher: "true" }), "push")].sort(),
    ["changes", "docs-integrity", "launcher-build", "native-build-windows", "security-license-scan"],
  );
});

test("the ci-gate job lists every job the gate reasons about", () => {
  const workflow = readFileSync(new URL("../../.github/workflows/ci.yml", import.meta.url), "utf8");
  const gate = workflow.slice(workflow.indexOf("\n  ci-gate:"));
  const needsBlock = gate.slice(gate.indexOf("needs:"), gate.indexOf("runs-on:"));
  for (const job of Object.keys(needsFor(outputs(), {}))) {
    assert.match(needsBlock, new RegExp(`- ${job}\\b`), `ci-gate needs ${job}`);
    assert.match(workflow, new RegExp(`\\n  ${job}:\\n`), `${job} is a job in ci.yml`);
  }
});

// The Deadlock-only scope skips the CS2 DLL and tests. That is safe only while no CS2 build file or
// non-Deadlock source refers to the Deadlock-only folders.
test("CS2 build files and sources never reach into the Deadlock-only folders", () => {
  for (const file of ["AfxHookSource2/CMakeLists.txt", "CMake/Source2Tests.cmake"]) {
    const source = readFileSync(new URL(`../../${file}`, import.meta.url), "utf8");
    assert.doesNotMatch(source, /Deadlock\//, `${file} references a Deadlock folder`);
  }
  const tracked = execFileSync("git", ["ls-files", "-z", "AfxHookSource2", "Shared", "Automation/tests", "Automation/tools"], {
    cwd: repoRoot,
    encoding: "utf8",
  })
    .split("\0")
    .filter((file) => /\.(c|cpp|h|hpp|inc)$/.test(file));
  const offenders = tracked.filter((file) => {
    if (classifyFiles([file]).native !== "all") return false;
    const source = readFileSync(new URL(`../../${file}`, import.meta.url), "utf8");
    return /#\s*include\s*["<][^">]*Deadlock\//.test(source);
  });
  assert.deepEqual(offenders, []);
});
