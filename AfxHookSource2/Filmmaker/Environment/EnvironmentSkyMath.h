#pragma once
// THE ENVIRONMENT SKY: the atmosphere by day and by moonlight, and the light it sends to the map.
// The key light (sun by day, moon by night) lives in EnvironmentClimateMath.h.
// The HLSL twin is EnvironmentSkyRadianceShaders.h; change both together. Tests: environment-sky-math-tests.
//
// Sources (nothing here is our own model):
//  - Atmosphere: a port of wwwtyro/glsl-atmosphere (The Unlicense), a single-scattering Rayleigh and
//    Mie ray march over a spherical planet (16 view steps, 8 light steps, Rayleigh phase and the
//    Cornette-Shanks Mie phase). Constants are the repo's README values.
//  - Night sky: Jensen, Durand, Stark, Premoze, Dorsey, Shirley, "A Physically-Based Night Sky Model"
//    (SIGGRAPH 2001): the moonlit sky is the daylight sky model lit from the moon's direction, the moon
//    disc is a lit sphere with the Lommel-Seeliger law, and the dark side keeps a little earthshine.
#include "EnvironmentClimateMath.h"
#include <algorithm>
#include <cmath>
namespace Filmmaker::Environment {
namespace SkyAtmosphere {
inline constexpr float kPlanetRadius = 6371e3f, kAtmosphereRadius = 6471e3f, kEyeHeight = 1e3f;
inline constexpr float kRayleighR = 5.5e-6f, kRayleighG = 13.0e-6f, kRayleighB = 22.4e-6f;
inline constexpr float kMie = 21e-6f, kRayleighHeight = 8e3f, kMieHeight = 1.2e3f, kMieG = .758f;
inline constexpr float kSunIntensity = 22.f;
// The README's sun of 22 lands the horizon about a third brighter than the previous procedural sky;
// this keeps the new sky in the exposure range the maps and the tonemapper were tuned for.
inline constexpr float kSkyExposure = .75f;
inline constexpr int kViewSteps = 16, kLightSteps = 8;
// Horizon haze scales the Mie (aerosol) coefficient around the README value: the old default 0.4 is
// the reference sky, 0 a clear deep blue, 2 a milky one.
inline float MieScale(float haze) { return (std::max)(haze, 0.f) / .4f; }
// The moonlit sky is the same scattering lit by the moon (Jensen et al.), much dimmer than sunlight.
inline constexpr float kMoonSkyScale = .07f;
// The night floor (airglow and starlight) is the previous procedural sky's night gradient.
inline Vec3 NightFloor(float rayZ) {
    const float h = std::pow(std::clamp(rayZ, 0.f, 1.f), .4f);
    const Vec3 horizon{.018f, .022f, .05f}, zenith{.002f, .004f, .018f};
    return horizon * (1 - h) + zenith * h;
}
struct Span { float x, y; };
inline Span RaySphere(Vec3 origin, Vec3 dir, float radius) {
    const float b = 2 * Dot(dir, origin), c = Dot(origin, origin) - radius * radius;
    const float d = b * b - 4 * c;
    if (d < 0) return {1e5f, -1e5f};
    const float s = std::sqrt(d);
    return {(-b - s) * .5f, (-b + s) * .5f};
}
// Single scattering toward `ray` from a light in `light`, unit intensity. Rays below the horizon are
// lifted onto it: the map hides the ground, and a planet-hitting ray would go black.
inline Vec3 Scatter(Vec3 ray, Vec3 light, float mieScale, int viewSteps = kViewSteps, int lightSteps = kLightSteps) {
    ray.z = (std::max)(ray.z, .01f);
    ray = Normalize(ray);
    const Vec3 origin{0, 0, kPlanetRadius + kEyeHeight};
    Span p = RaySphere(origin, ray, kAtmosphereRadius);
    if (p.x > p.y) return {};
    // The eye is inside the atmosphere: march from the eye, and stop at the ground only when it is
    // ahead (the repo clamps to the planet root unconditionally, which is behind an upward ray).
    const float ground = RaySphere(origin, ray, kPlanetRadius).x;
    if (ground > 0) p.y = (std::min)(p.y, ground);
    p.x = (std::max)(p.x, 0.f);
    const float step = (p.y - p.x) / viewSteps, kM = kMie * mieScale;
    const float mu = Dot(ray, light), mumu = mu * mu, gg = kMieG * kMieG;
    const float pi = 3.14159265f;
    const float phaseR = 3.f / (16.f * pi) * (1 + mumu);
    const float phaseM = 3.f / (8.f * pi) * ((1 - gg) * (mumu + 1)) /
                         (std::pow((std::max)(1 + gg - 2 * mu * kMieG, 1e-4f), 1.5f) * (2 + gg));
    Vec3 totalR{}, totalM{};
    float odR = 0, odM = 0, t = p.x;
    for (int i = 0; i < viewSteps; ++i, t += step) {
        const Vec3 pos = origin + ray * (t + step * .5f);
        const float h = std::sqrt(Dot(pos, pos)) - kPlanetRadius;
        const float dR = std::exp(-h / kRayleighHeight) * step, dM = std::exp(-h / kMieHeight) * step;
        odR += dR; odM += dM;
        const float lightStep = RaySphere(pos, light, kAtmosphereRadius).y / lightSteps;
        float lR = 0, lM = 0, lt = 0;
        for (int j = 0; j < lightSteps; ++j, lt += lightStep) {
            const Vec3 q = pos + light * (lt + lightStep * .5f);
            const float lh = std::sqrt(Dot(q, q)) - kPlanetRadius;
            lR += std::exp(-lh / kRayleighHeight) * lightStep;
            lM += std::exp(-lh / kMieHeight) * lightStep;
        }
        const float m = kM * (odM + lM), r = odR + lR;
        const Vec3 attenuation{std::exp(-(m + kRayleighR * r)), std::exp(-(m + kRayleighG * r)),
                               std::exp(-(m + kRayleighB * r))};
        totalR = totalR + Vec3{attenuation.x * dR, attenuation.y * dR, attenuation.z * dR};
        totalM = totalM + Vec3{attenuation.x * dM, attenuation.y * dM, attenuation.z * dM};
    }
    return {phaseR * kRayleighR * totalR.x + phaseM * kM * totalM.x,
            phaseR * kRayleighG * totalR.y + phaseM * kM * totalM.y,
            phaseR * kRayleighB * totalR.z + phaseM * kM * totalM.z};
}
} // namespace SkyAtmosphere

// The sky's radiance toward `ray` without the sun disc, the moon disc or stars: what lights the map.
// Mirrors SkyRadiance in EnvironmentSkyRadianceShaders.h (fast steps).
inline Vec3 ClimateSkyRadiance(const Climate &c, Vec3 ray, int viewSteps = 8, int lightSteps = 4) {
    using namespace SkyAtmosphere;
    const float day = SkySmooth((c[CL_sunElevation] + 14) / 19);
    const Vec3 moon = ClimateMoonDirection(c);
    const float moonGain = (std::max)(c[CL_moon], 0.f) * MoonIllumination(c[CL_moonPhase]) *
                           SkySmooth((moon.z + .02f) / .07f) * (1 - day);
    const float mie = MieScale(c[CL_haze]);
    const Vec3 sun = Scatter(ray, ClimateTrueSunDirection(c), mie, viewSteps, lightSteps);
    Vec3 color{sun.x * c[CL_sunColorR], sun.y * c[CL_sunColorG], sun.z * c[CL_sunColorB]};
    if (moonGain > 1e-4f) {
        const Vec3 m = Scatter(ray, moon, mie, viewSteps, lightSteps) * (moonGain * kMoonSkyScale);
        color = color + Vec3{m.x * c[CL_moonColorR], m.y * c[CL_moonColorG], m.z * c[CL_moonColorB]};
    }
    return color * (kSkyExposure * kSunIntensity) + NightFloor(ray.z) * (1 - day);
}
// Cosine-weighted mean radiance of the upper hemisphere (6 heights by 8 bearings): the sky light the
// relit map takes. The last result is kept, since the inputs change only when the sky is edited.
inline Vec3 ClimateSkyIrradiance(const Climate &c) {
    static thread_local std::array<float, 15> lastKey{};
    static thread_local Vec3 lastValue{};
    static thread_local bool valid = false;
    const std::array<float, 15> key{c[CL_sunAzimuth], c[CL_sunElevation], c[CL_haze], c[CL_sunColorR],
                                     c[CL_sunColorG], c[CL_sunColorB], c[CL_moon], c[CL_moonAzimuth],
                                     c[CL_moonElevation], c[CL_moonPhase], c[CL_moonColorR], c[CL_moonColorG],
                                     c[CL_moonColorB], 0, 0};
    if (valid && key == lastKey) return lastValue;
    Vec3 sum{};
    constexpr int heights = 6, bearings = 8;
    for (int i = 0; i < heights; ++i) {
        const float z = std::sqrt((i + .5f) / heights), r = std::sqrt((std::max)(0.f, 1 - z * z));
        for (int j = 0; j < bearings; ++j) {
            const float a = (j + .5f) * 6.28318531f / bearings;
            sum = sum + ClimateSkyRadiance(c, {r * std::cos(a), r * std::sin(a), z});
        }
    }
    lastKey = key;
    lastValue = sum * (1.f / (heights * bearings));
    valid = true;
    return lastValue;
}

} // namespace Filmmaker::Environment
