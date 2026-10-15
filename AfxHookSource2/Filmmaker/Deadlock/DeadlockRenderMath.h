#pragma once
#include <cmath>
#include <cstdint>
namespace Filmmaker::DeadlockRenderMath {
// Inclusive Out. Count once from the duration; accumulating fractional tick steps can
// overshoot an exact boundary (e.g. 16 ticks at 120 fps) and omit the final sample.
inline uint64_t FrameCount(int first, int last, double interval, int fps) {
    if (last < first || !(interval > 0) || !std::isfinite(interval) || fps < 1) return 0;
    return static_cast<uint64_t>(std::floor((static_cast<double>(last) - first) * interval * fps + 1e-7)) + 1;
}
}
