#include "GpuPassProfiler.h"
#include "GpuPassProfileMath.h"
#include "MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Filmmaker::GpuProfile {
namespace {
using Microsoft::WRL::ComPtr;
// Scopes beyond this in one frame go untimed; frames not resolved after this many presents
// (a lost device, a query the driver never answers) are dropped.
constexpr unsigned MaxScopesPerFrame = 1024, MaxPendingFrames = 8, MaxPooledQueries = 4096;

struct Timed { unsigned pass = 0; ComPtr<ID3D11Query> begin, end; };
struct FrameQueries {
    ComPtr<ID3D11Query> disjoint, begin, end;
    std::vector<Timed> scopes;
    unsigned age = 0;
};

std::atomic<bool> g_enabled{false};
std::mutex g_mutex; // everything below
std::vector<std::string> g_names;
ComPtr<ID3D11Device> g_device;
std::vector<ComPtr<ID3D11Query>> g_timestamps, g_disjoints;
std::unique_ptr<FrameQueries> g_open;
std::deque<std::unique_ptr<FrameQueries>> g_pending;
PassStats g_stats;
bool g_reset = false;
ULONGLONG g_lastLog = 0;
struct Sampled { double sum = 0, last = 0; unsigned samples = 0, passes = 0; };
std::map<std::string, Sampled> g_sampled;
std::map<uint64_t, Sampled> g_programs;
void ResetStats() { g_stats.Reset(); g_sampled.clear(); g_programs.clear(); }

ComPtr<ID3D11Query> Take(std::vector<ComPtr<ID3D11Query>>& pool, D3D11_QUERY type) {
    ComPtr<ID3D11Query> q;
    if (!pool.empty()) { q = std::move(pool.back()); pool.pop_back(); return q; }
    if (!g_device) return q;
    const D3D11_QUERY_DESC desc{type, 0};
    if (FAILED(g_device->CreateQuery(&desc, &q))) q.Reset();
    return q;
}
void Give(std::vector<ComPtr<ID3D11Query>>& pool, ComPtr<ID3D11Query>& q) {
    if (q && pool.size() < MaxPooledQueries) pool.push_back(std::move(q));
    q.Reset();
}
bool Read(ID3D11DeviceContext* ctx, ID3D11Query* q, void* data, UINT size) {
    return q && ctx->GetData(q, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
}
void ReleaseAll() {
    g_open.reset(); g_pending.clear(); g_timestamps.clear(); g_disjoints.clear(); g_device.Reset();
}
const char* Name(unsigned pass) { return pass < g_names.size() ? g_names[pass].c_str() : "?"; }

void Resolve(ID3D11DeviceContext* immediate) {
    while (!g_pending.empty()) {
        auto& f = *g_pending.front();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 begin = 0, end = 0;
        if (!Read(immediate, f.disjoint.Get(), &disjoint, sizeof(disjoint)) ||
            !Read(immediate, f.begin.Get(), &begin, sizeof(begin)) || !Read(immediate, f.end.Get(), &end, sizeof(end))) {
            if (++f.age <= MaxPendingFrames) return;
            g_pending.pop_front(); // never answered: drop the frame and its queries
            continue;
        }
        std::vector<Interval> intervals;
        intervals.reserve(f.scopes.size());
        uint64_t dropped = 0;
        for (auto& s : f.scopes) {
            UINT64 a = 0, b = 0;
            if (Read(immediate, s.begin.Get(), &a, sizeof(a)) && Read(immediate, s.end.Get(), &b, sizeof(b)) && b >= a) {
                intervals.push_back({s.pass, a, b});
                Give(g_timestamps, s.begin); Give(g_timestamps, s.end);
            } else {
                // Recorded in a command list that has not run (or never will): never reuse these.
                ++dropped; s.begin.Reset(); s.end.Reset();
            }
        }
        if (!disjoint.Disjoint) g_stats.AddFrame(intervals, begin, end, disjoint.Frequency, dropped);
        Give(g_disjoints, f.disjoint); Give(g_timestamps, f.begin); Give(g_timestamps, f.end);
        g_pending.pop_front();
    }
}

void LogTop() {
    const auto now = GetTickCount64();
    if (!MvmDebugLog_Active() || now - g_lastLog < 5000) return;
    g_lastLog = now;
    const auto frame = g_stats.Frame();
    if (!frame.frames) return;
    std::string line;
    char buffer[160];
    unsigned shown = 0;
    for (const auto& r : g_stats.Rows()) {
        if (shown++ == 12) break;
        snprintf(buffer, sizeof(buffer), " %s=%.3f", Name(r.pass), r.meanMs);
        line += buffer;
    }
    MvmDebugLog_LinefAlways("gpu.profile", "frames=%u frameMs=%.3f p95=%.3f timedMs=%.3f dropped=%llu%s", frame.frames,
                            frame.frameMeanMs, frame.frameP95Ms, frame.coveredMeanMs,
                            (unsigned long long)frame.droppedScopes, line.c_str());
}
} // namespace

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
void SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (enabled && !g_enabled.load(std::memory_order_relaxed)) g_reset = true;
    g_enabled.store(enabled, std::memory_order_relaxed);
}
void Reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_reset = true;
}
unsigned PassId(const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned i = 0; i < g_names.size(); ++i)
        if (g_names[i] == name) return i;
    g_names.emplace_back(name);
    return unsigned(g_names.size() - 1);
}

