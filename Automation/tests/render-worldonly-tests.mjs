// World Only pass invariants (Docs/agent/render-system-contract.md 9c).
//
// These are source-level: the classifier and the arm/restore lifecycle are the parts that silently
// regress, and both are cheap to pin here. The behaviour itself is verified live by
// Automation/verify/verify-render-worldonly.ps1 -- a draw filter can only really be proven against
// a running frame, and that script asserts the per-class kept/dropped counters and the image A/B.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repositoryRoot, ...parts), "utf8");

const worldOnly = read("AfxHookSource2", "Filmmaker", "Render", "RenderWorldOnly.cpp");
const worldOnlyHeader = read("AfxHookSource2", "Filmmaker", "Render", "RenderWorldOnly.h");
const sceneSystem = read("AfxHookSource2", "SceneSystem.cpp");
const driver = read("AfxHookSource2", "Filmmaker", "Render", "RenderSessionDriver.cpp");
const manifest = read("AfxHookSource2", "Filmmaker", "Render", "RenderManifest.cpp");
const command = read("AfxHookSource2", "Filmmaker", "Render", "RenderCommand.cpp");
// CS2's live pass view and its other answers to the shared render code (RenderHost.h).
const host = read("AfxHookSource2", "Filmmaker", "Render", "RenderHostSource2.cpp");
const types = read("AfxHookSource2", "Filmmaker", "Render", "RenderTypes.h");
const renderJs = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraEditorRenderJs.h");
const hooks = read("AfxHookSource2", "RenderSystemDX11Hooks.cpp");

// The classifier is the whole feature. Each of these prefixes was established against live draws
// (the reversed weapon-path spelling and `compmat_` in particular); dropping one silently puts a
// subject back in the plate.
test("the classifier still covers every measured subject material spelling", () => {
  const classify = worldOnly.slice(worldOnly.indexOf("WoClass Classify("), worldOnly.indexOf("bool ClassIsWorld"));
  for (const rule of [
    "characters/models/shared/arms/", // first-person arms AND third-person hands (same asset)
    "characters/",                    // agent bodies, ragdolls, worn gear
    "weapons/",                       // econ namespace as a whole: models, stickers, KEYCHAINS
    "models/weapons/",                // world-model spelling: segments REVERSED
    "compmat_",                       // composited econ skin: no path at all
    "models/props/",                  // kept, but must classify so it is never mistaken for a subject
  ]) {
    assert.ok(classify.includes(rule), `classifier lost the '${rule}' rule`);
  }
});

// A leak found live 2026-08-10: charms live at `weapons/keychains/...`, which a `weapons/models/`
// prefix missed, so they hung in mid-air where the gun had been removed. The subject namespaces
// must be matched at their ROOT, not at a guessed sub-path.
test("econ subject namespaces are matched at their root", () => {
  const classify = worldOnly.slice(worldOnly.indexOf("WoClass Classify("), worldOnly.indexOf("// ---- \"is the bomb"));
  assert.doesNotMatch(classify, /StartsWith\(materialName, "weapons\/models\/"\)/,
    "weapons/ must be matched at the root or keychains/stickers leak back in");
  assert.doesNotMatch(classify, /StartsWith\(materialName, "characters\/models\/"\)\s*\)?\s*\n\s*return WoClass::Players/,
    "characters/ must be matched at the root");
});

test("world and props are kept; players, viewmodel and weapons are dropped", () => {
  const keep = worldOnly.slice(worldOnly.indexOf("bool ClassIsWorld"), worldOnly.indexOf("// ---- state"));
  assert.match(keep, /c == WoClass::World \|\| c == WoClass::Props/);
  // Anything else would put a subject class on the keep side.
  assert.doesNotMatch(keep, /WoClass::Players|WoClass::Viewmodel|WoClass::Weapons/);
});

