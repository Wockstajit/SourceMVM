// Players Only invariants (Docs/features/filmmaker_render_passes.md).
//
// Players Only is one visibility group of the multipass system: its render is an extra render of
// the same engine frame as the Main (beauty) render. So it must change NOTHING outside its own
// draws -- no convars, no sky material, no material pokes during a job -- and the pass compositor
// keys the background (chroma-painted scene + sky by depth) onto the chosen colour.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repositoryRoot, ...parts), "utf8");

const green = read("AfxHookSource2", "Filmmaker", "Render", "RenderPlayersOnlyGreen.cpp");
const greenHeader = read("AfxHookSource2", "Filmmaker", "Render", "RenderPlayersOnlyGreen.h");
const hooks = read("AfxHookSource2", "RenderSystemDX11Hooks.cpp");
const renderService = read("AfxHookSource2", "RenderServiceHooks.cpp");
const scene = read("AfxHookSource2", "SceneSystem.cpp");
const sceneHeader = read("AfxHookSource2", "SceneSystem.h");
const shader = read("Shaders", "afx_render_pass_ps_5_0.fxc");
const filmmaker = read("AfxHookSource2", "Filmmaker", "Filmmaker.cpp");

test("scene policy admits players/equipment and preserves effects while replacing map draws", () => {
  assert.match(green, /characters\/models\/shared\/arms\//);
  assert.match(green, /weapons\//);
  assert.match(green, /w_models\/w_eq_defuser/);
  assert.match(green, /PlayerEquipment/);
  assert.match(green, /RenderPlayersOnlyGreen_IsEffectMaterial/);
  assert.match(greenHeader, /RenderPlayersOnlyGreen_IsDecalMaterial/);
  assert.match(scene, /IsEntirelyPlayersOnlyGreenSkippedBatch/);
  assert.match(green, /volumetric/);
  assert.match(sceneHeader, /SceneSystem_SetPlayersOnlyGreenPolicy/);
  assert.match(scene, /ApplyPlayersOnlyGreenMaterials\(sceneData, a4/);
  assert.match(scene, /RestorePlayersOnlyGreenMaterials\(sceneData/);
  assert.match(scene, /compare-before-write/);
  assert.match(green, /chromakey_unlit\.vmat/);
});

test("the job path changes no global state: there is no job-wide green mode", () => {
  for (const source of [green, greenHeader, scene, hooks]) {
    assert.doesNotMatch(source, /JobActive\(\)/);
    assert.doesNotMatch(source, /SetJobSkyOverride/);
  }
  const pump = green.slice(green.indexOf("void RenderPlayersOnlyGreen_RunMainThreadFrame"),
    green.indexOf("void RenderPlayersOnlyGreen_OnLevelInit"));
  // Convars, the sky dome and material pokes follow the LIVE preview arm only.
  assert.match(pump, /const bool desired = g_armed\.load\(std::memory_order_acquire\);/);
  assert.doesNotMatch(pump, /g_renderActive[\s\S]*ReconcileSceneCvars/);
});

test("no recorded render repaints materials: a job's Players Only is masked by depth", () => {
  const begin = hooks.slice(hooks.indexOf("void BeginPassRender(int key)"), hooks.indexOf("void EndPassRender()"));
  assert.match(begin, /g_CurrentPassRenderKey\.store\(key/);
  assert.doesNotMatch(begin, /RenderPlayersOnlyGreen_SetRenderPassActive/);
  assert.doesNotMatch(begin, /BeginReliable/);
  assert.match(green, /if \(AfxStreams_CurrentPassRenderKey\(\) >= 0\) return false;/);
  // The scene update must see the render key before it builds the extra render's command lists,
  // and the previous render must have finished before the key changes.
  assert.ok(renderService.indexOf("g_Old_SceneSystem_WaitForRenderingToComplete(g_pSceneSystem);")
    < renderService.indexOf("RenderSystemDX11_EngineThread_BeginNextRenderPass();"));
  assert.ok(renderService.indexOf("RenderSystemDX11_EngineThread_BeginNextRenderPass();")
    < renderService.indexOf("FrameUpdate(g_pSceneSystem, 1);"));
  // The job's Players Only takes its depth reference from an output-less World Only render, and
  // the compositor masks it by depth, never by the chroma repaint.
  assert.match(hooks, /ref\.PassReferenceOnly = true;/);
  assert.match(hooks, /params\.worldRef = pass\.elements == Filmmaker::VisibleElements::OnlyPlayers;/);
  assert.match(shader, /if \(mask == 3\) \{[\s\S]*?here < world - max\(1\.5, world \* 0\.004\)/);
});

test("the green clear stays scoped to the scene resource and smoke is not suppressed", () => {
  assert.match(hooks, /AfxPlayersOnlyGreenMatchesSceneTarget/);
  assert.match(hooks, /candidateResource == sceneResource/);
  assert.match(hooks, /RenderPlayersOnlyGreen_KeyColor/);
  assert.doesNotMatch(green, /r_drawparticles/);
  assert.doesNotMatch(green, /cl_smoke_volumeprop/);
  assert.doesNotMatch(hooks, /AfxRender_SetSmokeSuppressed\(true/);
});

test("the compositor keys the chroma scene and the sky onto the chosen colour", () => {
  const mask = shader.slice(shader.indexOf("if (mask == 1)"), shader.indexOf("} else if (mask == 2)"));
  assert.match(mask, /keyIn\.a/);          // chroma match tolerance
  assert.match(mask, /nearFar\.z/);        // sky by depth: env_sky keeps its own colour
  assert.match(shader, /lerp\(keyOut\.rgb, col\.rgb, subject\)/);
  assert.match(greenHeader, /RenderPlayersOnlyGreen_OnLevelInit/);
  assert.match(filmmaker, /RenderPlayersOnlyGreen_RunMainThreadFrame\(\)/);
});
