#!/usr/bin/env node
// Decides which CI jobs a change needs, and whether the aggregated result may pass. Used by the
// `changes` and `ci-gate` jobs in .github/workflows/ci.yml. See Docs/setup/ci.md.
//
//   node Automation/verify/ci-change-scope.mjs --scope
//       Reads the GitHub event from the environment, lists the changed files, and writes
//       mode, native, launcher and panorama to $GITHUB_OUTPUT (stdout when unset).
//   node Automation/verify/ci-change-scope.mjs --gate
//       Reads the aggregated job results from NEEDS_JSON and fails when any job failed or was
//       cancelled, or when a job this change needed did not run.
//
// Every rule errs toward running more. A path no rule recognizes runs nothing extra, so a new
// native input directory must be added to NATIVE_RULES below and to the tests.

import { spawnSync } from "node:child_process";
import { appendFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = fileURLToPath(new URL("../../", import.meta.url));

// Changes to the CI definition itself run every job, so the workflow validates itself.
const EVERYTHING_RULES = [
  /^\.github\/workflows\/ci\.yml$/,
  /^Automation\/verify\/ci-change-scope\.mjs$/,
];

// Inputs of the x64 CS2 and Deadlock build: CMake, the hooks, their dependencies and generated
// inputs, the Rust crate, the Go demo helpers, the native tests and the build-time tools.
const NATIVE_RULES = [
  /^(AfxHookSource2|Shared|Lib|Deps|Shaders|ShaderBuilder|Misc|Resources|CMake)\//,
  /^FilmmakerDemo[A-Za-z]*Go\//,
  /^Automation\/(tests|tools)\/[^/]+\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|inc)$/,
  /^(CMakeLists\.txt|CMakePresets\.json|Cargo\.toml|Cargo\.lock|\.gitmodules|rust-toolchain(\.toml)?)$/,
];

// Files only the Deadlock targets compile. The CS2 target and its tests never reference these
// folders (ci-change-scope-tests.mjs checks that), so a change limited to them skips the CS2 DLL,
// its shader bundle and its tests. Any other native change builds and tests both games, because
// AfxHookDeadlock compiles many CS2 sources itself.
const DEADLOCK_ONLY_RULES = [
  /^AfxHookSource2\/(Filmmaker\/)?Deadlock\//,
  /^Automation\/tests\/deadlock-[^/]+\.(cpp|h|hpp|inc)$/,
];

// launcher-build: the Electron app, the injector protocol submodule it builds against, and the
// launcher tests the job runs.
const LAUNCHER_RULES = [
  /^SourceMovieMakerLauncher\//,
  /^Deps\/release\/injector(\/|$)/,
  /^Automation\/tests\/launcher-/,
  /^\.gitmodules$/,
];

// panorama-tests reads the Panorama JS headers, the cosmetics catalog generator and itself.
const PANORAMA_RULES = [
  /^AfxHookSource2\/Filmmaker\/Panorama\//,
  /^Automation\/tools\/generate_cosmetics_catalog\.py$/,
  /^Automation\/tests\/panorama-interaction-layout-tests\.mjs$/,
];

const matches = (rules, file) => rules.some((rule) => rule.test(file));
const normalizePath = (file) => file.replace(/\\/g, "/");

const EVERYTHING = { native: "all", launcher: true, panorama: true };

// Maps changed repository paths to the components they affect.
export function classifyFiles(files) {
  const result = { native: "none", launcher: false, panorama: false };
  for (const raw of files) {
    const file = normalizePath(raw);
    if (matches(EVERYTHING_RULES, file)) return { ...EVERYTHING };
    if (matches(DEADLOCK_ONLY_RULES, file)) {
      if (result.native === "none") result.native = "deadlock";
    } else if (matches(NATIVE_RULES, file)) {
      result.native = "all";
    }
    if (matches(LAUNCHER_RULES, file)) result.launcher = true;
    if (matches(PANORAMA_RULES, file)) result.panorama = true;
  }
  return result;
}

// mode "full": the shipping Visual Studio build without compile caches, for nightly runs, manual
// runs and release lines. mode "fast": the cached Ninja build, limited to what the change touches.
// `files` is null when the change could not be listed; that runs everything.
export function decideScope({ event, ref = "", baseRef = "", files }) {
  if (event === "schedule" || event === "workflow_dispatch")
    return { mode: "full", ...EVERYTHING, reason: `${event} run validates everything` };
  if (event === "push" && ref.startsWith("refs/heads/release/"))
    return { mode: "full", ...EVERYTHING, reason: "release branch push validates everything" };
  if (event === "pull_request" && baseRef.startsWith("release/"))
    return { mode: "full", ...EVERYTHING, reason: "pull request into a release branch validates everything" };
  if (event === "merge_group")
    return { mode: "fast", ...EVERYTHING, reason: "merge queue groups build and test everything" };
  if ((event !== "pull_request" && event !== "push") || files === null)
    return { mode: "fast", ...EVERYTHING, reason: "changed files unknown; running everything" };
  return { mode: "fast", ...classifyFiles(files), reason: `${files.length} changed file(s)` };
}