// The bomb is the only state-dependent verdict, and the ONLY thing the planted gate may admit.
test("the bomb is its own class and only the C4 material paths reach it", () => {
  const classify = worldOnly.slice(worldOnly.indexOf("WoClass Classify("), worldOnly.indexOf("// ---- \"is the bomb"));
  const bombRules = classify.slice(classify.indexOf("WoClass::Bomb") - 400, classify.indexOf("return WoClass::Bomb;"));
  assert.match(bombRules, /weapons\/models\/c4\//);
  assert.match(bombRules, /models\/weapons\/w_c4/);
  // Every path that maps to Bomb must be a c4 path. A stray rule here would smuggle a whole
  // weapon family back in under the bomb gate.
  for (const literal of bombRules.match(/"[^"]+"/g) ?? []) {
    assert.match(literal, /c4/, `non-C4 material path '${literal}' classifies as the bomb`);
  }
  // Bomb is classified BEFORE the general weapon rules, or they would swallow it first.
  assert.ok(classify.indexOf("return WoClass::Bomb;") < classify.indexOf("return WoClass::Weapons;"));
});

// The bomb is visible when it is RESTING IN THE SCENE (planted, or dropped on the ground) and
// hidden only while carried -- a carried one would hang in mid-air once its carrier is removed.
test("the bomb is kept unless it is being carried, and only via a global bit", () => {
  const kept = worldOnly.slice(worldOnly.indexOf("bool ClassIsKept"), worldOnly.indexOf("// ---- bounded"));
  assert.match(kept, /if \(c == WoClass::Bomb\) return BombPlanted\(\);/);
  // The rest of the verdict stays material-only.
  assert.match(kept, /return ClassIsWorld\(c\);/);
  const pump = worldOnly.slice(
    worldOnly.indexOf("void RenderWorldOnly_RunMainThreadFrame"),
    worldOnly.indexOf("void RenderWorldOnly_SetArmed"),
  );
  assert.match(pump, /state == BombState::Planted\) \|\| \(state == BombState::Dropped\)/);
  // Existence of the planted entity + the carryable one's owner handle, not a game-rules field.
  assert.match(worldOnly, /planted_c4/);
  assert.match(worldOnly, /weapon_c4/);
  assert.match(worldOnly, /m_hOwnerEntity/);
  // Never a per-draw entity lookup: the scan runs on the main-thread pump, not in the filter.
  const filter = worldOnly.slice(worldOnly.indexOf("bool RenderWorldOnly_RejectsMaterialFrom"));
  assert.doesNotMatch(filter.slice(0, filter.indexOf("}")), /ScanBomb|SafeEntity/);
});

