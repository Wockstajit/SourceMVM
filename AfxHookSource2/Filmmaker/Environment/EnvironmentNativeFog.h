#pragma once
#include "EnvironmentScene.h"
#include <string>

namespace Filmmaker::Environment {

// Synchronizes CS2's native Source 2 `C_GradientFog` entity with Filmmaker's Half-Life: Alyx
// fog settings on the main thread, snapshotting map-authored values on first touch and
// restoring them cleanly when disabled or on level change. Main thread only.
void UpdateNativeFog(const Scene &scene, const std::string &map, bool active, Vec3 camera, bool cameraValid);
void ResetNativeFog();

// True while CS2's native C_GradientFog is actively driven by Half-Life: Alyx fog mode so
// the complementary volumetric scatter/god-ray pass avoids double-fogging opaque surfaces.
bool NativeAlyxFogActive();

} // namespace Filmmaker::Environment
