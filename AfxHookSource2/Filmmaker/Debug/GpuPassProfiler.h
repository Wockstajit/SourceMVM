#pragma once
// GPU time of the hook's own render passes, from D3D11 timestamp queries, beside the GPU frame
// they run in. Off by default; `mirv_filmmaker gpuprofile on` starts it and `mirv_filmmaker
// gpuprofile` prints the passes, most expensive first (also in `health` while on).
//
// A pass is timed with GPU_PASS_SCOPE(ctx, "name") around the calls that record it, on the
// immediate context or on a deferred context whose command list executes before the frame's
// present. Work the hook adds inside native CS2 draws (derived material programs, the lamp shadow
// kernel) is not a separate pass: it shows in the frame minus the timed passes, and its cost is
// measured by toggling it (Automation/verify/profile-gpu-costs.py).
#include <cstdint>

struct ID3D11DeviceContext;

namespace Filmmaker::GpuProfile {

bool Enabled();
void SetEnabled(bool enabled);
void Reset();
// A stable id for a pass name (a string literal); ids are shared by every scope with that name.
unsigned PassId(const char* name);

class Scope {
public:
    Scope(ID3D11DeviceContext* ctx, unsigned pass);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    ID3D11DeviceContext* m_ctx = nullptr;
    void* m_end = nullptr;
};

// A cost measured by sampling whole single frames elsewhere: the lit native draws with the hook's
// patched programs bound (EnvironmentNativeMaterial's MaterialTiming, one frame every second
// while profiling). Kept beside the passes as the mean of its samples.
void NoteSampled(const char* name, double ms, unsigned passes);
void NoteSampledProgram(uint64_t program, double ms, unsigned passes);

// Frame boundary, on the render thread with the immediate context, once per present.
void OnPresent(ID3D11DeviceContext* immediate);
// The pass table to the console.
void Print(unsigned maxRows = 40);

} // namespace Filmmaker::GpuProfile

#define GPU_PASS_SCOPE_JOIN2(a, b) a##b
#define GPU_PASS_SCOPE_JOIN(a, b) GPU_PASS_SCOPE_JOIN2(a, b)
#define GPU_PASS_SCOPE(ctx, name)                                                                         \
    static const unsigned GPU_PASS_SCOPE_JOIN(gpuPassId_, __LINE__) = ::Filmmaker::GpuProfile::PassId(name); \
    ::Filmmaker::GpuProfile::Scope GPU_PASS_SCOPE_JOIN(gpuPassScope_, __LINE__)((ctx), GPU_PASS_SCOPE_JOIN(gpuPassId_, __LINE__))
