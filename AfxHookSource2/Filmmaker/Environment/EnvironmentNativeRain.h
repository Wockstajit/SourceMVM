#pragma once
#include "EnvironmentScene.h"
#include <string>
namespace Filmmaker::Environment {
// CS2 native world weather supplies base weapon/player wetness. Precipitation and active
// impacts use the environment renderer so maps without Valve rain-trace data are supported.
// The optional native rain_storm particle is disabled on those maps.
//
// Main thread only. `active` is the environment's own "renderer may run on this map" state.
void UpdateNativeRain(const Scene &scene, const std::string &map, bool active, Vec3 camera, bool cameraValid);
// Level change: particle indices belong to the old manager and must not be destroyed or reused.
void ResetNativeRain();
// True while the native system is running, so the shader rain stands down (render thread reads it).
bool NativeRainActive();
// One line for `environment status` / diagnostics.
const char *NativeRainStatus();
// The map's WORLD weather, as de_train's info_map_parameters sets it: registering the world as
// rain-exposed turns on CS2's own wet/droplet shading on first-person weapons, gloves and arms, and
// the strength/ripple/wetness values drive materials authored with F_WETNESS. Main thread only.
struct NativeWorldWeather {
    bool exposed = false;
    float rain = 0, ripple = 0, rippleDirection = 0, wetness = 0, drying = 0;
};
NativeWorldWeather NativeWorldWeatherFor(const Scene &scene, bool active);
// Writes the report for `environment mapinfo` into `out`.
void NativeWorldWeatherReport(char *out, size_t size);
// Pure mapping, kept here so the render test and UI agree: precipitation count -> rain_storm CP3.x
// (the system maps 1..4 onto drop opacity and trail length).
inline float NativeRainIntensity(int count) {
    float t = count <= 0 ? 0.f : (count >= 6000 ? 1.f : float(count) / 6000.f);
    return 1.f + 3.f * t;
}
} // namespace Filmmaker::Environment
