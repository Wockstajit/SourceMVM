// World + Players / No Gun invariants (Docs/features/filmmaker_render_passes.md).
//
// In a render job the NoGun render is an extra render of the same engine frame: the recorder writes
// r_drawviewmodel false for that render only (measured to work per pass) and puts the user's value
// back when the render ends. The RenderWorldPlayersNoGun module is the LIVE workspace preview.

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repositoryRoot, ...parts), "utf8");

const moduleSource = read("AfxHookSource2", "Filmmaker", "Render", "RenderWorldPlayersNoGun.cpp");
const header = read("AfxHookSource2", "Filmmaker", "Render", "RenderWorldPlayersNoGun.h");
const hooks = read("AfxHookSource2", "RenderSystemDX11Hooks.cpp");
const plan = read("AfxHookSource2", "Filmmaker", "Render", "RenderPassPlan.h");
const driver = read("AfxHookSource2", "Filmmaker", "Render", "RenderSessionDriver.cpp");
// CS2's live pass view (RenderHost_ApplyLivePreview).
const command = read("AfxHookSource2", "Filmmaker", "Render", "RenderHostSource2.cpp");
const guard = read("AfxHookSource2", "Filmmaker", "Render", "RenderStateGuard.cpp");
const filmmaker = read("AfxHookSource2", "Filmmaker", "Filmmaker.cpp");

test("the live preview module only hides the first-person viewmodel and its weapon FX", () => {
  assert.match(header, /RunMainThreadFrame/);
  assert.match(moduleSource, /r_drawviewmodel/);
  assert.match(moduleSource, /ParticleFx_SetFirstPersonFxSuppressed\(true\)/);
  for (const forbidden of [/RenderWorldOnly/, /SetMaterial/i, /materialOverride/i, /ClearDepth/i, /r_drawcsplayers/]) {
    assert.doesNotMatch(moduleSource, forbidden);
  }
  assert.match(filmmaker, /RenderWorldPlayersNoGun_RunMainThreadFrame\(\)/);
});

test("every extra render hides the viewmodel for that render only, and restores it", () => {
  const begin = hooks.slice(hooks.indexOf("void BeginPassRender(int key)"), hooks.indexOf("void EndPassRender()"));
  assert.match(begin, /if \(key <= \(int\)Filmmaker::RenderKey::Main\) return;/);
  assert.match(begin, /SetBoolCvar\("r_drawviewmodel", false, &m_PassViewmodelOld\)/);
  const end = hooks.slice(hooks.indexOf("void EndPassRender()"), hooks.indexOf("static bool SetBoolCvar("));
  assert.match(end, /SetBoolCvar\("r_drawviewmodel", m_PassViewmodelOld, nullptr\)/);
  // Written directly with the change callback: a console command would land after the render.
  const set = hooks.slice(hooks.indexOf("static bool SetBoolCvar("), hooks.indexOf("int m_PassRenderKey"));
  assert.match(set, /CallChangeCallback/);
});

test("World + Players, Gun Only and Players Only share the NoGun render", () => {
  assert.match(plan, /case VisibleElements::WorldAndPlayers:\s*case VisibleElements::OnlyWeapon:\s*case VisibleElements::OnlyPlayers: return RenderKey::NoGun;/);
});

test("a job never drives the live preview arm, and the user's cvar survives every outcome", () => {
  const saving = driver.slice(driver.indexOf("case S::SavingState"), driver.indexOf("case S::SeekingPreRoll"));
  assert.match(saving, /RenderWorldPlayersNoGun_SetArmed\(false, "render job"\)/);
  assert.match(saving, /TrackCvar\("r_drawviewmodel"/);
  assert.match(command, /RenderWorldPlayersNoGun_SetArmed\(e == VisibleElements::WorldAndPlayers/);
  assert.match(guard, /r_drawviewmodel when tracked/);
});
