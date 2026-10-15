#pragma once
#include "EnvironmentMath.h"
namespace Filmmaker::Environment {
struct StudioLight {
    float width = 64, height = 64, sourceRadius = 0;
    int pattern = 0, profile = 0, cue = 0;
    float patternScale = 4, rate = 1, amount = 1, phase = 0;
};
// Four fixed quadrature samples; never move with the camera or random frame state.
inline Vec3 AreaLightSample(Vec3 position, Vec3 right, Vec3 up, float width, float height, int sample) {
    constexpr float quadrature = .28867513459f;
    return position + right * ((sample & 1 ? 1.f : -1.f) * width * quadrature) +
           up * ((sample & 2 ? 1.f : -1.f) * height * quadrature);
}
inline float StudioCue(const StudioLight &s, double time) {
    if (!s.cue)
        return 1;
    double cycles = time * s.rate + s.phase;
    double phase = cycles - std::floor(cycles);
    float value = 1;
    if (s.cue == 1)
        value = float(.5 + .5 * std::sin(phase * 6.283185307179586));
    if (s.cue == 2)
        value = float(.55 + .25 * std::sin(cycles * 19.1) + .2 * std::sin(cycles * 47.3));
    if (s.cue == 3)
        value = phase < .07 || (phase > .16 && phase < .21) ? 1.f : 0.f;
    if (s.cue == 4)
        value = float(std::pow((std::max)(0., std::sin(phase * 6.283185307179586)), 8));
    return 1 - s.amount + s.amount * std::clamp(value, 0.f, 1.f);
}
inline float ShadowPenumbra(float sourceRadius, float blockerDistance, float receiverDistance) {
    return sourceRadius * (std::max)(0.f, receiverDistance - blockerDistance) /
           (std::max)(blockerDistance, 1.f);
}
} // namespace Filmmaker::Environment
