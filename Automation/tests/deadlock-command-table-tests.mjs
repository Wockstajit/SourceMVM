// Static checks that the Deadlock command surface cannot drift from what its UI sends.
//
//   node --test Automation/tests/deadlock-command-table-tests.mjs
//
// The Deadlock dispatcher is one `if (0 == _stricmp(sub, "verb"))` chain in DeadlockCommands.cpp,
// and its help is a table (kHelpRows) printed by PrintHelp. Nothing used to tie the three together,
// which is how the Regular Timeline's +/-10 s buttons shipped sending `seekrel` -- a verb the
// dispatcher never had -- and how the in-game help went stale. These tests read the sources, so
// they run without the game.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const read = (rel) => readFileSync(join(root, rel), "utf8");

const commandsCpp = read("AfxHookSource2/Deadlock/DeadlockCommands.cpp");
const jsDir = "AfxHookSource2/Filmmaker/Deadlock";
const jsHeaders = readdirSync(join(root, jsDir)).filter((f) => f.endsWith("Js.h")).map((f) => `${jsDir}/${f}`);

function dispatchedVerbs() {
  const verbs = new Set();
  for (const m of commandsCpp.matchAll(/0 == _stricmp\(sub, "([a-z_]+)"\)/g)) verbs.add(m[1]);
  return verbs;
}

function helpRowVerbs() {
  const table = commandsCpp.match(/kHelpRows\[\]\s*=\s*\{([\s\S]*?)\n\};/);
  assert.ok(table, "DeadlockCommands.cpp must define the help table `kHelpRows[] = { ... };`");
  const verbs = new Set();
  for (const m of table[1].matchAll(/\{\s*"([a-z_]+)"\s*,/g)) verbs.add(m[1]);
  return verbs;
}

// Only quoted string literals count: comments that mention a command are not commands.
function emittedCommands(text) {
  const out = [];
  for (const m of text.matchAll(/['"](?:\\x02MVMCMD )?mirv_(deadlock|filmmaker) ([a-z_]+)/g)) {
    out.push({ root: m[1], verb: m[2] });
  }
  return out;
}

test("every dispatched verb has a help row, and every help row is dispatched", () => {
  const dispatched = dispatchedVerbs();
  const help = helpRowVerbs();
  assert.ok(dispatched.size >= 30, `expected the full dispatcher, found ${dispatched.size} verbs`);
  const missingHelp = [...dispatched].filter((v) => !help.has(v)).sort();
  const deadHelp = [...help].filter((v) => !dispatched.has(v)).sort();
  assert.deepEqual(missingHelp, [], `verbs with no help row: ${missingHelp.join(", ")}`);
  assert.deepEqual(deadHelp, [], `help rows for verbs the dispatcher does not handle: ${deadHelp.join(", ")}`);
});

test("every verb the Deadlock UI and hotkeys send exists in the dispatcher", () => {
  const dispatched = dispatchedVerbs();
  const sources = [
    ...jsHeaders,
    "AfxHookSource2/Deadlock/DeadlockHotkeys.cpp",
    "AfxHookSource2/Deadlock/DeadlockUi.cpp",
  ];
  const unknown = [];
  for (const rel of sources) {
    for (const { verb } of emittedCommands(read(rel))) {
      if (!dispatched.has(verb)) unknown.push(`${rel}: ${verb}`);
    }
  }
  assert.deepEqual([...new Set(unknown)], [], `UI sends verbs the dispatcher does not handle:\n${unknown.join("\n")}`);
});

test("the UI and hotkeys use the canonical root mirv_filmmaker", () => {
  const offenders = [];
  for (const rel of [...jsHeaders, "AfxHookSource2/Deadlock/DeadlockHotkeys.cpp", "AfxHookSource2/Deadlock/DeadlockUi.cpp"]) {
    for (const { root: r, verb } of emittedCommands(read(rel))) {
      if (r !== "filmmaker") offenders.push(`${rel}: mirv_${r} ${verb}`);
    }
  }
  assert.deepEqual([...new Set(offenders)], [], `non-canonical command roots:\n${offenders.join("\n")}`);
});

test("help is printed under the canonical root only", () => {
  const printHelp = commandsCpp.match(/void PrintHelp\(\)\s*\{([\s\S]*?)\n\}/);
  assert.ok(printHelp, "PrintHelp must exist");
  assert.ok(!/mirv_deadlock/.test(printHelp[1]), "PrintHelp must not advertise the mirv_deadlock alias");
});

test("netcon and Panorama dispatch accept both roots by name", () => {
  const dispatch = commandsCpp.match(/bool Deadlock::Commands_DispatchLine\([\s\S]*?\n\}/);
  assert.ok(dispatch, "Commands_DispatchLine must exist");
  assert.match(dispatch[0], /_stricmp\(name, "mirv_filmmaker"\)/);
  assert.match(dispatch[0], /_stricmp\(name, "mirv_deadlock"\)/);
});

test("sub-handlers receive the typed root instead of a hardcoded alias", () => {
  // HandheldCam_RunCommand was removed 2026-09-14 along with the rest of Deadlock's HandheldCam
  // bridge (DeadlockCameraEffects.cpp) -- no UI ever exposed it, and a stray console test's
  // "enabled": true had persisted into the settings sidecar and silently re-armed the shake on
  // every later launch. DeadlockPathShake_RunCommand is the remaining example of this same pattern.
  for (const fn of ["DeadlockPathShake_RunCommand", "UiLive_RunCommand"]) {
    const call = commandsCpp.match(new RegExp(`${fn}\\(argc, args, ([^)]+)\\)`));
    assert.ok(call, `${fn} call not found`);
    assert.ok(!/"mirv_deadlock"/.test(call[1]), `${fn} is passed the hardcoded alias`);
  }
});
