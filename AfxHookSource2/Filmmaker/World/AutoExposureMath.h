#pragma once
// Eye adaptation, engine-free. The same model as Unreal Engine's auto exposure
// (Engine/Shaders/Private/PostProcessHistogramCommon.ush, ComputeEyeAdaptation): exposure moves
// toward its target at a constant number of stops per second while far from it, and eases in
// exponentially over the last ExponentialTransitionDistance stops so it never overshoots or snaps.
#include <algorithm>
#include <cmath>

namespace Filmmaker::AutoExposureMath {

// Unreal's r.EyeAdaptation.ExponentialTransitionDistance default, in stops.
inline constexpr float TransitionStops = 1.5f;
// Unreal's reference frame time for matching the exponential's slope to the linear segment.
inline constexpr float ReferenceFrameTime = 1.f / 60.f;

// One adaptation step of log2 exposure. speed is in stops per second (> 0); dt in seconds.
inline float Adapt(float currentLog2, float targetLog2, float dt, float speed) {
    if (!(dt > 0) || !(speed > 0) || !std::isfinite(currentLog2)) return targetLog2;
    const float diff = targetLog2 - currentLog2;
    const float distance = std::fabs(diff);
    if (distance > TransitionStops) {
        const float step = dt * speed;
        return diff > 0 ? (std::min)(targetLog2, currentLog2 + step) : (std::max)(targetLog2, currentLog2 - step);
    }
    // M scales the exponential so its rate at the transition distance equals the linear rate.
    const float startTime = TransitionStops / speed;
    const float m = ReferenceFrameTime / ((1.f - std::exp2(-ReferenceFrameTime * speed)) * startTime);
    const float factor = (std::min)(1.f, (1.f - std::exp2(-dt * speed)) * m);
    return currentLog2 + diff * factor;
}

// The exposure (tonemap scale) that maps the metered average luminance to the key, raised or
// lowered by compensation stops, clamped to the scale range the tonemapper accepts.
inline float TargetScale(float meteredLog2, float key, float compensationStops, float minScale, float maxScale) {
    const float scale = key * std::exp2(compensationStops - meteredLog2);
    if (!std::isfinite(scale)) return 1.f;
    return std::clamp(scale, minScale, maxScale);
}

// PARTIAL ADAPTATION: how bright the meter is taken to be, part of the way from a reference level
// to what it read (both log2 luminance). 1 is full adaptation: every scene is brought to the same
// mid-grey, so a dim room is lifted to daylight brightness and the sunlit street seen through its
// doorway is lifted with it and clips to white, while snow is pulled down to grey. Below 1 a dim
// scene stays dimmer and a bright one brighter, as they look to the eye, and the exposure moves
// over a smaller range, so what is bright in a dark frame is not blown out. (The idea of the
// "automatic key" of Krawczyk, Myszkowski and Seidel 2005, "Perceptual Effects in Real-Time Tone
// Mapping": the key a scene is mapped to falls with its luminance instead of staying fixed.)
inline float PartialMeter(float meteredLog2, float referenceLog2, float strength) {
    if (!std::isfinite(meteredLog2) || !std::isfinite(referenceLog2)) return meteredLog2;
    return referenceLog2 + (meteredLog2 - referenceLog2) * std::clamp(strength, 0.f, 1.f);
}

} // namespace Filmmaker::AutoExposureMath
