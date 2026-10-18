#include "../../AfxHookSource2/Filmmaker/Debug/GpuPassProfileMath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace Filmmaker::GpuProfile;

static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", what); std::exit(1); }
}
static bool Near(double a, double b) { return std::abs(a - b) < 1e-6; }

int main() {
    // Nested and overlapping passes count once; separate ones add; empty ones are ignored.
    Check(CoveredTicks({}) == 0, "Nothing timed covers nothing");
    Check(CoveredTicks({{0, 10, 20}, {1, 12, 18}}) == 10, "A nested pass is inside its parent");
    Check(CoveredTicks({{0, 10, 20}, {1, 15, 30}}) == 20, "Overlapping passes merge");
    Check(CoveredTicks({{0, 40, 50}, {1, 10, 20}}) == 20, "Separate passes add, in any order");
    Check(CoveredTicks({{0, 10, 20}, {1, 20, 25}}) == 15, "Touching passes merge without a gap");
    Check(CoveredTicks({{0, 10, 10}, {1, 30, 20}}) == 0, "Empty and reversed intervals cover nothing");

    // 1 MHz ticks: one tick is 0.001 ms.
    PassStats stats;
    for (unsigned frame = 0; frame < 100; ++frame) {
        std::vector<Interval> intervals{{0, 1000, 3000}};             // pass 0: 2 ms every frame
        if (frame % 4 == 0) intervals.push_back({1, 3000, 9000});      // pass 1: 6 ms every fourth frame
        intervals.push_back({2, 9000, 9500});                         // pass 2: twice a frame, 0.5 ms each
        intervals.push_back({2, 9600, 10100});
        stats.AddFrame(intervals, 0, 20000, 1000000, frame == 7 ? 3 : 0);
    }
    const auto frame = stats.Frame();
    Check(frame.frames == 100 && Near(frame.frameMeanMs, 20) && Near(frame.frameP95Ms, 20), "Frame time per frame");
    Check(Near(frame.coveredMeanMs, 2 + 1 + 1.5), "Timed GPU time is the union of the frame's passes");
    Check(frame.droppedScopes == 3, "Scopes that ran outside their frame are counted");
    const auto rows = stats.Rows();
    Check(rows.size() == 3 && rows[0].pass == 0 && rows[1].pass == 1 && rows[2].pass == 2, "Most expensive pass first");
    Check(Near(rows[0].meanMs, 2) && Near(rows[0].callsPerFrame, 1) && Near(rows[0].framesTimed, 1), "Every-frame pass");
    Check(Near(rows[2].meanMs, 1) && Near(rows[2].callsPerFrame, 2), "A pass timed twice a frame sums");
    Check(Near(rows[1].meanMs, 1.5) && Near(rows[1].maxMs, 6) && Near(rows[1].p95Ms, 6) && Near(rows[1].framesTimed, .25),
          "A budgeted pass shows its per-frame average and the frames it ran");

    // The window keeps the most recent frames only, and a pass that stopped fades to zero.
    for (unsigned frame = 0; frame < PassStats::Window; ++frame) stats.AddFrame({{0, 0, 1000}}, 0, 10000, 1000000);
    const auto later = stats.Rows();
    Check(stats.Frame().frames == PassStats::Window && Near(stats.Frame().frameMeanMs, 10), "Window of recent frames");
    Check(later.size() == 1 && later[0].pass == 0 && Near(later[0].meanMs, 1), "Passes outside the window are gone");

    // A pass first seen late gets zero for the frames before it; a bad frame is ignored.
    PassStats late;
    late.AddFrame({}, 0, 1000, 1000);
    late.AddFrame({{5, 0, 2}}, 0, 1000, 1000);
    late.AddFrame({{5, 0, 2}}, 10, 5, 1000);
    late.AddFrame({{5, 0, 2}}, 0, 1000, 0);
    Check(late.Frame().frames == 2 && late.Rows().size() == 1 && late.Rows()[0].pass == 5 && Near(late.Rows()[0].meanMs, 1),
          "Late passes and invalid frames");
    stats.Reset();
    Check(stats.Frame().frames == 0 && stats.Rows().empty(), "Reset clears the window");
    std::puts("GPU pass profile math: coverage, per-pass window statistics and ordering hold");
    return 0;
}
