import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

test("BakedShadowFx owns only lb_enable_baked_shadows", () => {
  const src = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "BakedShadowFx.cpp"),
    "utf8",
  );
  const header = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "BakedShadowFx.h"),
    "utf8",
  );
  assert.match(src, /"lb_enable_baked_shadows"/);
  assert.match(header, /lb_enable_baked_shadows/);
  assert.doesNotMatch(src, /"sc_disable_baked_lighting"/);
  assert.doesNotMatch(src, /AgentDbgShadowLog|debug-24a695\.log/);
  const targets = src.match(/CvarTarget g_targets\[\] = \{([\s\S]*?)\n\};/);
  assert.ok(targets, "expected g_targets initializer");
  assert.match(targets[1], /lb_enable_baked_shadows/);
  assert.doesNotMatch(
    targets[1],
    /lb_precomputed|sparseshadowtree|lb_mixed_shadows|lb_csm_override_staticgeo/,
  );
});

test("DynamicShadowFx owns only csm_max_num_cascades_override", () => {
  const src = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "DynamicShadowFx.cpp"),
    "utf8",
  );
  assert.match(src, /csm_max_num_cascades_override/);
  assert.doesNotMatch(
    src,
    /"lb_enable_shadow_casting"|"lb_enable_sunlight"|"lb_enable_baked_shadows"|"r_shadows"/,
  );
  assert.doesNotMatch(src, /AgentDbgDynShadowLog|debug-24a695\.log/);
});

test("standalone shadow-separation verifier exists and stays decoupled", () => {
  const verifier = fs.readFileSync(
    path.join(root, "Automation", "verify", "verify-shadow-separation.ps1"),
    "utf8",
  );
  assert.match(verifier, /verify-shadow-separation/);
  assert.match(verifier, /demo_gototick 2936/);
  assert.match(verifier, /mirv_filmmaker world bakedshadows off/);
  assert.match(verifier, /mirv_filmmaker world dynamicshadows off/);
  assert.match(verifier, /650,650,950,950/);
  assert.match(verifier, /1000,750,1400,1050/);
  assert.match(verifier, /mirv_filmmaker mouse free/);
  // Must not fold into unrelated lighting/sky matrix verifier.
  assert.doesNotMatch(verifier, /verify-sky-light-map-matrix|OnlyMap|de_ancient/);
});

test("sun.md documents proven baked gate and rejected controls", () => {
  const doc = fs.readFileSync(path.join(root, "Docs", "effects", "sun.md"), "utf8");
  assert.match(doc, /lb_enable_baked_shadows/);
  assert.match(doc, /csm_max_num_cascades_override/);
  assert.match(doc, /sc_disable_baked_lighting/);
  assert.match(doc, /lb_csm_override_staticgeo/);
  assert.match(doc, /2000879|ClientVersion/);
  assert.match(doc, /DisableShadows_t|m_nSceneObjectOverrideFlags/);
});

test("ShadowStageProbe is a dedicated SST->CSM research module", () => {
  const src = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "ShadowStageProbe.cpp"),
    "utf8",
  );
  const header = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "ShadowStageProbe.h"),
    "utf8",
  );
  const cmake = fs.readFileSync(path.join(root, "AfxHookSource2", "CMakeLists.txt"), "utf8");
  const worldCmd = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "WorldCommand.cpp"),
    "utf8",
  );
  assert.match(header, /CopySSTDepthToCSM/);
  assert.match(src, /CopySSTDepthToCSM/);
  assert.match(src, /world\.shadowstage/);
  assert.match(src, /ShadowStageProbe_SetSuppressCopy/);
  assert.match(cmake, /ShadowStageProbe\.cpp/);
  assert.match(worldCmd, /shadowstage/);
  assert.doesNotMatch(src, /lb_enable_baked_shadows|csm_max_num_cascades_override/);
});

test("standalone SST-CSM verifier exists and stays decoupled", () => {
  const verifier = fs.readFileSync(
    path.join(root, "Automation", "verify", "verify-sst-csm-boundary.ps1"),
    "utf8",
  );
  assert.match(verifier, /shadowstage suppress/);
  assert.match(verifier, /demo_gototick 2936/);
  assert.match(verifier, /650,650,950,950/);
  assert.doesNotMatch(verifier, /OnlyMap|de_ancient/);
  // Own entry point: must not invoke the product baked/live verifier.
  assert.doesNotMatch(verifier, /(?:pwsh|powershell).*verify-shadow-separation\.ps1|-File\s+.*verify-shadow-separation\.ps1/);
});

