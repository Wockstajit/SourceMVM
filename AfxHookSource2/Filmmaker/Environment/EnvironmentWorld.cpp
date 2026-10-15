#include "EnvironmentWorld.h"
#include "EnvironmentBakedLighting.h"
#include "EnvironmentScene.h"
#include "../World/NightMode.h"
#include "../World/LightFx.h"
#include "../EasterEggs/EasterEggWorld.h"
#include "../EasterEggs/EasterEggUwu.h"
#include "../World/SunFx.h"
#include "../World/SkyOrient.h"
#include "../World/SkyboxLibrary.h"
#include "../World/CloudFx.h"
#include "../World/ShadowFx.h"
#include "../World/BakedShadowFx.h"
#include "../World/DynamicShadowFx.h"
#include "../World/CustomShadowFx.h"
#include "../World/WorldModulation.h"
#include "../Debug/MvmDebugLog.h"
#include "../../SceneSystem.h"
#include "../../MirvColors.h"
#include <algorithm>
namespace Filmmaker::Environment {
WorldLook CaptureWorldLook(WorldLookCapture source) {
    WorldLook look = DefaultWorldLook();
    auto &v = look.fields;
    look.sky = SkyboxLibrary_Current();
    int r, g, b;
    LightFx_GetTint(&r, &g, &b);
    v["exposure"] = {NightMode_Exposure()};
    v["lightColor"] = {double(r), double(g), double(b)};
    v["lightAmount"] = {LightFx_Amount()};
    v["lightUniform"] = {double(LightFx_Uniform())};
    v["lightDirection"] = {LightFx_SunYaw(), LightFx_SunPitch()};
    if (source == WorldLookCapture::User) {
        const auto lighting = EasterEggLighting_UserState();
        v["lightColor"] = {double(lighting.r), double(lighting.g), double(lighting.b)};
        v["lightAmount"] = {lighting.amount};
        v["lightUniform"] = {double(lighting.uniform)};
        v["lightDirection"][0] = lighting.yaw;
    }
    v["sunPositionOverride"] = {double(SunFx_PositionOverridden())};
    if (SunFx_PositionOverridden())
        v["sunPosition"] = {SunFx_Azimuth(), SunFx_Elevation()};
    v["sunSize"] = {SunFx_Size()};
    v["sunBrightness"] = {SunFx_Brightness()};
    v["sunGlow"] = {SunFx_Glow()};
    v["sunGlowSize"] = {SunFx_GlowSize()};
    v["sunVisible"] = {double(SunFx_Visible())};
    bool lights, baked, dynamic;
    CaptureNativeLighting(lights, baked, dynamic);
    v["engineLights"] = {double(lights)};
    v["shadows"] = {double(baked && dynamic)};
    v["shadowLayers"] = {double(baked), double(dynamic)};
    v["extraShadows"] = {double(CustomShadowFx_Enabled())};
    v["shadowStrength"] = {CustomShadowFx_Strength()};
    v["shadowStep"] = {CustomShadowFx_StepPixels()};
    v["shadowThickness"] = {CustomShadowFx_Thickness()};
    v["shadowSteps"] = {double(CustomShadowFx_Steps())};
    v["skyDirection"] = {SkyOrient_Yaw(), SkyOrient_Pitch()};
    v["skyMirror"] = {double(SkyOrient_Mirror())};
    v["skyTargets"] = {double(SkyOrient_SkyEnabled()), double(SkyOrient_FogEnabled())};
    int rgba[4];
    v["skyTint"] = {double(SceneSystem_GetSkyColor(false, rgba))};
    v["skyColor"] = {double(rgba[0]), double(rgba[1]), double(rgba[2]), double(rgba[3])};
    v["cloudTint"] = {double(SceneSystem_GetSkyColor(true, rgba))};
    v["cloudColor"] = {double(rgba[0]), double(rgba[1]), double(rgba[2])};
    v["cloudsVisible"] = {double(SceneSystem_CloudsVisible())};
    v["cloudDensity"] = {CloudFx_Density()};
    v["cloudBrightness"] = {CloudFx_Brightness()};
    v["cloudSize"] = {CloudFx_Size()};
    v["cloudDrift"] = {CloudFx_Drift()};
    v["worldStrength"] = {WorldModulation_Strength()};
    for (int i = 0; i < 4; ++i) {
        bool on;
        if (source == WorldLookCapture::User)
            EasterEggUwu_GetUserTint((ModCategory)i, &r, &g, &b, &on);
        else
            WorldModulation_GetTint((ModCategory)i, &r, &g, &b, &on);
        v["worldTint"][i] = on;
        v["worldColors"][i * 3] = r;
        v["worldColors"][i * 3 + 1] = g;
        v["worldColors"][i * 3 + 2] = b;
    }
    for (int i = 0; i < 2; ++i) {
        unsigned char rr = 255, gg = 255, bb = 255;
        v["smokeTint"][i] = MirvColors_GetSmokeColorRgb(i != 0, &rr, &gg, &bb);
        v["smokeColors"][i * 3] = rr;
        v["smokeColors"][i * 3 + 1] = gg;
        v["smokeColors"][i * 3 + 2] = bb;
    }
    return look;
}
bool PrepareWorldLook(const WorldLook &look, std::string &error) {
    if (look.sky.empty())
        return true;
    for (const auto &entry : SkyboxLibrary_Entries())
        if (entry.name == look.sky) {
            if (SceneSystem_ResolveMaterial(entry.material.c_str()))
                return true;
            error = "Sky material could not be loaded: " + look.sky;
            return false;
        }
    error = "Missing sky: " + look.sky;
    return false;
}
void ApplyWorldLook(const WorldLook &look, const std::string &page) {
    auto n = [&](const char *key, int i = 0) { return look.fields.at(key).at(i); };
    // Restore material tint ownership before sky/cloud backends restore their own values.
    if (page == "all" || page == "atmosphere") {
        WorldModulation_Reset();
        WorldModulation_SetStrength(float(n("worldStrength")));
        for (int i = 0; i < 4; ++i) {
            WorldModulation_SetTint((ModCategory)i, int(n("worldColors", i * 3)),
                                    int(n("worldColors", i * 3 + 1)), int(n("worldColors", i * 3 + 2)));
            WorldModulation_SetEnabled((ModCategory)i, n("worldTint", i) != 0);
        }
        for (int i = 0; i < 2; ++i)
            MirvColors_SetSmokeColor(i != 0, n("smokeTint", i) != 0, int(n("smokeColors", i * 3)),
                                     int(n("smokeColors", i * 3 + 1)), int(n("smokeColors", i * 3 + 2)));
    }
    if (page == "all" || page == "sky") {
        if (look.sky.empty())
            SkyboxLibrary_Default();
        else
            SkyboxLibrary_Apply(look.sky.c_str());
        SkyOrient_Reset();
        SkyOrient_SetYaw(float(n("skyDirection", 0)));
        SkyOrient_SetPitch(float(n("skyDirection", 1)));
        SkyOrient_SetMirror(n("skyMirror") != 0);
        SkyOrient_SetSkyEnabled(n("skyTargets", 0) != 0);
        SkyOrient_SetFogEnabled(n("skyTargets", 1) != 0);
        int rgba[] = {int(n("skyColor", 0)), int(n("skyColor", 1)), int(n("skyColor", 2)),
                      int(n("skyColor", 3))};
        SceneSystem_SetSkyColor(false, n("skyTint") != 0, rgba);
        rgba[0] = int(n("cloudColor", 0));
        rgba[1] = int(n("cloudColor", 1));
        rgba[2] = int(n("cloudColor", 2));
        rgba[3] = 255;
        SceneSystem_SetSkyColor(true, n("cloudTint") != 0, rgba);
        SceneSystem_SetCloudsVisible(n("cloudsVisible") != 0);
        CloudFx_ResetAll();
        if (n("cloudDensity") != 1)
            CloudFx_SetDensity(float(n("cloudDensity")));
        if (n("cloudBrightness") != 1)
            CloudFx_SetBrightness(float(n("cloudBrightness")));
        if (n("cloudSize") != 1)
            CloudFx_SetSize(float(n("cloudSize")));
        if (n("cloudDrift") != 1)
            CloudFx_SetDrift(float(n("cloudDrift")));
    }
    if (page == "all" || page == "lighting") {
        ResetEngineLightingTakeover();
        LightFx_Reset();
        SunFx_ResetAll();
        SunFx_ResetLightAngles();
        NightMode_SetExposure(float(n("exposure")));
        LightFx_SetTint(int(n("lightColor", 0)), int(n("lightColor", 1)), int(n("lightColor", 2)));
        LightFx_SetAmount(float(n("lightAmount")));
        LightFx_SetUniform(n("lightUniform") != 0);
        LightFx_SetSceneLightsEnabled(n("engineLights") != 0);
        if (n("lightDirection", 0) != 0)
            LightFx_SetSunYaw(float(n("lightDirection", 0)));
        if (n("lightDirection", 1) != 0)
            LightFx_SetSunPitch(float(n("lightDirection", 1)));
        if (n("sunPositionOverride")) {
            SunFx_SetAzimuth(float(n("sunPosition", 0)));
            SunFx_SetElevation(float(n("sunPosition", 1)));
        }
        if (n("sunSize") != 1)
            SunFx_SetSize(float(n("sunSize")));
        if (n("sunBrightness") != 1)
            SunFx_SetBrightness(float(n("sunBrightness")));
        if (n("sunGlow") != 1)
            SunFx_SetGlow(float(n("sunGlow")));
        SunFx_SetGlowSize(float(n("sunGlowSize")));
        SunFx_SetVisible(n("sunVisible") != 0);
        CustomShadowFx_SetEnabled(false);
        CustomShadowFx_ResetSettings();
        CustomShadowFx_SetStrength(float(n("shadowStrength")));
        CustomShadowFx_SetStepPixels(float(n("shadowStep")));
        CustomShadowFx_SetThickness(float(n("shadowThickness")));
        CustomShadowFx_SetSteps(int(n("shadowSteps")));
        BakedShadowFx_SetEnabled(n("shadowLayers", 0) != 0);
        DynamicShadowFx_SetEnabled(n("shadowLayers", 1) != 0);
        CustomShadowFx_SetEnabled(n("extraShadows") != 0);
    }
    MvmDebugLog_LinefAlways("environment.reset", "page=%s sky='%s' exposure=%.3f skyTint=%d", page.c_str(),
                            look.sky.c_str(), n("exposure"), int(n("skyTint")));
}
void WorldLook_OnLevelInit() {
    OnLevelInit();
    LightFx_OnLevelInit();
    BakedShadowFx_OnLevelInit();
    DynamicShadowFx_OnLevelInit();
    WorldModulation_OnLevelInit();
    SunFx_OnLevelInit();
    CloudFx_OnLevelInit();
    SkyOrient_OnLevelInit();
}
} // namespace Filmmaker::Environment
