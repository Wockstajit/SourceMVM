#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Filmmaker::Environment {
// Physically based sun shadow softness (the "Sun size" setting).
//
// The sun is not a point: seen from a receiver it is a disc of angular diameter A (0.53 degrees for
// the real one). A blocker `d` units in front of the receiver along the light therefore hides only
// part of that disc, and the shadow edge ramps over a penumbra of width
//
//     w = d * 2 * tan(A / 2)
//
// so a contact point is sharp (d = 0) and the shadow softens in proportion to the distance to the
// caster. The owned sun-visibility pass (EnvironmentSunVisibilityShaders.h) evaluates exactly this
// per pixel: a blocker search measures d, then a filter of radius w / 2 averages the shadow map.
inline constexpr unsigned SunSearchTaps = 24, SunFilterTaps = 32;
struct SunDiskTap {float x, y, radius, unused;};
template<unsigned Count, bool Search> inline std::array<SunDiskTap, Count> SunDiskTaps() {
    std::array<SunDiskTap, Count> result{};
    for(unsigned i=0;i<Count;++i){
        const float t=(i+.5f)/Count, radius=Search?t*t:std::sqrt(t), angle=i*2.39996323f;
        result[i]={std::cos(angle)*radius,std::sin(angle)*radius,radius,0};
    }
    return result;
}
inline constexpr float SunRealDiameterDegrees = 0.53f;
inline constexpr float SunDiameterMaxDegrees = 5.0f;

inline float SunTanHalfAngle(float diameterDegrees) {
    const float clamped = (std::clamp)(std::isfinite(diameterDegrees) ? diameterDegrees : SunRealDiameterDegrees,
                                     0.f, SunDiameterMaxDegrees);
    return std::tan(clamped * 3.14159265358979f / 360.f);
}
// Full penumbra width, in world units, on a receiver `distance` behind its blocker.
inline float SunPenumbraWidth(float distance, float diameterDegrees) {
    return 2.f * (std::max)(0.f, distance) * SunTanHalfAngle(diameterDegrees);
}
// Radius of the filter disc for that penumbra, in shadow-map texels. `minTexels` keeps a contact
// from aliasing: it is the smallest radius the filter is ever allowed (about one bilinear footprint).
inline float SunFilterRadiusTexels(float distance, float diameterDegrees, float texelWorld, float minTexels = .75f) {
    if (!(texelWorld > 0.f)) return minTexels;
    return (std::max)(minTexels, .5f * SunPenumbraWidth(distance, diameterDegrees) / texelWorld);
}
// Blocker search radius in world units: the widest penumbra a blocker `maxDistance` away could
// cast, which is the farthest a ray offset by half the disc can still reach the receiver.
inline float SunSearchRadius(float maxDistance, float diameterDegrees) {
    return (std::max)(0.f, maxDistance) * SunTanHalfAngle(diameterDegrees);
}
// A blocker at lateral offset `offset` and distance `distance` in front of the receiver hides part of
// the sun disc exactly when it lies inside the cone the disc subtends from the receiver.
inline bool SunBlockerInsideCone(float offset, float distance, float diameterDegrees) {
    return offset <= (std::max)(0.f, distance) * SunTanHalfAngle(diameterDegrees) + 1e-6f;
}
// Fraction of a straight shadow edge's penumbra covered at lateral position x past the geometric
// edge (negative inside the shadow), i.e. the visibility an ideal filter gives. 0 = umbra, 1 = lit.
inline float SunEdgeVisibility(float x, float penumbraWidth) {
    if (!(penumbraWidth > 0.f)) return x > 0.f ? 1.f : 0.f;
    return (std::clamp)(.5f + x / penumbraWidth, 0.f, 1.f);
}
} // namespace Filmmaker::Environment
