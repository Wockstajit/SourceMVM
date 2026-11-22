#pragma once
// The weather cover volume (EnvironmentWeatherCoverMath.h): the worker that bakes it once per map
// and the texture the weather shaders read. Nothing here runs per frame once the bake is done.
#include "EnvironmentShadowMath.h"
#include <d3d11.h>
#include <memory>
#include <string>

namespace Filmmaker::Environment {
struct WeatherCoverBinding {
    ID3D11ShaderResourceView *view = nullptr; // null until the bake is done
    Vec3 low{}, perUnit{};                    // voxel coordinate = (world - low) * perUnit
    unsigned nx = 0, ny = 0, nz = 0;
};
// Render thread, every frame precipitation or a ground coating is on. Starts the bake for a new map
// and returns the finished volume.
WeatherCoverBinding PrepareWeatherCover(ID3D11Device *device, const std::shared_ptr<const OcclusionMesh> &world);
// One line for `environment status`: what the worker is doing and how long the bake took.
std::string WeatherCoverStatus();
// Stops the worker and frees the volume.
void EndWeatherCover();
// The finished volume's value at a world point (0 when there is none), for `environment cover`.
float WeatherCoverAt(Vec3 point);
// The height below which the sky is shut off over a point of the map, read down from `high` to
// `low` in steps: a street's own ground, a house's roof. `low` when the sky reaches all the way
// down, and when there is no volume yet (WeatherCoverReady says which).
float WeatherCoverTop(float x, float y, float low, float high, float step = 24);
bool WeatherCoverReady();
// Writes the map's walkable floor (the nav mesh) seen from above as a binary PPM, one pixel per 8
// units: light grey where open air is unbroken, green where it reaches in under cover (brighter is
// more), dark blue where it does not. For checking a map's interiors at a glance.
bool WriteWeatherCoverPlan(const OcclusionMesh &world, const std::string &path, std::string &error);
} // namespace Filmmaker::Environment
