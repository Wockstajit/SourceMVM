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
    // GROUND FOG: the low layer that lies on the floor and parts around players (density 0 = off).
    // Thickness over the floor, billow detail, how hard bodies push it, seconds to refill a cleared
    // trail, eddy strength, and the drift that carries it (speed, compass direction).
    CL_groundFog,
    CL_groundFogHeight,
    CL_groundFogDetail,
    CL_groundFogFlow,
    CL_groundFogRecovery,
    CL_groundFogSwirl,
    CL_groundFogWind,
    CL_groundFogWindAngle,
    // How much of the floor the fog covers: 1 is an unbroken sheet, lower breaks it into patches the
    // fluid carries, which is what makes the moving air easy to see.
    CL_groundFogCoverage,
    // The ground fog's own colour. It defaults to a warm white: the height fog's grey-blue reads as a
    // dirty film on the floor, and a dense low fog is white where the sun reaches it.
    CL_groundFogColorR,
    CL_groundFogColorG,
    CL_groundFogColorB,
    // THE LOOK. From a defined sheet with a top surface to thin torn mist: how gradually it fades upward,
    // how much fine torn detail it has, how big that detail is and how far the drift stretches it, how
    // big the rolling lumps are, how fast it all churns. And its light: the sun on it, and the colour
    // and brightness of the fog in shade. None of these touch the fluid players push.
    CL_groundFogSoftness,
    CL_groundFogWisps,
    CL_groundFogWispSize,
    CL_groundFogStreak,
    CL_groundFogBillowSize,
    CL_groundFogChurn,
    CL_groundFogSun,
    CL_groundFogShade,
    CL_groundFogShadeR,
    CL_groundFogShadeG,
    CL_groundFogShadeB,
    // How wide a player's mark on the fog is: a share of the body's size that scales the clearing, the
    // shove and the heaped rim together.
    CL_groundFogPushSize,
    // How deep the snow lies where it has drifted (1 = about shin deep). Deep snow is parted by the
    // people who walk through it; thin snow only takes their prints.
    CL_snowDepth,
    // A fill light on players under dynamic lighting: how many times brighter their darkest shade
    // is drawn (1 = as lit). Players take their ambient from sparse probes and stand out black in
    // shade next to walls lit per pixel; the lift fades out as a pixel gets brighter.
    CL_playerFill,
    // Wind gusts you can see in the rain or snow: fronts that sweep downwind, in which the
    // precipitation is thicker, leans or streaks with the wind, and blown mist or snow fills the
    // air (GustFront in EnvironmentShaders.h). 0 is steady weather, 1 a storm or a blizzard.
    CL_gusts,
    // THE GROUND DRIFT the gusts blow along the floor (GustVolumeMarch, GustLookFor): each scales the
    // weather's own look, 1 as it comes. How dense the blown layer is, how high it stands, how much
    // it is torn into wisps (0 an even sheet), and how strongly loose snow streams over the ground and
    // a gust roughens wet ground (ApplyCoatings).
    CL_gustDensity,
    CL_gustHeight,
    CL_gustWisps,
    CL_gustFloor,
    // THE MOON (EnvironmentSkyMath.h). Brightness (0 = no moon), where it stands, its phase (0 new,
    // 0.5 full, 1 new again), its apparent size in degrees (the real moon is 0.52) and its colour.
    // Once the sun has set it is the key light: the map is lit and shadowed from it.
    CL_moon,
    CL_moonAzimuth,
    CL_moonElevation,
    CL_moonPhase,
    CL_moonSize,
    CL_moonColorR,
    CL_moonColorG,
    CL_moonColorB,
    // How much rain and storms darken the sky's cloud layer (0 = they leave it as it is).
    CL_cloudRain,
    // THE ENVIRONMENT'S OWN AUTO EXPOSURE (World/AutoExposure.h). While Environment lighting is on
    // the tonemapper's exposure follows the relit frame, so a map lit by our sun and sky is not left
    // at the exposure its baked lighting was authored for (which read far too bright on Mirage and
    // Dust2). On by default; `exposureBias` raises or lowers the result, in stops. Nothing to do with
    // the in-game Auto exposure under Effects > Lighting, which keeps its own switch and settings.
    CL_autoExposure,
    CL_exposureBias,
    // TIME OF DAY (Sky > Sun & moon; ApplyTimeOfDay below). The sun and the moon are placed by the
    // hour (0-24), by which way the sun's path across the sky faces (the compass direction it stands
    // in at noon) and by how high it climbs at noon. The four position values above (sun and moon
    // azimuth and elevation) are worked out from these and are not set by hand.
    CL_timeOfDay,
    CL_sunPath,
    CL_sunHeight,
    // The map's own clouds (geometry in its 3D skybox) hidden under the map's own sky, so the
    // Environment's cloud layer can stand in for them. With the Environment sky on they are always
    // hidden.
    CL_hideMapClouds,
    // How dark the map's and the players' sun shadows make the ground fog (0 = the fog ignores
    // them). The ground fog's own: the Fog page's Light shafts switch is the height fog's.
    CL_groundFogShadows,
    // CS2's beads of water on wet ground (EnvironmentGameTextures.h), read from the installed game.
    // 0 keeps the plain wet film. (The snow always takes CS2's own snow texture.)
    CL_rainTexture,
    // SAND on the ground (Weather > Sand): drawn through the snow's path (cover, drifts, footprints,
    // the hole each foot sinks in) with its own CC0 Poly Haven textures (EnvironmentGameTextures.h) and
    // its own look. How much lies, how deep it lies where it has drifted (1 = about ankle deep), which
    // texture style is drawn (0 Dusty Mix, 1 Cracked Crust, 2 Pebbly Grit, 3 Trampled), continuous colour
    // harmonization (0 = each scan's original captured colour, 1 = unified desert sand palette), and the
    // harmonized target sand colour (linear RGB). Where both sand and snow are set, the larger one lies.
    CL_sand,
    CL_sandDepth,
    // TUMBLEWEEDS (EnvironmentTumbleweeds.cpp): how many roll through the map at once, carried by the
    // wind. They come in from out of sight upwind and leave out of sight downwind.
    CL_tumbleweeds,
    CL_sandStyle,
    CL_sandHarmonize,
    CL_sandColorR,
    CL_sandColorG,
    CL_sandColorB,
    // Whether snow/sand also coats props (cars, crates, barrels) above the walkable floor,
    // including subtle side/edge accumulation. 0 keeps coating on walkable ground and wall bases.
    CL_snowProps,
    CL_sandProps,
    // Whether the map's fill lights (Light::fill) shine under Environment lighting.
    CL_fillLights,
    // Volumetric light scattering intensity multiplier for sun, placed lights, and map lamps in height fog.
    CL_fogLightIntensity,
    // Half-Life: Alyx 3-layer volumetric + gradient + sky-bounce fog mode for height fog and weather.
    CL_alyxFog,
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
        // Where the default time of day puts the sun (15:15, path 68.68 degrees, 60 degrees at noon).
        {"sunAzimuth", "sky", 135.0f, 0.0f, 360.0f},
        {"sunElevation", "sky", 34.82f, -30.0f, 90.0f},
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
        {"groundFog", "atmosphere", 0.0f, 0.0f, 4.0f},
        {"groundFogHeight", "atmosphere", 56.0f, 8.0f, 400.0f},
        {"groundFogDetail", "atmosphere", 0.6f, 0.0f, 1.0f},
        {"groundFogFlow", "atmosphere", 1.0f, 0.0f, 3.0f},
        {"groundFogRecovery", "atmosphere", 8.0f, 0.5f, 60.0f},
        {"groundFogSwirl", "atmosphere", 0.8f, 0.0f, 2.0f},
        {"groundFogWind", "atmosphere", 12.0f, 0.0f, 200.0f},
        {"groundFogWindAngle", "atmosphere", 30.0f, 0.0f, 360.0f},
        {"groundFogCoverage", "atmosphere", 0.85f, 0.05f, 1.0f},
        {"groundFogColorR", "atmosphere", 0.93f, 0.0f, 1.0f},
        {"groundFogColorG", "atmosphere", 0.91f, 0.0f, 1.0f},
        {"groundFogColorB", "atmosphere", 0.87f, 0.0f, 1.0f},
        {"groundFogSoftness", "atmosphere", 0.35f, 0.0f, 1.0f},
        {"groundFogWisps", "atmosphere", 0.0f, 0.0f, 1.0f},
        {"groundFogWispSize", "atmosphere", 36.0f, 8.0f, 240.0f},
        {"groundFogStreak", "atmosphere", 0.2f, 0.0f, 1.0f},
        {"groundFogBillowSize", "atmosphere", 220.0f, 60.0f, 1000.0f},
        {"groundFogChurn", "atmosphere", 1.0f, 0.0f, 5.0f},
        {"groundFogSun", "atmosphere", 1.0f, 0.0f, 3.0f},
        {"groundFogShade", "atmosphere", 1.0f, 0.0f, 3.0f},
        {"groundFogShadeR", "atmosphere", 1.0f, 0.0f, 1.0f},
        {"groundFogShadeG", "atmosphere", 1.0f, 0.0f, 1.0f},
        {"groundFogShadeB", "atmosphere", 1.0f, 0.0f, 1.0f},
        {"groundFogPushSize", "atmosphere", 0.55f, 0.2f, 2.0f},
        {"snowDepth", "weather", 0.5f, 0.0f, 1.0f},
        {"playerFill", "lighting", 1.8f, 1.0f, 4.0f},
        {"gusts", "weather", 0.0f, 0.0f, 1.0f},
        {"gustDensity", "weather", 1.0f, 0.0f, 2.0f},
        {"gustHeight", "weather", 1.0f, 0.25f, 2.0f},
        {"gustWisps", "weather", 1.0f, 0.0f, 2.0f},
        {"gustFloor", "weather", 1.0f, 0.0f, 2.0f},
        {"moon", "sky", 1.0f, 0.0f, 4.0f},
        // A full moon at the default time: opposite the sun, below the horizon.
        {"moonAzimuth", "sky", 315.0f, 0.0f, 360.0f},
        {"moonElevation", "sky", -30.0f, -30.0f, 90.0f},
        {"moonPhase", "sky", 0.5f, 0.0f, 1.0f},
        {"moonSize", "sky", 1.5f, 0.1f, 12.0f},
        {"moonColorR", "sky", 0.75f, 0.0f, 2.0f},
        {"moonColorG", "sky", 0.85f, 0.0f, 2.0f},
        {"moonColorB", "sky", 1.0f, 0.0f, 2.0f},
        {"cloudRain", "sky", 1.0f, 0.0f, 1.0f},
        {"autoExposure", "lighting", 1.0f, 0.0f, 1.0f},
        {"exposureBias", "lighting", 0.0f, -3.0f, 3.0f},
        {"timeOfDay", "sky", 15.25f, 0.0f, 24.0f},
        {"sunPath", "sky", 68.68f, 0.0f, 360.0f},
        {"sunHeight", "sky", 60.0f, 5.0f, 90.0f},
        {"hideMapClouds", "sky", 0.0f, 0.0f, 1.0f},
        {"groundFogShadows", "atmosphere", 1.0f, 0.0f, 1.0f},
        {"rainTexture", "weather", 1.0f, 0.0f, 2.0f},
        {"sand", "weather", 0.0f, 0.0f, 1.0f},
        {"sandDepth", "weather", 0.5f, 0.0f, 1.0f},
        {"tumbleweeds", "weather", 0.0f, 0.0f, 16.0f},
        {"sandStyle", "weather", 0.0f, 0.0f, 3.0f},
        {"sandHarmonize", "weather", 1.0f, 0.0f, 1.0f},
        {"sandColorR", "weather", 0.62f, 0.0f, 1.0f},
        {"sandColorG", "weather", 0.51f, 0.0f, 1.0f},
        {"sandColorB", "weather", 0.34f, 0.0f, 1.0f},
        {"snowProps", "weather", 1.0f, 0.0f, 1.0f},
        {"sandProps", "weather", 1.0f, 0.0f, 1.0f},
        {"fillLights", "lighting", 1.0f, 0.0f, 1.0f},
        {"fogLightIntensity", "atmosphere", 1.0f, 0.0f, 4.0f},
        {"alyxFog", "atmosphere", 0.0f, 0.0f, 1.0f},


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
    // value) so turning relighting on always gives shadowed, occluded interiors. Light shafts are
    // the same kind of thing for the fogs (they draw nothing without one), and stay on as well: a
    // reset used to leave them off where a new scene has them on, so the two were not the same
    // starting point.
    for (ClimateField field : {CL_enabled, CL_sky, CL_clouds, CL_relighting,
         CL_worldGi, CL_bakedSuppressed,
         CL_storm, CL_stormHold, CL_puddleNative,
         CL_sceneReflections})
        if (page == "all" || page == ClimateSpecs()[field].page) c[field] = 0;
}
inline bool ClimateValueValid(int i, double n) {
    if (i < 0 || i >= ClimateCount || !std::isfinite(n) || n < ClimateSpecs()[i].lo ||
        n > ClimateSpecs()[i].hi)
        return false;
    return (i != CL_bakedSuppressed && i != CL_enabled &&
            i != CL_relighting && i != CL_worldGi && i != CL_bounceSteps && i != CL_storm && i != CL_stormSeed && i != CL_stormHold && i != CL_puddleNative &&
            i != CL_tumbleweeds && i != CL_sandStyle) ||
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
        c[CL_groundFog] = 0;
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
// Direct solar energy stops at the horizon. Twilight belongs to sky/indirect
// lighting and must not produce a sun disk or specular highlight below the map.
// These two are the sun itself: what the sky draws (its disc, the daylight and the twilight).
inline float ClimateTrueSunVisibility(const Climate &c) {
    float visible=std::clamp(c[CL_sunElevation],0.f,1.f);
    return visible*visible*(3-2*visible);
}
inline Vec3 ClimateTrueSunDirection(const Climate &c) {
    float a = Radians(c[CL_sunAzimuth]), e = Radians(c[CL_sunElevation]);
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}
inline float SkySmooth(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline Vec3 ClimateMoonDirection(const Climate &c) {
    const float a = Radians(c[CL_moonAzimuth]), e = Radians(c[CL_moonElevation]);
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}
// Lit share of the moon's disc for a phase (0 new, 0.5 full, 1 new again).
inline float MoonIllumination(float phase) { return .5f - .5f * std::cos(phase * 6.28318531f); }
// TIME OF DAY. One slider moves the sun and the moon together, the way they move.
// The sun goes round one arc a day: it comes up at 6:00 on the horizon a quarter turn before
// `sunPath`, stands highest at noon in the `sunPath` direction, `sunHeight` degrees up, goes down at
// 18:00 a quarter turn after it, and at midnight is as far below the horizon as it was above it at
// noon. The moon goes round the same arc opposite the sun, whatever its phase: it rises as the sun
// sets and is up all night. The phase changes only how it looks and how much light it gives (the
// real moon's place follows its phase, so a new or a quarter moon was gone at night, and moving the
// Phase slider moved the moon across the sky instead of changing its shape).
inline Vec3 DayArcDirection(float hours, float path, float height) {
    const float t = (hours - 6.f) / 12.f * 3.14159265f, h = Radians(path), e = Radians(height);
    const Vec3 rise{std::cos(h - 1.57079633f), std::sin(h - 1.57079633f), 0};
    const Vec3 noon{std::cos(e) * std::cos(h), std::cos(e) * std::sin(h), std::sin(e)};
    return {rise.x * std::cos(t) + noon.x * std::sin(t), rise.y * std::cos(t) + noon.y * std::sin(t), noon.z * std::sin(t)};
}
inline void DirectionAngles(Vec3 d, float &azimuth, float &elevation) {
    elevation = std::asin(std::clamp(d.z, -1.f, 1.f)) * 57.2957795f;
    azimuth = std::atan2(d.y, d.x) * 57.2957795f;
    if (azimuth < 0) azimuth += 360.f;
    if (azimuth >= 360.f) azimuth -= 360.f;
}
// Places the sun and the moon (their azimuth and elevation) from the time of day and the sun's path
// and height. Elevations stop at the lowest the sky draws (-30, deep night).
inline void ApplyTimeOfDay(Climate &c) {
    float azimuth, elevation;
    DirectionAngles(DayArcDirection(c[CL_timeOfDay], c[CL_sunPath], c[CL_sunHeight]), azimuth, elevation);
    c[CL_sunAzimuth] = azimuth;
    c[CL_sunElevation] = std::clamp(elevation, -30.f, 90.f);
    DirectionAngles(DayArcDirection(c[CL_timeOfDay] - 12.f, c[CL_sunPath], c[CL_sunHeight]), azimuth, elevation);
    c[CL_moonAzimuth] = azimuth;
    c[CL_moonElevation] = std::clamp(elevation, -30.f, 90.f);
}
// The other way: the sun was placed by its angles (the map's own sun on load, a starting look, a
// `climate sunAzimuth` command). Finds the time and the path that put it there, keeping the noon
// height unless the sun stands higher (or deeper at night) than it allows. Of the two times of day
// with the sun at that height (morning and afternoon, or before dawn and after dusk), the one
// nearer the time it was is kept. ApplyTimeOfDay afterwards gives back the same angles.
inline void TimeOfDayFromSun(Climate &c) {
    const float elevation = c[CL_sunElevation];
    const float height = std::clamp((std::max)(c[CL_sunHeight], std::abs(elevation)), 5.f, 90.f);
    c[CL_sunHeight] = height;
    const float s = std::clamp(std::sin(Radians(elevation)) / std::sin(Radians(height)), -1.f, 1.f);
    const float first = std::asin(s), second = 3.14159265f - first;
    auto hours = [](float t) { float h = std::fmod(6.f + t / 3.14159265f * 12.f, 24.f); return h < 0 ? h + 24.f : h; };
    auto apart = [](float a, float b) { float d = std::abs(a - b); return (std::min)(d, 24.f - d); };
    const float now = c[CL_timeOfDay];
    const float t = apart(hours(first), now) <= apart(hours(second), now) ? first : second;
    // Where the sun stands round from the path's noon direction at that point of the arc.
    const float offset = std::atan2(-std::cos(t), std::sin(t) * std::cos(Radians(height))) * 57.2957795f;
    float path = std::fmod(c[CL_sunAzimuth] - offset, 360.f);
    if (path < 0) path += 360.f;
    c[CL_timeOfDay] = std::clamp(hours(t), 0.f, 24.f);
    c[CL_sunPath] = path;
}
inline bool IsTimeOfDayField(ClimateField f) { return f == CL_timeOfDay || f == CL_sunPath || f == CL_sunHeight; }
// THE CLIMATE COLOURS a scene key animates as ONE colour (`env.<id>`), the way CONFIG edits each
// with one colour bar: three consecutive climate fields from `red` on. A key holds 0-255 per
// component, CONFIG's bar, where 255 is `scale` (the sun and moon bars run to 2, a brighter-than-
// white tint). Three separate floats would key red without green and blend through odd hues.
struct ClimateColour { const char *id; ClimateField red; float scale; };
static_assert(CL_sunColorB == CL_sunColorR + 2 && CL_moonColorB == CL_moonColorR + 2 &&
              CL_groundFogColorB == CL_groundFogColorR + 2 && CL_groundFogShadeB == CL_groundFogShadeR + 2 &&
              CL_sandColorB == CL_sandColorR + 2,
              "a keyed climate colour is three consecutive fields");
inline const std::array<ClimateColour, 5> &ClimateColours() {
    static const std::array<ClimateColour, 5> list{{{"sunColor", CL_sunColorR, 2.f}, {"moonColor", CL_moonColorR, 2.f},
        {"groundFogColor", CL_groundFogColorR, 1.f}, {"groundFogShadeColor", CL_groundFogShadeR, 1.f},
        {"sandColor", CL_sandColorR, 1.f}}};
    return list;
}
inline double ClimateColourToKey(const ClimateColour &k, float value) {
    return std::clamp(double(value) / k.scale * 255., 0., 255.);
}
inline float ClimateColourFromKey(const ClimateColour &k, int component, double key) {
    const auto &spec = ClimateSpecs()[k.red + component];
    return std::clamp(float(key / 255. * k.scale), spec.lo, spec.hi);
}
// How far the moon has taken over from the sun: none while the sun is up, all of it by the end of
// civil twilight (sun 6 degrees down), and only while the moon itself is above the horizon.
inline float MoonTakeover(const Climate &c) {
    return SkySmooth(-c[CL_sunElevation] / 6.f) * SkySmooth(c[CL_moonElevation]);
}
// Moonlight on the map relative to a power-1 sun: a full moon at brightness 1 is 10 % of it.
inline constexpr float kMoonLightScale = .1f;
inline float ClimateMoonPower(const Climate &c) {
    return (std::max)(c[CL_moon], 0.f) * MoonIllumination(c[CL_moonPhase]) * MoonTakeover(c) * kMoonLightScale;
}
// THE KEY LIGHT the map is lit and shadowed by: the sun while it is up, the moon once it has set.
// At the hand-over both are dark (the sun fades out over its last degree, the moon in over the first
// six of twilight), so the light changing direction never shows. HLSL twin: MoonKey() in
// EnvironmentClimateShaders.h.
inline bool ClimateMoonKey(const Climate &c) { return c[CL_sunElevation] < 0 && c[CL_moon] > 0; }
inline float ClimateSunVisibility(const Climate &c) {
    return ClimateMoonKey(c) ? SkySmooth(c[CL_moonElevation]) : ClimateTrueSunVisibility(c);
}
inline Vec3 ClimateSunDirection(const Climate &c) {
    return ClimateMoonKey(c) ? ClimateMoonDirection(c) : ClimateTrueSunDirection(c);
}
// Scalar strength of the key light (sun power times its horizon fade, or the moon's power).
inline float ClimateKeyPower(const Climate &c) {
    return ClimateMoonKey(c) ? ClimateMoonPower(c) : (std::max)(c[CL_sunPower], 0.f) * ClimateTrueSunVisibility(c);
}
// Whether anything this frame samples the world sun shadow map, from the effective climate.
// `lighting` is the frame's owned lighting: a captured frame hands Lighting to the native relight
// path and zeroes CL_relighting first. `receivers` covers placed props and the cascade debug view.
// The height fog asks only while its Light shafts switch is on: the shafts ARE the sun's shadow
// through it. The ground fog asks while its own Shadows slider is above 0. The climate sun starts as
// the map's own light_environment, so both fall where the map's sunlight does without the Sky or
// Lighting card.
inline bool ClimateSamplesSunShadow(const Climate &c, bool lighting, bool receivers) {
    if (c[CL_enabled] <= 0 || c[CL_contactShadows] <= 0)
        return false;
    if (ClimateMoonKey(c) ? ClimateMoonPower(c) <= 0 : (c[CL_sunElevation] < 0 || c[CL_sunPower] <= 0))
        return false;
    const bool lit = lighting || c[CL_relighting] > 0;
    return receivers || lit || c[CL_worldGi] > 0 || c[CL_clouds] > 0 ||
           c[CL_snow] > 0 || c[CL_sand] > 0 || c[CL_tumbleweeds] > 0 || c[CL_wet] > 0 || c[CL_puddles] > 0 ||
           (c[CL_fogDensity] > 0 && c[CL_lightShafts] > 0) || (c[CL_groundFog] > 0 && c[CL_groundFogShadows] > 0);
}
// The passes a restart switches off, with the value that means off: sky, lighting, fog and
// weather. Each is restored by a card switch that reads `held` (Sky and Fog through envPassPill,
// Weather through its own switch) or is a plain 0/1 switch. The grade (bloom, saturation, ...) is
// left alone: it costs nothing to keep and no switch could restore a held grade value.
inline const std::array<std::pair<ClimateField, float>, 13> &RestoredPassNeutrals() {
    static const std::array<std::pair<ClimateField, float>, 13> list{{
        {CL_sky, 0.f}, {CL_clouds, 0.f}, {CL_storm, 0.f}, {CL_relighting, 0.f}, {CL_worldGi, 0.f},
        {CL_bakedSuppressed, 0.f}, {CL_fogDensity, 0.f}, {CL_groundFog, 0.f}, {CL_snow, 0.f}, {CL_wet, 0.f},
        {CL_puddles, 0.f}, {CL_sand, 0.f}, {CL_tumbleweeds, 0.f}}};
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
        changed != CL_sunColorR && changed != CL_sunColorG && changed != CL_sunColorB &&
        changed != CL_timeOfDay && changed != CL_sunPath && changed != CL_sunHeight)
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
// Keep CL_alyxFog out of climateData so CLIMATE_VECTORS stays 33 and non-fog shaders keep their cached .cso bytecode;
// FrameConstants uploads it in depthRange.z instead.
inline constexpr int kShaderClimateCount = int(CL_alyxFog);
inline constexpr int kShaderClimateVectors = (kShaderClimateCount + 3) / 4;
inline std::string ClimateShaderDefines() {
    std::string s = "#define CLIMATE_VECTORS " + std::to_string(kShaderClimateVectors) + "\n";
    // Coating actors (players, which get no world coating). Keep in step with Frame::kMaxActors --
    // the cbuffer array and the C++ upload are sized from these two and must agree.
    s += "#define COATING_ACTORS 12\n";
    for (int i = 0; i < kShaderClimateCount; ++i)
        s += "#define CL_" + std::string(ClimateSpecs()[i].key) + " climateData[" + std::to_string(i / 4) +
             "][" + std::to_string(i % 4) + "]\n";
    s += "#define CL_alyxFog depthRange.z\n";
    return s;
}
// The sky light both fogs scatter when the Environment sky is off (fogAmbient in the shaders): the
// map's own authored sky colour, normalised to the brightness the fog has always had and mixed half
// and half with its neutral grey-blue, so maps differ in tint without one map's fog going dark.
// Without map data it is the neutral colour alone.
inline Vec3 FogAmbientFromMapSky(Vec3 skySrgb, bool known) {
    const Vec3 neutral{.45f, .5f, .6f};
    if (!known || !Finite(skySrgb)) return neutral;
    auto linear = [](float c) {
        c = std::clamp(c, 0.f, 1.f);
        return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
    };
    Vec3 sky{linear(skySrgb.x), linear(skySrgb.y), linear(skySrgb.z)};
    const float luminance = sky.x * .2126f + sky.y * .7152f + sky.z * .0722f;
    if (luminance < 1e-4f) return neutral;
    const float target = neutral.x * .2126f + neutral.y * .7152f + neutral.z * .0722f;
    sky = sky * (target / luminance);
    return {(neutral.x + (std::min)(sky.x, 1.5f)) * .5f, (neutral.y + (std::min)(sky.y, 1.5f)) * .5f,
            (neutral.z + (std::min)(sky.z, 1.5f)) * .5f};
}
// Which share of CS2's transparent coverage on a camera ray is a placed smoke. Mirrors
// SmokeRayShare in EnvironmentShaders.h: 1 when the ray passes through a smoke sphere, fading to 0
// between 1.15 and 1.6 radii from its centre; `entry` is where along the ray that smoke starts, -1
// when the ray misses every smoke. Coverage on other rays is glass, fire or foliage, not smoke.
// With no smoke positions at all (the hook hands coverage over only while smokes exist) every
// covered ray is taken as smoke from the camera on.
inline float SmokeRayShare(Vec3 camera, Vec3 dir, float maxT, const Vec3 *centers, const float *radii, int count,
                           float &entry) {
    if (count <= 0) {
        entry = 0;
        return 1;
    }
    entry = -1;
    float share = 0;
    for (int i = 0; i < count; ++i) {
        const float r = radii[i];
        if (!(r > 0)) continue;
        const Vec3 oc = camera - centers[i];
        const float b = Dot(oc, dir);
        const float closest = std::clamp(-b, 0.f, maxT);
        const Vec3 nearest = oc + dir * closest;
        const float miss = std::sqrt(Dot(nearest, nearest));
        const float t0 = std::clamp((miss - r * 1.15f) / (r * .45f), 0.f, 1.f);
        const float w = 1 - t0 * t0 * (3 - 2 * t0);
        if (w <= 0) continue;
        const float h = b * b - (Dot(oc, oc) - r * r);
        const float t = h >= 0 ? (std::max)(-b - std::sqrt(h), 0.f) : closest;
        if (t > maxT) continue;
        if (entry < 0 || t < entry) entry = t;
        share = (std::max)(share, w);
    }
    return share;
}
} // namespace Filmmaker::Environment