Scope::Scope(ID3D11DeviceContext* ctx, unsigned pass) {
    if (!ctx || !g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_open || g_open->scopes.size() >= MaxScopesPerFrame) return;
    Timed t;
    t.pass = pass;
    t.begin = Take(g_timestamps, D3D11_QUERY_TIMESTAMP);
    t.end = Take(g_timestamps, D3D11_QUERY_TIMESTAMP);
    if (!t.begin || !t.end) { Give(g_timestamps, t.begin); Give(g_timestamps, t.end); return; }
    ctx->End(t.begin.Get());
    m_ctx = ctx;
    auto* end = t.end.Get();
    end->AddRef(); // the frame may resolve and drop its reference before this scope closes
    m_end = end;
    g_open->scopes.push_back(std::move(t));
}
Scope::~Scope() {
    if (!m_ctx) return;
    auto* end = static_cast<ID3D11Query*>(m_end);
    m_ctx->End(end);
    end->Release();
}

void NoteSampled(const char* name, double ms, unsigned passes) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& s = g_sampled[name];
    s.sum += ms; s.last = ms; ++s.samples; s.passes = passes;
}
void NoteSampledProgram(uint64_t program, double ms, unsigned passes) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_programs.size() >= 4096 && !g_programs.count(program)) return;
    auto& s = g_programs[program];
    s.sum += ms; s.last = ms; ++s.samples; s.passes = passes;
}

void OnPresent(ID3D11DeviceContext* immediate) {
    if (!immediate) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_device) { ReleaseAll(); ResetStats(); }
        return;
    }
    if (!g_device) immediate->GetDevice(&g_device);
    if (g_reset) { g_reset = false; ResetStats(); }
    if (g_open) {
        immediate->End(g_open->end.Get());
        immediate->End(g_open->disjoint.Get());
        g_pending.push_back(std::move(g_open));
    }
    Resolve(immediate);
    LogTop();
    if (g_pending.size() >= MaxPendingFrames) return; // the GPU is that far behind: skip a frame
    auto f = std::make_unique<FrameQueries>();
    f->disjoint = Take(g_disjoints, D3D11_QUERY_TIMESTAMP_DISJOINT);
    f->begin = Take(g_timestamps, D3D11_QUERY_TIMESTAMP);
    f->end = Take(g_timestamps, D3D11_QUERY_TIMESTAMP);
    if (!f->disjoint || !f->begin || !f->end) return;
    immediate->Begin(f->disjoint.Get());
    immediate->End(f->begin.Get());
    g_open = std::move(f);
}

void Print(unsigned maxRows) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled.load(std::memory_order_relaxed)) {
        advancedfx::Message("  gpu profile: off (mirv_filmmaker gpuprofile on)\n");
        return;
    }
    const auto frame = g_stats.Frame();
    if (!frame.frames) {
        advancedfx::Message("  gpu profile: on, no resolved frame yet\n");
        return;
    }
    advancedfx::Message("  gpu profile over %u frames: GPU frame %.2f ms (p95 %.2f), timed hook passes %.2f ms, "
                        "rest %.2f ms (native CS2 and hook work inside native draws)%s\n",
                        frame.frames, frame.frameMeanMs, frame.frameP95Ms, frame.coveredMeanMs,
                        frame.frameMeanMs - frame.coveredMeanMs,
                        frame.droppedScopes ? " -- some scopes ran outside their frame" : "");
    advancedfx::Message("    %-34s %8s %8s %8s %6s %7s\n", "pass", "mean ms", "p95 ms", "max ms", "%frame", "calls");
    unsigned shown = 0;
    for (const auto& r : g_stats.Rows()) {
        if (shown++ == maxRows) break;
        advancedfx::Message("    %-34s %8.3f %8.3f %8.3f %5.1f%% %7.2f\n", Name(r.pass), r.meanMs, r.p95Ms, r.maxMs,
                            frame.frameMeanMs > 0 ? 100 * r.meanMs / frame.frameMeanMs : 0., r.callsPerFrame);
    }
    if (!g_sampled.empty()) {
        advancedfx::Message("  sampled whole frames (inside native draws, so part of the rest above):\n");
        advancedfx::Message("    %-34s %8s %8s %6s %7s %7s\n", "draws", "mean ms", "last ms", "%frame", "draws", "samples");
        std::vector<std::pair<std::string, Sampled>> rows(g_sampled.begin(), g_sampled.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.sum / a.second.samples > b.second.sum / b.second.samples; });
        for (const auto& [name, s] : rows) {
            const double mean = s.sum / s.samples;
            advancedfx::Message("    %-34s %8.3f %8.3f %5.1f%% %7u %7u\n", name.c_str(), mean, s.last,
                                frame.frameMeanMs > 0 ? 100 * mean / frame.frameMeanMs : 0., s.passes, s.samples);
        }
    }
    if (!g_programs.empty()) {
        std::vector<std::pair<uint64_t, Sampled>> rows(g_programs.begin(), g_programs.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.sum / a.second.samples > b.second.sum / b.second.samples; });
        advancedfx::Message("  costliest lit programs (sampled ms / draws):");
        for (size_t i = 0; i < rows.size() && i < 8; ++i)
            advancedfx::Message(" %016llx=%.2f/%u", (unsigned long long)rows[i].first,
                                rows[i].second.sum / rows[i].second.samples, rows[i].second.passes);
        advancedfx::Message("\n");
    }
}

} // namespace Filmmaker::GpuProfile
