import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const source = fs.readFileSync(
  path.join(root, "AfxHookSource2", "Filmmaker", "World", "SkyGpuRotation.cpp"),
  "utf8",
);

function bodyOf(name) {
  const signature = `bool ${name}(`;
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `${name} must exist`);
  const open = source.indexOf("{", start);
  let depth = 0;
  for (let i = open; i < source.length; i += 1) {
    if (source[i] === "{") depth += 1;
    if (source[i] === "}") {
      depth -= 1;
      if (depth === 0) return source.slice(open, i + 1);
    }
  }
  throw new Error(`${name} has no closing brace`);
}

test("sun decal rotation is sourced from Scene Lights, never Skybox", () => {
  const body = bodyOf("ApplySunVertexRotation");
  assert.match(body, /const float yaw = LightFx_SunYaw\(\)/);
  assert.match(body, /const float pitch = LightFx_SunPitch\(\)/);
  assert.doesNotMatch(body, /SkyOrient_Yaw\(\)|SkyOrient_Pitch\(\)|SkyOrient_Mirror\(\)/);
  assert.match(body, /source=scene-lights/);
});

test("Skybox rotation does not activate or transform the sun-decal path", () => {
  const body = bodyOf("HasSunOrientationOverride");
  assert.doesNotMatch(body, /HasSkyOrientationOverride|SkyOrient_/);
});

test("Scene Lights also drives parameterized sky-material suns", () => {
  const lights = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "LightFx.cpp"),
    "utf8",
  );
  const sun = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "SunFx.cpp"),
    "utf8",
  );
  assert.match(lights, /SunFx_SetSceneLightRotation\(LightFx_SunYaw\(\), LightFx_SunPitch\(\)\)/);
  assert.match(sun, /SunFx_SetSceneLightRotation\(float yawDegrees, float pitchDegrees\)/);
  assert.match(sun, /SunRotationMath::Rotate\(\s*\{ source\[0\], source\[1\], source\[2\] \}/);
});

test("Sun tint amount and mode reach both solar irradiance and glow colour", () => {
  const lights = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "LightFx.cpp"), "utf8",
  );
  const sun = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "SunFx.cpp"), "utf8",
  );
  const lightingUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudLightingTabJs.h"), "utf8",
  );
  assert.match(lights, /SunFx_SetSceneLightTint/);
  assert.match(sun, /SolarIrradiance[\s\S]*ApplyLightTint/);
  assert.match(sun, /g_vColorTint[\s\S]*ApplyLightTint/);
  // Sun is a sub-page of the one LIGHTING card now (Docs/features/filmmaker_config_panel.md).
  assert.match(lightingUi, /litSec\.__(?:page|group)\('Sun'\)/);
  assert.match(lightingUi, /world light mode ' \+ \(lightUniform \? 'uniform' : 'filtered'\)/);
  assert.match(lightingUi, /'Color only'/);
  assert.match(lightingUi, /'Filter \/ dim'/);
});

test("Environment pages keep lighting and atmosphere responsibilities separate", () => {
  const lightingUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudLightingTabJs.h"),
    "utf8",
  );
  const atmosphereUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudAtmosphereTabJs.h"),
    "utf8",
  );
  // World tint stays OUT of Lighting and inside Atmosphere -- that split is the point of
  // this test. Both are sub-pages of their card now rather than cards of their own.
  assert.doesNotMatch(lightingUi, /'World tint'/);
  assert.match(atmosphereUi, /atmosSec = section\(pageEffects, 'ATMOSPHERE'.*atmos\.card/s);
  assert.match(atmosphereUi, /atmosSec\.__(?:page|group)\('World tint'\)/);
  assert.match(atmosphereUi, /'Match sun'/);
  assert.match(atmosphereUi, /lightGrid\.selected === LIGHT_TINTS/);
});

test("sky rotation and sun decal have separate renderer paths", () => {
  const header = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "SkyGpuRotation.h"),
    "utf8",
  );
  assert.match(header, /Skybox yaw\/pitch never\n\/\/ touch them/);
  assert.match(source, /RotateSkyMatrix\(rotated, 0, SkyGpuDelta\(\)/);
  assert.match(source, /IsSunSpriteCandidate\(context/);
});

test("sky-only orientation still captures constants and enters the GPU sky draw path", () => {
  const noteMap = source.slice(
    source.indexOf("void SkyGpuRotation_NoteMap("),
    source.indexOf("void SkyGpuRotation_NoteUnmap("),
  );
  const beginDraw = source.slice(
    source.indexOf("SkyGpuRotationScope SkyGpuRotation_BeginDraw("),
    source.indexOf("void SkyGpuRotation_EndDraw("),
  );
  assert.match(noteMap, /HasSkyOrientationOverride\(\).*HasSunOrientationOverride\(\)/s);
  assert.match(beginDraw, /HasSkyOrientationOverride\(\).*HasSunOrientationOverride\(\)/s);
});

