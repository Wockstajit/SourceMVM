#include "../../AfxHookSource2/Deadlock/DeadlockCameraCommandMath.h"
#include <cstdio>
#include <limits>

int main() {
    using namespace Deadlock;
    int failures = 0, tick = -1;
    auto check = [&](bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };
    check(CameraTimeToTick(12.0, 10.0, 100, 1.0 / 64, tick) && tick == 228, "nonzero demo time origin");
    check(CameraTimeToTick(9.0, 10.0, 100, 1.0 / 64, tick) && tick == 36, "backward target");
    check(!CameraTimeToTick(0.0, 10.0, 100, 1.0 / 64, tick), "negative tick rejected");
    check(!CameraTimeToTick(10, 10, -1, 1.0 / 64, tick), "unloaded demo rejected");
    check(!CameraTimeToTick(10, 10, 0, 0, tick), "unknown clock rejected");
    check(!CameraTimeToTick(1e30, 10, 0, 0.01, tick), "tick overflow rejected");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check(ValidCameraFov(90) && !ValidCameraFov(0) && !ValidCameraFov(179) && !ValidCameraFov(nan), "projection bounds");
    check(CameraExportNext(10, nan) == CameraExportStep::Write, "first sample");
    check(CameraExportNext(10, 10) == CameraExportStep::Duplicate, "paused and duplicate view passes");
    check(CameraExportNext(10.02, 10) == CameraExportStep::Write, "forward playback");
    check(CameraExportNext(9.99, 10) == CameraExportStep::Duplicate, "resume interpolation jitter is skipped");
    check(CameraExportNext(9.99, 10, true) == CameraExportStep::Rewind, "one-tick rewind still stops export");
    check(CameraExportNext(10, 10, true) == CameraExportStep::Duplicate, "pause tick adjustment at unchanged time is not a seek");
    check(CameraExportNext(9, 10) == CameraExportStep::Rewind, "rewind cannot corrupt ordered export");
    check(CameraExportNext(nan, 10) == CameraExportStep::Invalid, "invalid clock cannot reach file");
    std::printf("Deadlock camera command math: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
