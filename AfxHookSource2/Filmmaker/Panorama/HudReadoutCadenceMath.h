#pragma once
#include <cstdint>

namespace Filmmaker {
// Passive readouts may wait; visibility and wheel feedback must publish immediately.
class HudReadoutCadence {
public:
    bool Ready(uint64_t nowMs, unsigned rateHz, bool urgent) {
        if (m_started && !urgent && rateHz && nowMs >= m_lastMs
            && nowMs - m_lastMs < 1000u / rateHz) return false;
        m_started = true;
        m_lastMs = nowMs;
        return true;
    }
private:
    bool m_started = false;
    uint64_t m_lastMs = 0;
};
} // namespace Filmmaker
