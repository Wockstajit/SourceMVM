#include "EnvironmentBakedLighting.h"
#include "../World/LightFx.h"
#include "../World/SunFx.h"
#include "../World/ShadowFx.h"
#include "../World/BakedShadowFx.h"
#include "../World/DynamicShadowFx.h"
#include "../Debug/MvmDebugLog.h"
namespace Filmmaker::Environment {
namespace {
bool lightingOwned = false, savedBakedShadows = true, savedDynamicShadows = true;
bool savedEngineLights=true;
bool skyOwned = false, savedSunVisible = true;
} // namespace
void SetEngineLightingTakeover(bool on) {
    if (on && !lightingOwned) {
        savedBakedShadows = ::Filmmaker::BakedShadowFx_Enabled();
        savedDynamicShadows = ::Filmmaker::DynamicShadowFx_Enabled();
        savedEngineLights=::Filmmaker::LightFx_SceneLightsEnabled();
        lightingOwned = true;
        MvmDebugLog_LinefAlways("environment.lighting",
                                "takeover=1 savedBakedShadows=%d savedDynamicShadows=%d savedEngineLights=%d tintUnchanged=1",
                                savedBakedShadows ? 1 : 0, savedDynamicShadows ? 1 : 0, savedEngineLights ? 1 : 0);
        // Take the engine's own direct lighting away, once, on the frame the pass starts. All
        // four have to go together or the map keeps the lighting we are replacing and ours is
        // merely added on top -- which is what "the baked shadows never went away" looks like.
        //   sun      the engine's directional light
        //   baked    lb_enable_baked_shadows, the light binner's baked shadow contribution
        //   dynamic  the live cascades
        //   scene    the map's own placed lights
        // Asserting once rather than every frame costs no per-frame cvar traffic and lets a
        // deliberate World & Effects edit made while the pass runs still win.
        ::Filmmaker::LightFx_SetEnvironmentSunSuppressed(true);
        ::Filmmaker::BakedShadowFx_SetEnabled(false);
        ::Filmmaker::DynamicShadowFx_SetEnabled(false);
        ::Filmmaker::LightFx_SetSceneLightsEnabled(false);
    }
    if (on)
        return;
    if (!lightingOwned)
        return;
    // Put back exactly what was found. The pass borrows these switches; it does not own them.
    ::Filmmaker::LightFx_SetEnvironmentSunSuppressed(false);
    ::Filmmaker::BakedShadowFx_SetEnabled(savedBakedShadows);
    ::Filmmaker::DynamicShadowFx_SetEnabled(savedDynamicShadows);
    ::Filmmaker::LightFx_SetSceneLightsEnabled(savedEngineLights);
    lightingOwned = false;
    MvmDebugLog_LinefAlways("environment.lighting",
                            "takeover=0 restoredBakedShadows=%d restoredDynamicShadows=%d restoredEngineLights=%d",
                            savedBakedShadows ? 1 : 0, savedDynamicShadows ? 1 : 0, savedEngineLights ? 1 : 0);
}
void ResetEngineLightingTakeover() { SetEngineLightingTakeover(false); }
// The procedural sky draws its own sun disc and halo out of SunColor(). CS2's own sun disc and
// its sun_disc_glow ring are separate objects on separate materials, and replacing the sky
// backdrop does not remove them -- so at angles where the map's authored sun sits behind
// geometry its halo still burns through, in the old colour, next to ours. This borrows the
// engine's sun visibility for as long as our sky is up and hands it back exactly as found.
void SetEngineSunDiscSuppressed(bool on) {
    if (on && !skyOwned) {
        savedSunVisible = ::Filmmaker::SunFx_Visible();
        skyOwned = true;
        ::Filmmaker::SunFx_SetVisible(false);
        MvmDebugLog_LinefAlways("environment.sky", "sunDiscSuppressed=1 savedVisible=%d",
                                savedSunVisible ? 1 : 0);
    }
    if (on)
        return;
    if (!skyOwned)
        return;
    ::Filmmaker::SunFx_SetVisible(savedSunVisible);
    skyOwned = false;
    MvmDebugLog_LinefAlways("environment.sky", "sunDiscSuppressed=0 restoredVisible=%d",
                            savedSunVisible ? 1 : 0);
}
void ResetEngineSunDiscSuppression() { SetEngineSunDiscSuppressed(false); }
void CaptureNativeLighting(bool &lights, bool &baked, bool &dynamic) {
    // While the pass owns these, report what the user chose rather than our override, so the
    // World & Effects switches keep showing their own state instead of reading as flipped off.
    lights = lightingOwned ? savedEngineLights : ::Filmmaker::LightFx_SceneLightsEnabled();
    baked = lightingOwned ? savedBakedShadows : ::Filmmaker::BakedShadowFx_Enabled();
    dynamic = lightingOwned ? savedDynamicShadows : ::Filmmaker::DynamicShadowFx_Enabled();
}
} // namespace Filmmaker::Environment
