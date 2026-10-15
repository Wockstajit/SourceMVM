#pragma once
#include "EnvironmentClimateMath.h"
namespace Filmmaker::Environment {
constexpr unsigned GiResolution = 128;
struct GiBounds {
    Vec3 low;
    float size;
};
// PLACED, and it stays placed. Centring this on the camera was tried and is wrong: bounce light
// that follows you is light with no fixed relationship to the room it is bouncing off, so the
// scene changes as you move through a shot instead of the shot being lit. The volume is a piece of
// set dressing you put somewhere -- `environment gi-center` drops it at the camera once.
inline GiBounds ClimateGiBounds(const Climate &c) {
    float size = c[CL_bounceSize];
    return {{c[CL_bounceX] - size * .5f, c[CL_bounceY] - size * .5f, c[CL_bounceZ] - size * .5f}, size};
}
inline Vec3 GiCoordinate(const GiBounds &b, Vec3 p) { return (p - b.low) * (1.f / b.size); }
inline Vec3 ClimateSunRadiance(const Climate &c) {
    float t = std::clamp((c[CL_sunElevation] + 4) / 30, 0.f, 1.f);
    float daylight = ClimateSunVisibility(c);
    // The same ramp and the same authored tint the surface shader uses, so the bounce agrees
    // with the direct light instead of drifting warm against it.
    const float gain = c[CL_sunPower] * daylight;
    return Vec3{gain * c[CL_sunColorR], gain * (.36f + .58f * t) * c[CL_sunColorG],
                gain * (.12f + .71f * t) * c[CL_sunColorB]};
}
} // namespace Filmmaker::Environment