test("ShadowFx is the product master engine-shadow gate", () => {
  const src = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "ShadowFx.cpp"),
    "utf8",
  );
  const lighting = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudLightingTabJs.h"),
    "utf8",
  );
  const worldCmd = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "WorldCommand.cpp"),
    "utf8",
  );
  assert.match(src, /BakedShadowFx_SetEnabled/);
  assert.match(src, /DynamicShadowFx_SetEnabled/);
  assert.match(worldCmd, /world shadows|sub, "shadows"/);
  // ONE switch for the engine's shadows, on the Sun page -- the sun's shadows belong with the
  // sun. Never split into the two cvars it drives: "Live shadows" / "Baked shadows" as separate
  // controls is the arrangement this test exists to keep out of the panel.
  // Docs/effects/shadow-separation.md section 14: Engine shadows is the all-on/off shortcut over
  // BOTH layers, and each layer also has its own switch on the Sun page.
  assert.match(lighting, /'Engine shadows'/);
  assert.match(lighting, /'mirv_filmmaker world shadows ' \+/,
    "Engine shadows drives both layers through the one world shadows command");
  assert.match(lighting, /'Baked shadows'[\s\S]{0,300}world bakedshadows toggle/);
  assert.match(lighting, /'Live engine shadows'[\s\S]{0,300}world dynamicshadows toggle/);
});

test("CustomShadowFx is Phase B directional shadow map", () => {
  const src = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "CustomShadowFx.cpp"),
    "utf8",
  );
  const pass = fs.readFileSync(
    path.join(root, "AfxHookSource2", "ContactShadowPass.cpp"),
    "utf8",
  );
  const atlas = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "CustomShadowAtlas.cpp"),
    "utf8",
  );
  const header = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "CustomShadowFx.h"),
    "utf8",
  );
  const lighting = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudLightingTabJs.h"),
    "utf8",
  );
  const shader = fs.readFileSync(
    path.join(root, "Shaders", "afx_contact_shadow_ps_5_0.fxc"),
    "utf8",
  );
  const cmake = fs.readFileSync(path.join(root, "AfxHookSource2", "CMakeLists.txt"), "utf8");
  assert.match(src, /AfxContactShadow::SetRequest/);
  assert.match(src, /phase-b-shadowmap|Phase B/);
  assert.match(src, /CustomShadowAtlas_SetFillBorrow/);
  assert.match(header, /CustomShadowFx_SetStepPixels/);
  assert.match(header, /CustomShadowFx_SetThickness/);
  assert.match(header, /CustomShadowFx_SetSteps/);
  assert.match(src, /customshadow (?:strength|reach|bias|steps)/);
  assert.match(pass, /softTexels|shadowAtlasSrv|BuildLightViewProj/);
  assert.match(atlas, /CopyResource|ClearDepthStencilView|4352|R16_TYPELESS/);
  // THE PANEL NO LONGER EXPOSES ANY OF THIS. The directional shadow-map composite was pulled out
  // of the Config panel on 2026-09-07 -- it is not a CS2 setting, and the EFFECTS tab is for what
  // the game already has. The C++ below it is untouched and `mirv_filmmaker world customshadow`
  // still drives it from the console, which is what the rest of this test covers.
  assert.doesNotMatch(lighting, /cmd: 'mirv_filmmaker world customshadow/);
  assert.doesNotMatch(lighting, /togglePill\([^,]+, 'Shader shadows'/);
  assert.doesNotMatch(lighting, /__page\('Shadow map'\)/);
  // The engine's own shadow switch stays, on the Sun page where the sun's shadows belong.
  assert.match(lighting, /shadowsToggle = togglePill\(sunPage, 'Engine shadows'/);
  assert.match(pass, /afx_contact_shadow_ps_5_0/);
  assert.match(shader, /texture2|shadow atlas|CascadeUv/);
  assert.match(cmake, /afx_contact_shadow_ps_5_0/);
  assert.match(cmake, /ContactShadowPass\.cpp/);
  assert.match(cmake, /CustomShadowAtlas\.cpp/);
});

test("standalone custom shadow-map verifier exists and stays decoupled", () => {
  const verifier = fs.readFileSync(
    path.join(root, "Automation", "verify", "verify-custom-shadow-map.ps1"),
    "utf8",
  );
  assert.match(verifier, /world shadows off/);
  assert.match(verifier, /world customshadow on/);
  assert.match(verifier, /customshadow reach/);
  assert.match(verifier, /customshadow bias/);
  assert.match(verifier, /customshadow steps/);
  assert.match(verifier, /demo_gototick 2936/);
  assert.doesNotMatch(verifier, /OnlyMap|de_ancient/);
  assert.doesNotMatch(verifier, /(?:pwsh|powershell).*verify-sst-csm-boundary|-File\s+.*verify-sst-csm-boundary/);
});
