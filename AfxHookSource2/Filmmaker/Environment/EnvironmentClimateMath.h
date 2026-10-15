#pragma once
#include "EnvironmentMath.h"
#include <array>
#include <map>
#include <string>
namespace Filmmaker::Environment {
// Shared schema: presets, controls, GPU constants and shader field names use this order.
enum ClimateField {
    CL_sky,
    CL_clouds,
    CL_coverage,
    CL_density,
    CL_cloudBase,
    CL_cloudDepth,
    CL_cloudScale,
    CL_windSpeed,
    CL_sunAzimuth,
    CL_sunElevation,
    CL_sunPower,
    CL_haze,
    CL_stars,
    CL_fogDensity,
    CL_fogHeight,
    CL_fogBase,
    CL_fogNoise,
    CL_fogRange,
    CL_scatter,
    CL_lightShafts,
    CL_snow,
    CL_wet,
    CL_puddles,
    CL_puddleHeight,
    CL_puddleDepth,
    CL_surfaceRadius,
    CL_surfaceX,
    CL_surfaceY,
    CL_maskScale,
    CL_ripple,
    CL_relight,
    CL_ambient,
    CL_sunRelight,
    CL_bakedSuppressed,
    CL_contactShadows,
    CL_particleDetail,
    CL_turbulence,
    CL_particleOpacity,
    CL_softness,
    CL_streak,
    CL_particleLight,
    CL_bloom,
    CL_bloomThreshold,
    CL_saturation,
    CL_contrast,
    CL_vignette,
    CL_grain,
    CL_chromatic,
    CL_fogStart,
    CL_localFogRadius,
    CL_fogX,
    CL_fogY,
    CL_fogZ,
    CL_enabled,
    CL_relighting,
    CL_ssr,
    CL_ssrRange,
    CL_ssrThickness,
    CL_gi,
    CL_giRange,
    CL_shadowRange,
    CL_shadowBias,
    CL_worldGi,
    CL_bounce,
    CL_bounceSize,
    CL_bounceX,
    CL_bounceY,
    CL_bounceZ,
    CL_bounceAlbedo,
    CL_sceneReflections,
    CL_reflectionRoughness,
    CL_storm,
    CL_stormInterval,
    CL_stormStrength,
    CL_stormRadius,
    CL_stormX,
    CL_stormY,
    CL_stormZ,
    CL_stormSeed,
    CL_stormHold,
    // Retained for old presets. Coverage puddles always use the shader film; native wet-ground
    // meshes were removed because regenerating them stalled every slider edit.
    CL_puddleNative,
    // Multiplies the elevation ramp in SunColor()/ClimateSunRadiance. 1,1,1 is the shipped look,
    // so an untouched scene renders exactly as before.
    CL_sunColorR,
    CL_sunColorG,
    CL_sunColorB,
    // Angular diameter of the sun in degrees (the real sun is 0.53). Sets the sun shadow penumbra
    // physically: its width is (distance from caster) * 2 * tan(size / 2), so contact points stay
    // sharp and a shadow softens with distance. See EnvironmentSunVisibilityMath.h.
    CL_sunAngle,
    // Flat irradiance the unbaked replay feeds through the map's own irradiance input while
    // CL_bakedSuppressed is on, so the native shader still applies albedo and occlusion to it.
    // Grey on purpose: Environment's ambient and tint colour it in the composite.
    CL_bakedFill,
    CL_mapLightPower,
    // Owned ambient occlusion and runtime sky visibility switches (Relight Scene ao/skyOcclusion).
    CL_ao,
    CL_skyOcclusion,
    CL_lampAngle,
    CL_shadowSoftness,
    CL_bounceSteps,
    ClimateCount
};
struct ClimateSpec {
    const char *key;
    const char *page;
    float initial, lo, hi;
};
inline const std::array<ClimateSpec, ClimateCount> &ClimateSpecs() {
    static const std::array<ClimateSpec, ClimateCount> values = {{
        {"sky", "sky", 0.0f, 0.0f, 1.0f},
        // On by default. A procedural sky with no cloud layer reads as a flat gradient, so
        // switching the sky on -- or resetting the card -- brings clouds with it; the Clouds page
        // switch still turns them off independently afterwards.
        // Off by default: the Sky card's switch turns sky AND clouds on together. Defaulting it on
        // made ANY card that enables the Environment pass (Weather, Fog) bring up the cloud sky.
        {"clouds", "sky", 0.0f, 0.0f, 1.0f},
        {"coverage", "sky", 0.55f, 0.0f, 1.0f},
        {"density", "sky", 0.65f, 0.0f, 2.0f},
        {"cloudBase", "sky", 1800.0f, -2000.0f, 32000.0f},
        {"cloudDepth", "sky", 1200.0f, 100.0f, 8000.0f},
        {"cloudScale", "sky", 0.0008f, 0.0001f, 0.01f},
        {"windSpeed", "sky", 25.0f, -600.0f, 600.0f},
        {"sunAzimuth", "lighting", 135.0f, 0.0f, 360.0f},
        {"sunElevation", "lighting", 35.0f, -30.0f, 90.0f},
        {"sunPower", "lighting", 1.0f, 0.0f, 8.0f},
        {"haze", "sky", 0.4f, 0.0f, 2.0f},
        {"stars", "sky", 0.3f, 0.0f, 1.0f},
        {"fogDensity", "atmosphere", 0.0f, 0.0f, 6.0f},
        {"fogHeight", "atmosphere", 320.0f, 20.0f, 3000.0f},
        {"fogBase", "atmosphere", 0.0f, -4000.0f, 32000.0f},
        {"fogNoise", "atmosphere", 0.4f, 0.0f, 1.0f},
        {"fogRange", "atmosphere", 4000.0f, 128.0f, 12000.0f},
        {"scatter", "atmosphere", 0.45f, 0.0f, 0.85f},
        {"lightShafts", "atmosphere", 1.0f, 0.0f, 1.0f},
        {"snow", "weather", 0.0f, 0.0f, 1.0f},
        {"wet", "weather", 0.0f, 0.0f, 1.0f},
        {"puddles", "weather", 0.0f, 0.0f, 1.0f},
        {"puddleHeight", "weather", 128.0f, -4000.0f, 8000.0f},
        {"puddleDepth", "weather", 24.0f, 1.0f, 200.0f},
        {"surfaceRadius", "weather", 0.0f, 0.0f, 8000.0f},
        {"surfaceX", "weather", 0.0f, -32768.0f, 32768.0f},
        {"surfaceY", "weather", 0.0f, -32768.0f, 32768.0f},
        {"maskScale", "weather", 0.02f, 0.002f, 0.1f},
        {"ripple", "weather", 0.35f, 0.0f, 2.0f},
        {"relight", "lighting", 0.0f, 0.0f, 1.0f},
        {"ambient", "lighting", 1.0f, 0.0f, 2.0f},
        {"sunRelight", "lighting", 0.6f, 0.0f, 3.0f},
        {"bakedSuppressed", "lighting", 0.0f, 0.0f, 1.0f},
        {"contactShadows", "lighting", 1.0f, 0.0f, 1.0f},
        {"particleDetail", "weather", 1.0f, 0.0f, 1.0f},
        {"turbulence", "weather", 1.0f, 0.0f, 8.0f},
        {"particleOpacity", "weather", 0.65f, 0.0f, 1.0f},
        {"softness", "weather", 8.0f, 0.1f, 64.0f},
        {"streak", "weather", 12.0f, 1.0f, 50.0f},
        {"particleLight", "weather", 0.4f, 0.0f, 2.0f},
        {"bloom", "atmosphere", 0.0f, 0.0f, 2.0f},
        {"bloomThreshold", "atmosphere", 0.8f, 0.1f, 8.0f},
        {"saturation", "atmosphere", 1.0f, 0.0f, 2.0f},
        {"contrast", "atmosphere", 1.0f, 0.25f, 2.0f},
        {"vignette", "atmosphere", 0.0f, 0.0f, 1.0f},
        {"grain", "atmosphere", 0.0f, 0.0f, 0.1f},
        {"chromatic", "atmosphere", 0.0f, 0.0f, 4.0f},
        {"fogStart", "atmosphere", 0.0f, 0.0f, 12000.0f},
        {"localFogRadius", "atmosphere", 0.0f, 0.0f, 8000.0f},
        {"fogX", "atmosphere", 0.0f, -32768.0f, 32768.0f},
        {"fogY", "atmosphere", 0.0f, -32768.0f, 32768.0f},
        {"fogZ", "atmosphere", 0.0f, -32768.0f, 32768.0f},
        {"enabled", "presets", 0.0f, 0.0f, 1.0f},
        {"relighting", "lighting", 0.0f, 0.0f, 1.0f},
        {"ssr", "weather", 0.0f, 0.0f, 1.0f},
        {"ssrRange", "weather", 1800.0f, 100.0f, 6000.0f},
        {"ssrThickness", "weather", 12.0f, 1.0f, 64.0f},
        {"gi", "lighting", 0.0f, 0.0f, 2.0f},
        {"giRange", "lighting", 200.0f, 20.0f, 800.0f},
        {"shadowRange", "atmosphere", 3000.0f, 256.0f, 8000.0f},
        {"shadowBias", "lighting", 2.0f, 0.1f, 16.0f},
        {"worldGi", "lighting", 0, 0, 1},
        {"bounce", "lighting", 1, 0, 4},
        {"bounceSize", "lighting", 2048, 512, 8192},
        {"bounceX", "lighting", 0, -32768, 32768},
        {"bounceY", "lighting", 0, -32768, 32768},
        {"bounceZ", "lighting", 0, -32768, 32768},
        {"bounceAlbedo", "lighting", .55f, .05f, .95f},
        {"sceneReflections", "lighting", 0, 0, 1},
        {"reflectionRoughness", "weather", .35f, 0, 1},
        {"storm", "weather", 0, 0, 1},
        {"stormInterval", "weather", 9, 2, 60},
        {"stormStrength", "weather", 2, 0, 8},
        {"stormRadius", "weather", 1600, 100, 8000},
        {"stormX", "weather", 0, -32768, 32768},
        {"stormY", "weather", 0, -32768, 32768},
        {"stormZ", "weather", 0, -8000, 32000},
        {"stormSeed", "weather", 13, 0, 65535},
        {"stormHold", "weather", 0, 0, 1},
        {"puddleNative", "weather", 0, 0, 1},
        {"sunColorR", "lighting", 1.0f, 0.0f, 2.0f},
        {"sunColorG", "lighting", 1.0f, 0.0f, 2.0f},
        {"sunColorB", "lighting", 1.0f, 0.0f, 2.0f},
        {"sunAngle", "lighting", 0.53f, 0.0f, 5.0f},
        {"bakedFill", "lighting", 0.6f, 0.0f, 4.0f},
        {"mapLightPower", "lighting", 1.0f, 0.0f, 8.0f},
        {"ao", "lighting", 1, 0, 1},
        {"skyOcclusion", "lighting", 1, 0, 1},
        {"lampAngle", "lighting", 1, 0, 5},
        {"shadowSoftness", "lighting", 1, 0, 4},
        {"bounceSteps", "lighting", 1, 1, 3},

    }};
    return values;
}
struct Climate {
    std::array<float, ((ClimateCount + 3) / 4) * 4> values{};
    Climate() {
        for (int i = 0; i < ClimateCount; ++i)
            values[i] = ClimateSpecs()[i].initial;
    }
    float &operator[](ClimateField field) { return values[field]; }
    float operator[](ClimateField field) const { return values[field]; }
};
inline void ResetClimatePage(Climate &c, const std::string &page) {
    for (int i = 0; i < ClimateCount; ++i)
        if (page == "all" || page == ClimateSpecs()[i].page)
            c.values[i] = ClimateSpecs()[i].initial;
    // Reset releases feature switches as well as tuning. Cast shadows, AO and sky occlusion are
    // quality parts of dynamic lighting, not features of their own: they stay on (their initial
    // value) so turning relighting on always gives shadowed, occluded interiors.
    for (ClimateField field : {CL_enabled, CL_sky, CL_clouds, CL_relighting,
         CL_worldGi, CL_bakedSuppressed,
         CL_lightShafts, CL_storm, CL_stormHold, CL_puddleNative,
         CL_sceneReflections})
        if (page == "all" || page == ClimateSpecs()[field].page) c[field] = 0;
}
inline bool ClimateValueValid(int i, double n) {
    if (i < 0 || i >= ClimateCount || !std::isfinite(n) || n < ClimateSpecs()[i].lo ||
        n > ClimateSpecs()[i].hi)
        return false;
    return (i != CL_bakedSuppressed && i != CL_enabled &&
            i != CL_relighting && i != CL_worldGi && i != CL_bounceSteps && i != CL_storm && i != CL_stormSeed && i != CL_stormHold && i != CL_puddleNative) ||
           std::floor(n) == n;
}
inline Climate EffectiveClimate(Climate c) {
    c[CL_ssr] = 0;
    c[CL_gi] = 0; // Retained schema slots for old presets; camera-dependent transport is removed.
    if (c[CL_enabled] == 0) {
        c[CL_bakedSuppressed] = 0;
        c[CL_storm] = 0;
        c[CL_sky] = 0;
        c[CL_clouds] = 0;
        c[CL_fogDensity] = 0;
        c[CL_sunAzimuth] = ClimateSpecs()[CL_sunAzimuth].initial;
        c[CL_sunElevation] = ClimateSpecs()[CL_sunElevation].initial;
        c[CL_sunPower] = 0;
    }
    if (c[CL_enabled] == 0 || c[CL_relighting] == 0) {
        c[CL_relight] = 0;
        c[CL_sunRelight] = 0;
        c[CL_gi] = 0;
    }
    c[CL_puddleNative] = 0;
    return c;
}
// Whether anything this frame samples the world sun shadow map, from the effective climate.
// `lighting` is the frame's owned lighting: a captured frame hands Lighting to the native relight
// path and zeroes CL_relighting first. `receivers` covers placed props and the cascade debug view.
// Fog on its own never asks: its light shafts are the sun's shadow, so they wait for the Sky or
// Lighting card to put a sun up.
inline bool ClimateSamplesSunShadow(const Climate &c, bool lighting, bool receivers) {
    if (c[CL_enabled] <= 0 || c[CL_contactShadows] <= 0 || c[CL_sunElevation] < 0 || c[CL_sunPower] <= 0)
        return false;
    const bool lit = lighting || c[CL_relighting] > 0;
    return receivers || lit || c[CL_worldGi] > 0 || c[CL_clouds] > 0 ||
           c[CL_snow] > 0 || c[CL_wet] > 0 || c[CL_puddles] > 0 ||
           (c[CL_sky] > 0 && c[CL_fogDensity] > 0 && c[CL_lightShafts] > 0);
}
// Direct solar energy stops at the horizon. Twilight belongs to sky/indirect
// lighting and must not produce a sun disk or specular highlight below the map.
inline float ClimateSunVisibility(const Climate &c) {
    float visible=std::clamp(c[CL_sunElevation],0.f,1.f);
    return visible*visible*(3-2*visible);
}
inline Vec3 ClimateSunDirection(const Climate &c) {
    float a = Radians(c[CL_sunAzimuth]), e = Radians(c[CL_sunElevation]);
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}
// The passes a restart switches off, with the value that means off: sky, lighting, fog and
// weather. Each is restored by a card switch that reads `held` (Sky and Fog through envPassPill,
// Weather through its own switch) or is a plain 0/1 switch. The grade (bloom, saturation, ...) is
// left alone: it costs nothing to keep and no switch could restore a held grade value.
inline const std::array<std::pair<ClimateField, float>, 10> &RestoredPassNeutrals() {
    static const std::array<std::pair<ClimateField, float>, 10> list{{
        {CL_sky, 0.f}, {CL_clouds, 0.f}, {CL_storm, 0.f}, {CL_relighting, 0.f}, {CL_worldGi, 0.f},
        {CL_bakedSuppressed, 0.f}, {CL_fogDensity, 0.f}, {CL_snow, 0.f}, {CL_wet, 0.f}, {CL_puddles, 0.f}}};
    return list;
}
// Key under which a held weather type is kept beside the climate keys.
inline const char *HeldWeatherTypeKey() { return "weatherType"; }

// A reopened demo, or a restarted game, keeps every saved value but starts with every pass off:
// re-applying sky, weather, fog or lighting at map load is a crash risk nobody asked for. Each
// pass that was on moves its value into `held` and goes neutral; switching that card on later puts
// the held value back (the UI reads `held`). A value already held from an earlier restore stays
// held. The renderer's master switch follows: nothing is on, so it is off.
inline void HoldRestoredPasses(Climate &c, int &weatherType, std::map<std::string, float> &held) {
    for (const auto &pass : RestoredPassNeutrals()) {
        const float value = c[pass.first];
        if (std::abs(value - pass.second) > 1e-4f) {
            held[ClimateSpecs()[pass.first].key] = value;
            c[pass.first] = pass.second;
        }
    }
    if (weatherType != 0) {
        held[HeldWeatherTypeKey()] = float(weatherType);
        weatherType = 0;
    }
    c[CL_enabled] = 0;
}
// Baked daylight painted into the 3D skybox and into unlit shafts. The material
// path never multiplies those draws, so the composite and the shaft blend scale
// them by how high the sun is. The shader in EnvironmentShaders.h uses these edges.
inline float BakedDaylightScale(float sunElevation, float ambient) {
    const float span = 8.f - (-10.f);
    float t = std::clamp((sunElevation - (-10.f)) / span, 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    const float nightFloor = (std::max)(std::clamp(ambient, 0.f, 1.f) * .35f, .02f);
    return nightFloor + (1.f - nightFloor) * t;
}
inline float UnlitOverlayScale(float sunElevation, float ambient) {
    const float span = 8.f - (-10.f);
    float t = std::clamp((sunElevation - (-10.f)) / span, 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    return BakedDaylightScale(sunElevation, ambient) * (.55f + .45f * t);
}
inline void CoupleSunToLighting(Climate &c, ClimateField changed) {
    if (changed != CL_sunAzimuth && changed != CL_sunElevation && changed != CL_sunPower &&
        changed != CL_sunColorR && changed != CL_sunColorG && changed != CL_sunColorB)
        return;
    c[CL_enabled] = 1;
    c[CL_sky] = 1;
    c[CL_relighting] = 1;
    // Zero flattening is an authored value; sun edits must preserve material contrast.
}
inline float SnowSlope(float up) {
    float t = std::clamp((up - .35f) / .5f, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline int ClimateSteps(const Climate &, bool exporting = false) {
    return exporting ? 64 : 32;
}
inline std::string ClimateShaderDefines() {
    std::string s = "#define CLIMATE_VECTORS " + std::to_string((ClimateCount + 3) / 4) + "\n";
    // Coating actors (players, which get no world coating). Keep in step with Frame::kMaxActors --
    // the cbuffer array and the C++ upload are sized from these two and must agree.
    s += "#define COATING_ACTORS 12\n";
    for (int i = 0; i < ClimateCount; ++i)
        s += "#define CL_" + std::string(ClimateSpecs()[i].key) + " climateData[" + std::to_string(i / 4) +
             "][" + std::to_string(i % 4) + "]\n";
    return s;
}
} // namespace Filmmaker::Environment
