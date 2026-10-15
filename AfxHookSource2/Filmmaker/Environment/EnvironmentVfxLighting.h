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
bool IsVfxLightingEvent(const char* name);
void OnVfxLightingEvent(const char* name, const JsonValue& data);
void CaptureVfxLights(Relight::Scene& scene, double time, const std::string& map, bool active);
}
