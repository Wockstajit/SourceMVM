#pragma once
#include "EnvironmentMath.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace Filmmaker::Environment {
// Runtime sky visibility for the ambient term.
//
// The owned ambient is sky light, and without it the shader lights a sealed interior and a
// narrow alley exactly as brightly as an open square: that reads as light leaking through
// walls and as the flat blue shaded regions. Real-time engines solve this with sky occlusion
// (Unreal's distance-field sky occlusion, Frostbite's sky visibility). Here the map's own
// render geometry is drawn into whole-map orthographic depth maps from a fixed set of sky
// directions; a receiver's visibility is the fraction of those directions that reach it,
// weighted by how much each direction contributes to a surface with its normal. The depths
// are generated in this session from geometry, never read from Valve's baked data, and the
// sky's colour and brightness still come from the live settings every frame.
inline constexpr unsigned SkyVisibilitySize = 1024;
inline constexpr unsigned SkyVisibilityDirections = 14;

// Zenith, a ring of five at 55 degrees and a ring of eight at 22 degrees: roughly equal solid
// angle per direction over the upper hemisphere. Unit vectors pointing TOWARD the sky.
inline std::array<Vec3, SkyVisibilityDirections> SkyVisibilityDirectionSet() {
    std::array<Vec3, SkyVisibilityDirections> d{};
    d[0] = {0, 0, 1};
    unsigned n = 1;
    auto ring = [&](float elevationDegrees, unsigned count, float offsetDegrees) {
        const float e = Radians(elevationDegrees);
        for (unsigned i = 0; i < count; ++i) {
            const float a = Radians(offsetDegrees + 360.f * i / count);
            d[n++] = {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
        }
    };
    ring(55, 5, 0);
    ring(22, 8, 22.5f);
    return d;
}

// Per-direction weight for a receiver normal: cosine lobe, so a floor is dominated by the
// zenith and a wall by the low ring on its own side. A small floor keeps downward faces from
// dividing by zero; they fall back to the plain average.
inline float SkyVisibilityWeight(Vec3 normal, Vec3 direction) {
    return (std::max)(0.f, Dot(normal, direction)) + .02f;
}

// CPU reference of the shader's combination: visible[i] in [0,1] per direction.
inline float SkyVisibility(Vec3 normal, const std::array<float, SkyVisibilityDirections> &visible) {
    const auto directions = SkyVisibilityDirectionSet();
    float sum = 0, weight = 0;
    for (unsigned i = 0; i < SkyVisibilityDirections; ++i) {
        const float w = SkyVisibilityWeight(normal, directions[i]);
        sum += w * visible[i];
        weight += w;
    }
    return weight > 0 ? sum / weight : 1;
}

// Applied to the ambient visibility the native material multiplies its ambient by.
inline float ApplySkyVisibility(float ambientOcclusion, float skyVisibility, float strength) {
    const float s = std::clamp(strength, 0.f, 1.f);
    return ambientOcclusion * (1 - s + s * std::clamp(skyVisibility, 0.f, 1.f));
}
} // namespace Filmmaker::Environment
