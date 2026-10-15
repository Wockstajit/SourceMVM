#pragma once
#include <cmath>
#include <limits>
#include "../Filmmaker/Deadlock/DeadlockDemoClockMath.h"

namespace Deadlock {
inline bool ValidCameraFov(double value) {
    return std::isfinite(value) && value > 1 && value < 179;
}

// Demo time can have a nonzero origin. Convert relative to a measured tick/time pair.
inline bool CameraTimeToTick(double time, double now, int tick, double interval, int& out) {
    if (tick < 0 || !std::isfinite(time) || !std::isfinite(now)
        || !std::isfinite(interval) || interval <= 0) return false;
    const double value = std::round(tick + (time - now) / interval);
    if (value < 0 || value > (std::numeric_limits<int>::max)()) return false;
    out = static_cast<int>(value);
    return true;
}

enum class CameraExportStep { Write, Duplicate, Rewind, Invalid };
inline CameraExportStep CameraExportNext(double time, double last, bool tickRewound = false) {
    if (!std::isfinite(time)) return CameraExportStep::Invalid;
    if (!std::isfinite(last)) return CameraExportStep::Write;
    // Resume can move interpolated time backward within a tick. Keep the written
    // high-water mark through that jitter; an actual tick rewind still stops IO.
    // Pausing can decrement the reported tick while time stays exactly at the
    // last written sample. That is a duplicate, not a backward camera sample.
    if ((tickRewound && time < last - 1e-6)
        || time < last - Filmmaker::DemoClockConfig().backwardsJitterTolerance)
        return CameraExportStep::Rewind;
    if (time <= last + 1e-6) return CameraExportStep::Duplicate;
    return CameraExportStep::Write;
}
}
