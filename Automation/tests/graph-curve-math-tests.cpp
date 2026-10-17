// The Bezier segment math the camera GRAPH draws and the camera PATH flies
// (Filmmaker/Movie/GraphCurveMath.h). One function serves both, so these pin the properties the
// path relies on: straight segments stay straight, a split leaves the curve unchanged, auto
// tangents are smooth through a key, and angles take the short way round.
#include "../../AfxHookSource2/Filmmaker/Movie/GraphCurveMath.h"
#include "../../AfxHookSource2/Filmmaker/Movie/CamMarkers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Filmmaker::GraphCurve;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", what); }
}
bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

Handle H(double tx, double dv) { Handle h; h.active = true; h.tx = tx; h.dv = dv; return h; }

} // namespace

int main() {
    // No handles on either side: the graph draws a straight line, so the camera must fly one.
    {
        const Handle none;
        check(near(SegmentValue(100, 10, none, 200, 30, none, 150), 20.0), "an unhandled segment is linear");
        check(near(SegmentValue(100, 10, none, 200, 30, none, 125), 15.0), "linear at a quarter");
        check(near(SegmentValue(100, 10, none, 200, 30, none, 100), 10.0), "starts on key A");
        check(near(SegmentValue(100, 10, none, 200, 30, none, 200), 30.0), "ends on key B");
    }

    // Handles bend the segment but never move its ends.
    {
        const Handle outA = H(0.4, 25.0), inB = H(0.2, -5.0);
        check(near(SegmentValue(0, 0, outA, 90, 10, inB, 0), 0.0), "a handled segment starts on key A");
        check(near(SegmentValue(0, 0, outA, 90, 10, inB, 90), 10.0), "and ends on key B");
        const double mid = SegmentValue(0, 0, outA, 90, 10, inB, 45);
        check(mid > 5.0 + 1.0, "an upward out-handle lifts the middle of the segment");
        // Easing out of A: a flat (dv 0) out handle makes the start slower than linear.
        const double eased = SegmentValue(0, 0, H(0.5, 0.0), 90, 10, Handle(), 9);
        check(eased < 1.0, "a flat out handle eases the start");
    }

    // A split at any tick is the same curve: the camera keeps the graph's shape when a marker
    // lands on a tick where this channel has no key.
    {
        const Handle outA = H(0.45, 40.0), inB = H(0.3, -12.0);
        const double t0 = 1000, v0 = -20, t1 = 1240, v1 = 35;
        for (double at : { 1001.0, 1077.0, 1120.0, 1239.0 }) {
            const Split s = SplitAt(t0, v0, outA, t1, v1, inB, at);
            check(near(s.value, SegmentValue(t0, v0, outA, t1, v1, inB, at)), "the split point is on the curve");
            double worst = 0.0;
            for (double t = t0; t <= t1; t += 0.5) {
                const double whole = SegmentValue(t0, v0, outA, t1, v1, inB, t);
                const double halves = (t <= at)
                    ? SegmentValue(t0, v0, s.outA, at, s.value, s.inK, t)
                    : SegmentValue(at, s.value, s.outK, t1, v1, s.inB, t);
                worst = std::max(worst, std::abs(whole - halves));
            }
            check(worst < 1e-6, "the two halves trace the original curve");
        }
        const Handle none;
        const Split straight = SplitAt(0, 0, none, 10, 10, none, 4);
        check(near(straight.value, 4.0) && !straight.inK.active && !straight.outK.active,
            "a straight segment splits into two straight ones");
    }

    // Auto tangents (the graph's Smooth): one tangent line through an interior key, so the curve
    // has no corner there, and the slope is the Catmull-Rom slope of the neighbours.
    {
        const double ticks[3] = { 0, 30, 90 };
        const double values[3] = { 0, 30, 60 };
        Handle in, out;
        AutoTangent(ticks, values, 3, 1, in, out);
        const double slope = (60.0 - 0.0) / (90.0 - 0.0);
        check(in.active && out.active, "an interior key gets both handles");
        check(near(out.dv / (out.tx * 60.0), slope), "the out handle follows the neighbours' slope");
        check(near(-in.dv / (in.tx * 30.0), slope), "the in handle is on the same line");
        Handle in0, out0;
        AutoTangent(ticks, values, 3, 0, in0, out0);
        check(!in0.active && out0.active, "the first key has no incoming handle");
        Handle in2, out2;
        AutoTangent(ticks, values, 3, 2, in2, out2);
        check(in2.active && !out2.active, "the last key has no outgoing handle");
    }

    // Angles: a yaw from 170 to -170 is a 20 degree turn, not 340.
    {
        check(near(Unwrap(170.0, -170.0), 190.0), "yaw wraps forward across 180");
        check(near(Unwrap(-170.0, 170.0), -190.0), "and backward");
        check(near(Unwrap(10.0, 20.0), 20.0), "a small turn is untouched");
        check(IsAngleChannel(3) && IsAngleChannel(4) && IsAngleChannel(5), "pitch, yaw, roll are angles");
        check(!IsAngleChannel(0) && !IsAngleChannel(6), "position and FOV are not");
        // A huge value unwraps in one step (a while loop walking 360 froze the game on a saved yaw of 4.7e10).
        const double far = Unwrap(89.8082, 4.71641e10);
        check(std::isfinite(far) && std::fabs(far - 89.8082) <= 180.0 + 1e-6, "a huge yaw unwraps to within half a turn");
        check(std::fabs(Unwrap(0.0, 1e300)) <= 180.0 + 1e-6, "even an absurd one");
        check(!std::isfinite(Unwrap(10.0, std::nan(""))), "NaN passes through instead of hanging");
    }

    // The marker limits: the user's frozen path (marker 1 of the 2026-10-17 "ballz" demo) loads sane.
    {
        Filmmaker::CamMarker bad;
        bad.x = -490.679; bad.y = 1286.46; bad.z = 143.036; bad.pitch = 10.2872; bad.yaw = 4.71641e10; bad.fov = 90;
        bad.out[4].active = true; bad.out[4].tx = 0.333333; bad.out[4].dv = -1.57214e10;
        bad.out[0].active = true; bad.out[0].tx = 0.333333; bad.out[0].dv = 10.1437;
        check(Filmmaker::SanitizeMarker(bad), "an out-of-range marker is reported as repaired");
        check(std::fabs(bad.yaw) <= 180.0, "its yaw is folded back within one turn");
        check(bad.out[4].dv == 0.0, "its runaway yaw handle is dropped");
        check(bad.out[0].dv == 10.1437 && bad.pitch == 10.2872 && bad.x == -490.679, "everything sane is kept");
        check(!Filmmaker::SanitizeMarker(bad), "and a sane marker is left alone");
        Filmmaker::CamMarker spin; spin.yaw = 720.0; spin.roll = -1440.0;
        check(!Filmmaker::SanitizeMarker(spin) && spin.yaw == 720.0 && spin.roll == -1440.0, "a path may still spin several turns");
        Filmmaker::CamMarker junk; junk.pitch = std::nan(""); junk.fov = 0; junk.x = 1e300; junk.in[3].tx = 7;
        Filmmaker::SanitizeMarker(junk);
        check(junk.pitch == 0.0 && junk.fov == 1.0 && junk.x == Filmmaker::kMarkerMaxCoord && junk.in[3].tx == 1.0, "NaN, zero FOV, far coordinates and bad handle spans are clamped");
    }

    if (g_failures == 0) std::printf("graph-curve-math-tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