// Which aggregated jobs must pass for this scope. Jobs not listed here may pass or be skipped.
export function requiredJobs(scope, event) {
  const required = new Set(["changes", "docs-integrity", "security-license-scan"]);
  if (event === "pull_request") required.add("changed-file-policy");
  if (scope.launcher === "true") required.add("launcher-build");
  if (scope.panorama === "true") required.add("panorama-tests");
  if (scope.mode === "full") required.add("native-full-validation");
  else if (scope.native === "all" || scope.native === "deadlock") required.add("native-build-windows");
  return required;
}

// `needs` is the workflow's `needs` context. Returns the reasons the gate must fail.
export function evaluateGate(needs, event) {
  const failures = [];
  for (const [job, { result }] of Object.entries(needs)) {
    if (result === "failure" || result === "cancelled") failures.push(`${job}: ${result}`);
  }
  if (needs.changes?.result !== "success") {
    if (!failures.some((failure) => failure.startsWith("changes:")))
      failures.push(`changes: ${needs.changes?.result ?? "missing"}`);
    return failures;
  }
  const scope = needs.changes.outputs ?? {};
  if (!["fast", "full"].includes(scope.mode) || !["none", "deadlock", "all"].includes(scope.native))
    return [...failures, `changes: unexpected outputs ${JSON.stringify(scope)}`];
  for (const job of requiredJobs(scope, event)) {
    const result = needs[job]?.result ?? "missing";
    if (result !== "success" && !failures.some((failure) => failure.startsWith(`${job}:`)))
      failures.push(`${job}: ${result}, but this change needs it`);
  }
  return failures;
}

function git(args) {
  const run = spawnSync("git", args, { cwd: repoRoot, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  return run.status === 0 ? run.stdout : null;
}

const listChanged = (range) => {
  const output = git(["diff", "--name-only", "--no-renames", ...range]);
  return output === null ? null : output.split("\n").filter(Boolean);
};

function changedFiles(env) {
  if (env.GITHUB_EVENT_NAME === "pull_request") {
    const { PR_BASE_SHA: base, PR_HEAD_SHA: head } = env;
    return base && head ? listChanged([`${base}...${head}`]) : null;
  }
  if (env.GITHUB_EVENT_NAME === "push") {
    const { PUSH_BEFORE: before, GITHUB_SHA: after } = env;
    // A new branch has no previous commit, and a forced update may not descend from it.
    if (!before || /^0+$/.test(before) || !after) return null;
    if (git(["merge-base", "--is-ancestor", before, after]) === null) return null;
    return listChanged([before, after]);
  }
  return null;
}

function runScope(env) {
  const files = changedFiles(env);
  const scope = decideScope({
    event: env.GITHUB_EVENT_NAME,
    ref: env.GITHUB_REF,
    baseRef: env.GITHUB_BASE_REF,
    files,
  });
  const outputs = [
    `mode=${scope.mode}`,
    `native=${scope.native}`,
    `launcher=${scope.launcher}`,
    `panorama=${scope.panorama}`,
  ];
  if (env.GITHUB_OUTPUT) appendFileSync(env.GITHUB_OUTPUT, `${outputs.join("\n")}\n`);
  console.log(`${outputs.join(" ")} (${scope.reason})`);
  if (env.GITHUB_STEP_SUMMARY) {
    const shown = (files ?? []).slice(0, 50).map((file) => `- \`${file}\``);
    if (files && files.length > shown.length) shown.push(`- ... ${files.length - shown.length} more`);
    appendFileSync(
      env.GITHUB_STEP_SUMMARY,
      [`### CI scope`, "", `\`${outputs.join(" ")}\`: ${scope.reason}`, "", ...shown, ""].join("\n"),
    );
  }
  return 0;
}

function runGate(env) {
  let needs;
  try {
    needs = JSON.parse(env.NEEDS_JSON ?? "");
  } catch {
    console.error("ci-gate: NEEDS_JSON is missing or not JSON");
    return 1;
  }
  const failures = evaluateGate(needs, env.GITHUB_EVENT_NAME);
  for (const [job, { result }] of Object.entries(needs)) console.log(`${job}: ${result}`);
  if (failures.length === 0) {
    console.log("ci-gate: every required job passed");
    return 0;
  }
  console.error(`ci-gate: ${failures.length} problem(s)`);
  for (const failure of failures) console.error(`  ${failure}`);
  return 1;
}

function main(argv, env) {
  if (argv.length === 1 && argv[0] === "--scope") return runScope(env);
  if (argv.length === 1 && argv[0] === "--gate") return runGate(env);
  console.error("usage: ci-change-scope.mjs --scope | --gate");
  return 2;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exitCode = main(process.argv.slice(2), process.env);
}