test("the planted-bomb scan is throttled and inert while disarmed", () => {
  const pump = worldOnly.slice(
    worldOnly.indexOf("void RenderWorldOnly_RunMainThreadFrame"),
    worldOnly.indexOf("void RenderWorldOnly_SetArmed"),
  );
  assert.match(pump, /const bool armed = g_armed\.load/);
  assert.match(pump, /if \(!armed && !g_jobUsesPass\.load/);
  assert.match(pump, /g_bombPlanted\.store\(false/);
  assert.match(pump, /kBombScanFrames/);
});

test("an unclassifiable draw is kept, never dropped", () => {
  const classify = worldOnly.slice(worldOnly.indexOf("WoClass Classify("), worldOnly.indexOf("bool ClassIsWorld"));
  assert.match(classify, /if \(!materialName \|\| !\*materialName\) return WoClass::World;/);
});

// Rule 9c: no material substitution and no depth writes. The ONE piece of engine state World Only
// touches is r_drawcsplayers (player shadows, below), and it is saved and restored.
test("World Only never substitutes materials or writes depth", () => {
  for (const forbidden of [/SetMaterial/, /materialOverride/i, /ClearDepth/i]) {
    assert.doesNotMatch(worldOnly, forbidden);
  }
});

// A draw filter cannot remove a shadow: the shadow pass runs upstream of the draws it sees. Forcing
// r_drawcsplayers is the only reach into that pass, so it must exist -- and must be reversible.
test("player shadows are removed by the convar, and the user's value is restored", () => {
  assert.match(worldOnly, /kDrawPlayersCvar = "r_drawcsplayers"/);
  const reconcile = worldOnly.slice(
    worldOnly.indexOf("void ReconcilePlayerDrawCvar"),
    worldOnly.indexOf("enum class BombState"),
  );
  assert.match(reconcile, /if \(armed == g_cvarApplied\) return;/); // idempotent, no per-frame writes
  assert.match(reconcile, /g_cvarPrevValue = ReadBoolCvarValue\(kDrawPlayersCvar/); // save on arm
  assert.match(reconcile, /g_cvarPrevValue\.empty\(\) \? "1" : g_cvarPrevValue\.c_str\(\)/); // restore verbatim
  // Driven from the main-thread pump, and reconciled even on the frame it goes idle.
  const pump = worldOnly.slice(
    worldOnly.indexOf("void RenderWorldOnly_RunMainThreadFrame"),
    worldOnly.indexOf("void RenderWorldOnly_SetArmed"),
  );
  assert.match(pump, /ReconcilePlayerDrawCvar\(armed\);/);
  assert.ok(pump.indexOf("ReconcilePlayerDrawCvar") < pump.indexOf("if (!armed &&"),
    "the convar must be reconciled before the early-out, or disarming never restores it");
});

// The policy must not migrate into the draw hook: SceneSystem owns plumbing, RenderWorldOnly owns
// what a player is.
test("the draw hook asks the module and does not classify anything itself", () => {
  assert.match(sceneSystem, /RenderWorldOnly_Active\(\)/);
  assert.match(sceneSystem, /RenderWorldOnly_RejectsBatchFrom\(/);
  // mirv_streams' scene filter (upstream advancedfx) has its own material classifier; it is not ours.
  const ours = sceneSystem.replace(/static bool SceneDataMaterialIs\w+\([\s\S]*?\r?\n\}\r?\n/g, "");
  assert.doesNotMatch(ours, /characters\/models/);
  assert.doesNotMatch(ours, /compmat_/);
});

// A batch is an ARRAY and the hook can only skip it whole. Deciding from element 0 alone took the
// world geometry sharing a batch with a subject: a potted plant kept its depth but lost its colour
// pass and rendered as a black silhouette (de_mirage tick 5136, 2026-08-10).
test("the whole batch is classified, not just its first element", () => {
  assert.match(sceneSystem, /CollectBatchMaterialNames\(sceneData, a4, names\)/);
  const collect = sceneSystem.slice(
    sceneSystem.indexOf("int CollectBatchMaterialNames"),
    sceneSystem.indexOf("const char* DrawDescriptorTag"),
  );
  assert.match(collect, /sceneData \+ i/);          // strides across elements
  assert.match(collect, /kMaxBatchNames/);          // bounded
  assert.match(collect, /ProbeReadPointerAt/);      // SEH-guarded per element
});

test("a batch mixing world with subjects is kept whole", () => {
  const batch = worldOnly.slice(worldOnly.indexOf("bool RenderWorldOnly_RejectsBatchFrom"));
  assert.match(batch, /if \(!anyKept\) return true;/);
  // Never eat the map: any kept element vetoes the skip.
  assert.match(batch, /if \(keep\) anyKept = true;/);
  assert.match(batch, /MIXED batch kept whole/);
});

test("both hooked draw paths consult the filter", () => {
  const base = sceneSystem.slice(
    sceneSystem.indexOf("void new_DrawBaseSceneObject"),
    sceneSystem.indexOf("// ---- AGGREGATE SCENE OBJECTS"),
  );
  const aggregate = sceneSystem.slice(
    sceneSystem.indexOf("void __fastcall new_DrawAggregateSceneObject"),
    sceneSystem.indexOf("struct FloatColor"),
  );
  assert.match(base, /RenderWorldOnly_Active\(\)/);
  assert.match(aggregate, /RenderWorldOnly_Active\(\)/);
});

// Detouring a descriptor whose slot 1 is not the drawable-batch draw killed CS2 at startup twice
// (eight-at-once, then CSceneObjectDesc alone). The table must stay at the two confirmed classes.
test("the drawable-batch detour table stays at the two confirmed descriptor classes", () => {
  const tableStart = sceneSystem.indexOf("DrawDescriptorHook g_DrawDescriptorHooks[] = {");
  const table = sceneSystem.slice(tableStart, sceneSystem.indexOf("};", tableStart));
  assert.match(table, /CBaseSceneObjectDesc/);
  assert.match(table, /CAnimatableSceneObjectDesc/);
  assert.equal((table.match(/\{ "\.\?AV/g) ?? []).length, 2);
});

// The job owns the flag for its whole life and gives it back on EVERY outcome, because
// RestoreEverything is the single idempotent teardown shared by success, cancel and failure.
test("a render job drives World Only per render, never through the live arm", () => {
  const saving = driver.slice(driver.indexOf("case S::SavingState"), driver.indexOf("case S::SeekingPreRoll"));
  assert.match(saving, /RenderWorldOnly_SetArmed\(false, "render job"\)/);
  assert.match(saving, /RenderWorldOnly_SetJobUsesPass\(s_plan\.renders\[\(int\)RenderKey::WorldOnly\]\)/);
  const restore = driver.slice(driver.indexOf("void RestoreEverything()"), driver.indexOf("void FinishJob("));
  assert.match(restore, /RenderWorldOnly_SetJobUsesPass\(false\)/);
  // The draw hook decides by the key of the recorded render being drawn (set before its scene
  // update, cleared after it was waited for), never by render-thread queue flags.
  assert.match(worldOnly, /AfxStreams_CurrentPassRenderKey\(\) == \(int\)RenderKey::WorldOnly/);
  assert.doesNotMatch(worldOnly, /g_passActive/);
  assert.match(hooks, /g_CurrentPassRenderKey\.store\(key, std::memory_order_release\);/);
});

test("the live preview never fights a running job", () => {
  // The workspace arms World Only from ONE place, and that place stands down while a job runs: the
  // job's World Only render is an extra render of its own and the view must stay the full scene.
  const calls = [...host.matchAll(/RenderWorldOnly_SetArmed\(/g)];
  assert.equal(calls.length, 1);
  assert.doesNotMatch(command, /RenderWorldOnly_SetArmed\(/);
  const apply = command.slice(command.indexOf("void Render_ApplyLivePreview()"));
  assert.match(apply, /^void Render_ApplyLivePreview\(\) \{\s*if \(RenderSessionDriver_IsActive\(\)\)\s*return;\s*RenderHost_ApplyLivePreview\(/);
  assert.match(host, /RenderWorldOnly_SetArmed\(e == VisibleElements::OnlyWorld/);
});

test("the take records every pass it carries", () => {
  assert.match(types, /std::vector<PassConfig> passes/);
  assert.match(manifest, /b\.Key\("streams"\)/);
  assert.match(manifest, /PassStreamName\(pass\)/);
  assert.match(command, /"list\\":\[/);
});

test("the Render workspace exposes World Only in the pass grid and the live preview", () => {
  assert.match(renderJs, /\['world', 'World only'/);
  assert.match(renderJs, /mirv_filmmaker render pass toggle /);
  assert.match(renderJs, /mirv_filmmaker render pass preview /);
});

test("the probe and coverage instrumentation stay available", () => {
  assert.match(worldOnlyHeader, /RenderWorldOnly_ArmProbe/);
  assert.match(worldOnly, /MvmDebugLog_LinefAlways\("render\.worldonly"/);
  assert.match(worldOnly, /SceneSystem_ReportDrawCoverage\(\)/);
});
