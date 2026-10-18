#pragma once
// Engine-free bookkeeping for the GPU pass profiler (GpuPassProfiler.h): per-frame timestamp
// intervals in, per-pass window statistics out. Tested by Automation/tests/gpu-pass-profile-math-tests.cpp.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace Filmmaker::GpuProfile {

// One timed pass in one frame, in GPU timestamp ticks.
struct Interval {
    unsigned pass = 0;
    uint64_t begin = 0, end = 0;
};

// GPU time covered by at least one interval: nested or overlapping passes count once.
inline uint64_t CoveredTicks(std::vector<Interval> intervals) {
    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.begin < b.begin; });
    uint64_t covered = 0, open = 0, close = 0;
    bool any = false;
    for (const auto& i : intervals) {
        if (i.end <= i.begin) continue;
        if (!any || i.begin > close) {
            if (any) covered += close - open;
            open = i.begin; close = i.end; any = true;
        } else close = (std::max)(close, i.end);
    }
    if (any) covered += close - open;
    return covered;
}

// Rolling statistics over the last `Window` resolved frames. A pass that was not timed in a
// frame counts as 0 ms there, so a budgeted pass that runs every fourth frame shows its average
// per-frame cost, and its p95 shows the frames it ran.
struct PassRow {
    unsigned pass = 0;
    double meanMs = 0, p95Ms = 0, maxMs = 0, callsPerFrame = 0, framesTimed = 0;
};
struct FrameRow {
    unsigned frames = 0;
    double frameMeanMs = 0, frameP95Ms = 0, coveredMeanMs = 0;
    uint64_t droppedScopes = 0;
};

class PassStats {
public:
    static constexpr unsigned Window = 240;

    void Reset() { m_passes.clear(); m_frame.clear(); m_covered.clear(); m_frames = 0; m_next = 0; m_dropped = 0; }

    // One resolved frame: its intervals, the frame's own begin/end timestamps, the tick frequency.
    void AddFrame(const std::vector<Interval>& intervals, uint64_t frameBegin, uint64_t frameEnd, uint64_t frequency,
                  uint64_t droppedScopes = 0) {
        if (!frequency || frameEnd < frameBegin) return;
        const double ms = 1000.0 / double(frequency);
        const unsigned slot = m_next % Window;
        unsigned passes = 0;
        for (const auto& i : intervals) passes = (std::max)(passes, i.pass + 1);
        if (m_passes.size() < passes) m_passes.resize(passes);
        for (auto& p : m_passes) { p.ms.resize(Window, 0); p.calls.resize(Window, 0); p.ms[slot] = 0; p.calls[slot] = 0; }
        for (const auto& i : intervals) {
            if (i.end < i.begin) continue;
            auto& p = m_passes[i.pass];
            p.ms[slot] += float(double(i.end - i.begin) * ms);
            ++p.calls[slot];
        }
        m_frame.resize(Window, 0); m_covered.resize(Window, 0);
        m_frame[slot] = float(double(frameEnd - frameBegin) * ms);
        m_covered[slot] = float(double(CoveredTicks(intervals)) * ms);
        ++m_next; m_frames = (std::min)(m_frames + 1, Window);
        m_dropped += droppedScopes;
    }

    FrameRow Frame() const {
        FrameRow r;
        r.frames = m_frames; r.droppedScopes = m_dropped;
        if (!m_frames) return r;
        r.frameMeanMs = Mean(m_frame); r.frameP95Ms = P95(m_frame); r.coveredMeanMs = Mean(m_covered);
        return r;
    }

    // Every pass seen in the window, most expensive (mean per frame) first.
    std::vector<PassRow> Rows() const {
        std::vector<PassRow> rows;
        if (!m_frames) return rows;
        for (unsigned pass = 0; pass < m_passes.size(); ++pass) {
            const auto& p = m_passes[pass];
            if (p.ms.empty()) continue;
            PassRow r; r.pass = pass;
            unsigned calls = 0, timed = 0;
            for (unsigned f = 0; f < m_frames; ++f) { calls += p.calls[f]; timed += p.calls[f] ? 1 : 0; }
            if (!calls) continue;
            r.meanMs = Mean(p.ms); r.p95Ms = P95(p.ms);
            r.maxMs = *std::max_element(p.ms.begin(), p.ms.begin() + m_frames);
            r.callsPerFrame = double(calls) / m_frames; r.framesTimed = double(timed) / m_frames;
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end(), [](const PassRow& a, const PassRow& b) { return a.meanMs > b.meanMs; });
        return rows;
    }

private:
    struct Pass { std::vector<float> ms; std::vector<unsigned> calls; };
    double Mean(const std::vector<float>& v) const {
        double sum = 0; for (unsigned f = 0; f < m_frames; ++f) sum += v[f]; return sum / m_frames;
    }
    double P95(const std::vector<float>& v) const {
        std::vector<float> s(v.begin(), v.begin() + m_frames);
        const size_t k = (std::min)(s.size() - 1, size_t(double(s.size() - 1) * .95 + .5));
        std::nth_element(s.begin(), s.begin() + k, s.end());
        return s[k];
    }
    std::vector<Pass> m_passes;
    std::vector<float> m_frame, m_covered;
    unsigned m_frames = 0, m_next = 0;
    uint64_t m_dropped = 0;
};

} // namespace Filmmaker::GpuProfile
