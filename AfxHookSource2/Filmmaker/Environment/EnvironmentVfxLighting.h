#pragma once
#include "../Relight/RelightMath.h"
#include <cstdint>
namespace Filmmaker { struct JsonValue; }
namespace Filmmaker::Environment {
// Renderer-independent source snapshot, also used by the native CS2 particle lights.
struct MollyLightSource {
    uint32_t handle=0;unsigned slot=0;int entity=-1;const char* kind="";
    Vec3 position{};float lumens=0,range=0,radius=0;
};
void ResetVfxLightingSources();
std::vector<MollyLightSource> CaptureMollyLightSources();
// Grenade and bomb explosions from the last moment, with their age in demo seconds. Flashbangs are
// light, not a shock wave, so they are not here. The ground fog is blown away by these.
struct VfxBlast { Vec3 position{}; float age = 0; };
std::vector<VfxBlast> CaptureVfxBlasts(double time);
// Where short-lived effects are drawn right now: explosion clouds while they last, flashes, muzzles.
// A radius shrinks to 0 as the effect ends, so nothing that reads it sees a jump.
struct VfxEffectPoint { Vec3 position{}; float radius = 0; };
std::vector<VfxEffectPoint> CaptureVfxEffectPoints(double time, Vec3 camera);
bool IsVfxLightingEvent(const char* name);
void OnVfxLightingEvent(const char* name, const JsonValue& data);
void CaptureVfxLights(Relight::Scene& scene, double time, const std::string& map, bool active);
}