test("Skybox settings contain only operator controls, not test-sky shortcuts", () => {
  const weatherUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudWeatherSkyTabJs.h"),
    "utf8",
  );
  assert.doesNotMatch(weatherUi, /Use Aztec|test sky|skyAztecBtn/i);
  assert.doesNotMatch(weatherUi, /bakedShadowToggle|dynamicShadowToggle|Hide sun glow/);
  assert.doesNotMatch(weatherUi, /mirv_sky clouds draw/);
  assert.match(weatherUi, /subHead\(orientSec, 'APPLIES TO'\)/);
  assert.match(weatherUi, /skyTargetSeg = segmented\(orientSec/);
  assert.match(weatherUi, /\['sky', 'Skybox'\].*\['fog', 'Fog \/ smoke'\].*\['both', 'Both'\]/);
  assert.match(weatherUi, /world skyorient target sky/);
  assert.match(weatherUi, /world skyorient target fog/);
});

test("Skybox and fog rotation switches are independent and race-safe", () => {
  const weatherUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudWeatherSkyTabJs.h"),
    "utf8",
  );
  const targets = weatherUi.slice(weatherUi.indexOf("var skyTargetSeg"),weatherUi.indexOf("var skyOrientResetBtn"));
  assert.match(targets, /skyTargetUi = key/);
  assert.match(targets, /target sky .*key === 'fog' \? '0' : '1'/);
  assert.match(targets, /target fog .*key === 'sky' \? '0' : '1'/);

});

test("sun visibility and the engine shadow switch both sit under Sun", () => {
  const lightingUi = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudLightingTabJs.h"),
    "utf8",
  );
  assert.match(lightingUi, /Hide sun/);
  assert.match(lightingUi, /mirv_filmmaker world sun visible/);
  assert.match(lightingUi, /litSec = section\(pageEffects, 'LIGHTING'.*light\.card/s);
  // Sun visibility and the engine shadow switch are both on the Sun page: they are two halves of
  // the same thing, and the mod's own shadow-map composite is no longer in the panel at all.
  assert.match(lightingUi, /shadowsToggle = togglePill\(sunPage, 'Engine shadows'/);
  assert.doesNotMatch(lightingUi, /Shader shadows/);
  assert.match(lightingUi, /mirv_filmmaker world shadows/);
  assert.match(lightingUi, /Bloom size/);
  assert.match(lightingUi, /world sun glow_size/);
  assert.doesNotMatch(lightingUi, /mirv_sky sun draw/);
});

test("sun disc and its large companion glow share one rigid transform and size anchor", () => {
  const gpu = source;
  const sun = fs.readFileSync(path.join(root, "AfxHookSource2", "Filmmaker", "World", "SunFx.cpp"), "utf8");
  assert.match(gpu, /SunDrawKind::Disc/);
  assert.match(gpu, /SunDrawKind::LargeGlow/);
  assert.match(gpu, /NearDiscAnchor\(/);
  assert.match(gpu, /stableCenter/);
  assert.match(gpu, /RotateAroundPoint\(scaled/);
  assert.match(gpu, /ReadBufferToCpu\(context, source, cache->original/);
  assert.doesNotMatch(gpu, /SunMaterialHintScope|s_sunMaterialHint/);
  assert.match(gpu, /SunFx_GlowSize\(\)/);
  assert.match(gpu, /glowSize/);
  assert.match(sun, /SunFx_SetGlowSize/);
  assert.match(sun, /sunGlowSize/);
});

test("hiding the sun keeps SolarRadius valid so clouds stay in the sky branch", () => {
  const sun = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "SunFx.cpp"),
    "utf8",
  );
  // The size may come through the scene-key transient (WorldTransient::Number("sunSize", g_size, ..)),
  // but the radius is always the snapshot times the size -- never zeroed when the sun is hidden.
  assert.match(sun, /rad\[0\] = snap\.radius \* (?:g_size|float\(WorldTransient::Number\("sunSize", g_size, \d+\)\))/);
  assert.doesNotMatch(sun, /rad\[0\] = g_visible \? snap\.radius \* g_size : 0\.0f/);
});

test("Live shadows never write a global shadow or sunlight gate", () => {
  const liveShadows = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "DynamicShadowFx.cpp"),
    "utf8",
  );
  assert.match(liveShadows, /csm_max_num_cascades_override/);
  assert.doesNotMatch(
    liveShadows,
    /"lb_enable_shadow_casting"|"lb_enable_sunlight"|"lb_enable_dynamic_shadows"/,
  );
});

test("Baked shadows drive lb_enable_baked_shadows only", () => {
  const bakedShadows = fs.readFileSync(
    path.join(root, "AfxHookSource2", "Filmmaker", "World", "BakedShadowFx.cpp"),
    "utf8",
  );
  assert.match(bakedShadows, /"lb_enable_baked_shadows"/);
  assert.doesNotMatch(bakedShadows, /"sc_disable_baked_lighting"/);
  assert.doesNotMatch(bakedShadows, /masterGateNeedsLiveOff|bake-isolate|ScanShadowRelatedCvars/);
  const targets = bakedShadows.match(/CvarTarget g_targets\[\] = \{([\s\S]*?)\n\};/);
  assert.ok(targets, "expected g_targets initializer");
  assert.doesNotMatch(targets[1], /lb_csm_override_staticgeo|lb_precomputed|sparseshadowtree/);
});
