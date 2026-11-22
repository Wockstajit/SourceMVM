#include "../../AfxHookSource2/Filmmaker/World/AutoExposureMath.h"

#include <cmath>
#include <cstdio>

using namespace Filmmaker::AutoExposureMath;

static int failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

int main() {
    // Far from the target the exposure moves linearly at `speed` stops per second.
    Check(Near(Adapt(0.f, 5.f, .1f, 2.f), .2f, 1e-5f), "linear step brightening");
    Check(Near(Adapt(0.f, -5.f, .1f, 2.f), -.2f, 1e-5f), "linear step darkening");

    // Within the transition distance it eases in: it moves toward the target, never past it.
    {
        float e = 0.f;
        for (int i = 0; i < 600; ++i) {
            const float next = Adapt(e, 1.f, 1.f / 60, 2.f);
            Check(next >= e - 1e-6f && next <= 1.f + 1e-6f, "monotone and no overshoot");
            e = next;
        }
        Check(Near(e, 1.f, 1e-3f), "converges to the target");
    }

    // The two segments meet: the rate just inside the transition matches the linear rate.
    {
        const float dt = 1.f / 60, speed = 3.f;
        const float inside = Adapt(0.f, TransitionStops - 1e-3f, dt, speed);
        const float outside = Adapt(0.f, TransitionStops + 1e-3f, dt, speed);
        Check(Near(inside, outside, .01f), "linear and exponential rates match at the seam");
    }

    // A huge frame (a hitch or a seek) cannot overshoot either.
    Check(Adapt(0.f, 1.f, 10.f, 3.f) <= 1.f + 1e-6f, "long frame stops at the target");
    Check(Adapt(0.f, 3.f, 10.f, 3.f) <= 3.f + 1e-6f, "long linear frame stops at the target");

    // Invalid inputs snap to the target instead of producing NaN.
    Check(Adapt(NAN, 2.f, .1f, 1.f) == 2.f, "NaN current snaps");
    Check(Adapt(0.f, 2.f, 0.f, 1.f) == 2.f, "zero dt snaps");

    // A scene twice as bright gets half the exposure; compensation adds stops.
    {
        const float a = TargetScale(0.f, .5f, 0.f, .01f, 100.f);
        const float b = TargetScale(1.f, .5f, 0.f, .01f, 100.f);
        Check(Near(b, a * .5f, 1e-5f), "brighter scene halves exposure");
        Check(Near(TargetScale(0.f, .5f, 1.f, .01f, 100.f), 1.f, 1e-5f), "one stop compensation doubles");
        Check(Near(TargetScale(-20.f, .5f, 0.f, .01f, 4.f), 4.f, 1e-6f), "dark scene clamps to max");
        Check(Near(TargetScale(20.f, .5f, 0.f, .05f, 4.f), .05f, 1e-6f), "bright scene clamps to min");
    }

    // Partial adaptation: a dim room is lifted only part of the way to daylight brightness, so what
    // is sunlit in the same frame is not blown out, and a bright scene (snow) is not pulled to grey.
    {
        const float daylight = -1.5f, room = -4.1f, snow = 0.f;
        Check(Near(PartialMeter(daylight, daylight, .5f), daylight, 1e-6f), "the reference level is exposed as metered");
        Check(Near(PartialMeter(room, daylight, 1.f), room, 1e-6f), "full strength is full adaptation");
        Check(Near(PartialMeter(room, daylight, 0.f), daylight, 1e-6f), "no strength is a fixed exposure");
        const float full = TargetScale(room, .18f, 0.f, .05f, 4.f);
        const float half = TargetScale(PartialMeter(room, daylight, .5f), .18f, 0.f, .05f, 4.f);
        const float outdoors = TargetScale(daylight, .18f, 0.f, .05f, 4.f);
        Check(half > outdoors && half < full, "a dim room is brighter than outdoors' exposure and dimmer than full adaptation");
        Check(Near(std::log2(half / outdoors), .5f * std::log2(full / outdoors), 1e-4f), "half the stops of full adaptation");
        Check(TargetScale(PartialMeter(snow, daylight, .5f), .18f, 0.f, .05f, 4.f) > TargetScale(snow, .18f, 0.f, .05f, 4.f),
            "a bright scene keeps more exposure than full adaptation gives it");
        Check(!std::isfinite(PartialMeter(NAN, daylight, .5f)), "an unreadable meter stays unreadable");
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("auto exposure math: all tests passed\n");
    return 0;
}
