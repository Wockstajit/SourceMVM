#include "RenderPixelStateLayer.h"
#include "RenderPixelStateLayerDetail.h"

#include <atomic>
#include <cstring>

namespace Filmmaker::PixelStateLayer {
namespace {

std::atomic<bool> g_enabled{true};
Detail::Originals g_original; // set by Attach; the layer never holds anything without it

struct Held {
    ID3D11DeviceContext* context = nullptr; // non-null while a substitution is held
    bool shader = false;
    ID3D11PixelShader* engineShader = nullptr;
    bool view[ViewSlots]{};
    ID3D11ShaderResourceView* engineView[ViewSlots]{};
    ID3D11ShaderResourceView* oursView[ViewSlots]{};
    unsigned viewLow = ViewSlots, viewHigh = 0; // substituted slots lie in [viewLow, viewHigh)
    bool sampler[SamplerSlots]{};
    ID3D11SamplerState* engineSampler[SamplerSlots]{};
    ID3D11SamplerState* oursSampler[SamplerSlots]{};
    bool buffer[BufferSlots]{};
    ID3D11Buffer* engineBuffer[BufferSlots]{};
    ID3D11Buffer* oursBuffer[BufferSlots]{};
    bool dependency[BufferSlots]{};
    ID3D11Buffer* dependencyBuffer[BufferSlots]{};
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT blendMask = 0;
    D3D11_VIEWPORT viewport{};
} g_held;
Stats g_stats;
unsigned g_suspended = 0; // reused lit draws in progress (nested draws are blocked by the caller)

template <class T> void Drop(T*& pointer) { if (pointer) { pointer->Release(); pointer = nullptr; } }
template <class T> void Keep(T*& slot, T* value) { if (value) value->AddRef(); if (slot) slot->Release(); slot = value; }

} // namespace

namespace Detail {
void SetOriginals(const Originals& originals) { g_original = originals; }
const Originals& GetOriginals() { return g_original; }

bool Holds(ID3D11DeviceContext* context) { return g_held.context && g_held.context == context && !g_suspended; }

void GetShader(ID3D11PixelShader** shader, ID3D11ClassInstance** instances, UINT* instanceCount) {
    if (shader) { *shader = g_held.engineShader; if (*shader) (*shader)->AddRef(); }
    if (instanceCount) *instanceCount = 0;
    (void)instances;
}
bool ShaderHeld() { return g_held.shader; }

void FixViews(UINT start, UINT count, ID3D11ShaderResourceView** views) {
    if (!views || start >= g_held.viewHigh || start + count <= g_held.viewLow) return;
    for (UINT i = 0; i < count && start + i < ViewSlots; ++i) if (g_held.view[start + i]) {
        if (views[i]) views[i]->Release();
        views[i] = g_held.engineView[start + i];
        if (views[i]) views[i]->AddRef();
    }
}
void FixSamplers(UINT start, UINT count, ID3D11SamplerState** samplers) {
    if (!samplers) return;
    for (UINT i = 0; i < count && start + i < SamplerSlots; ++i) if (g_held.sampler[start + i]) {
        if (samplers[i]) samplers[i]->Release();
        samplers[i] = g_held.engineSampler[start + i];
        if (samplers[i]) samplers[i]->AddRef();
    }
}
void FixBuffers(UINT start, UINT count, ID3D11Buffer** buffers) {
    if (!buffers) return;
    for (UINT i = 0; i < count && start + i < BufferSlots; ++i) if (g_held.buffer[start + i]) {
        if (buffers[i]) buffers[i]->Release();
        buffers[i] = g_held.engineBuffer[start + i];
        if (buffers[i]) buffers[i]->AddRef();
    }
}

// A Set that reaches a substituted slot keeps ours bound and records the engine's value.
// Returns the array to forward: the caller's, or `scratch` with our bindings put back.
// A null array with a count is D3D11's way of clearing that range: `count` null bindings. Forwarded
// as it is, it would unbind ours while the layer still counted them as held.
ID3D11ShaderResourceView* const* SetViews(UINT start, UINT count, ID3D11ShaderResourceView* const* views,
                                          ID3D11ShaderResourceView** scratch) {
    if (start >= g_held.viewHigh || start + count <= g_held.viewLow) return views;
    bool touched = false;
    for (UINT i = 0; i < count && start + i < ViewSlots; ++i) if (g_held.view[start + i]) { touched = true; break; }
    if (!touched) return views;
    for (UINT i = 0; i < count; ++i) {
        const UINT slot = start + i;
        ID3D11ShaderResourceView* engine = views ? views[i] : nullptr;
        if (slot < ViewSlots && g_held.view[slot]) { Keep(g_held.engineView[slot], engine); scratch[i] = g_held.oursView[slot]; }
        else scratch[i] = engine;
    }
    return scratch;
}
ID3D11SamplerState* const* SetSamplers(UINT start, UINT count, ID3D11SamplerState* const* samplers,
                                       ID3D11SamplerState** scratch) {
    bool touched = false;
    for (UINT i = 0; i < count && start + i < SamplerSlots; ++i) if (g_held.sampler[start + i]) { touched = true; break; }
    if (!touched) return samplers;
    for (UINT i = 0; i < count; ++i) {
        const UINT slot = start + i;
        ID3D11SamplerState* engine = samplers ? samplers[i] : nullptr;
        if (slot < SamplerSlots && g_held.sampler[slot]) { Keep(g_held.engineSampler[slot], engine); scratch[i] = g_held.oursSampler[slot]; }
        else scratch[i] = engine;
    }
    return scratch;
}
// Constant buffers: a different buffer in a slot the lighting decision read ends the substitution
// (sets `flush`: flush, then forward the caller's array). A null array clears the range, as for
// views, so a lighting slot inside it changes too.
ID3D11Buffer* const* SetBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers, ID3D11Buffer** scratch, bool& flush) {
    flush = false;
    bool touched = false;
    for (UINT i = 0; i < count && start + i < BufferSlots; ++i) {
        const UINT slot = start + i;
        ID3D11Buffer* engine = buffers ? buffers[i] : nullptr;
        if (g_held.dependency[slot] && engine != g_held.dependencyBuffer[slot]) { flush = true; return buffers; }
        if (g_held.buffer[slot]) touched = true;
    }
    if (!touched) return buffers;
    for (UINT i = 0; i < count; ++i) {
        const UINT slot = start + i;
        ID3D11Buffer* engine = buffers ? buffers[i] : nullptr;
        if (slot < BufferSlots && g_held.buffer[slot]) { Keep(g_held.engineBuffer[slot], engine); scratch[i] = g_held.oursBuffer[slot]; }
        else scratch[i] = engine;
    }
    return scratch;
}
bool TouchesBuffers(UINT start, UINT count) {
    for (UINT i = 0; i < count && start + i < BufferSlots; ++i)
        if (g_held.buffer[start + i] || g_held.dependency[start + i]) return true;
    return false;
}
bool SameVertexShader(ID3D11VertexShader* shader) { return shader == g_held.vertexShader; }
} // namespace Detail

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}
bool Enabled() { return g_enabled.load(std::memory_order_relaxed) && g_original.attached; }

bool Active(ID3D11DeviceContext* context) { return Detail::Holds(context); }
void NoteReuse() { ++g_stats.reused; }
void Suspend() { ++g_suspended; }
void Resume() { if (g_suspended) --g_suspended; }
void FlushIfIdle(End reason) { if (!g_suspended) Flush(reason); }

void Adopt(ID3D11DeviceContext* context, const Substitution& s) {
    // The caller's references are ours now whatever happens.
    if (!context) return;
    if (g_held.context || !g_original.attached) {
        // Nothing can be held: hand the engine its bindings straight back.
        if (s.engineShader) { context->PSSetShader(s.engineShader, nullptr, 0); s.engineShader->Release(); }
        for (unsigned i = 0; i < s.viewCount; ++i) { context->PSSetShaderResources(s.viewSlot[i], 1, &s.engineView[i]); if (s.engineView[i]) s.engineView[i]->Release(); }
        for (unsigned i = 0; i < s.samplerCount; ++i) { context->PSSetSamplers(s.samplerSlot[i], 1, &s.engineSampler[i]); if (s.engineSampler[i]) s.engineSampler[i]->Release(); }
        for (unsigned i = 0; i < s.bufferCount; ++i) { context->PSSetConstantBuffers(s.bufferSlot[i], 1, &s.engineBuffer[i]); if (s.engineBuffer[i]) s.engineBuffer[i]->Release(); }
        return;
    }
    auto& h = g_held;
    h.shader = s.engineShader != nullptr;
    h.engineShader = s.engineShader;
    h.viewLow = ViewSlots; h.viewHigh = 0;
    for (unsigned i = 0; i < s.viewCount; ++i) {
        const UINT slot = s.viewSlot[i];
        h.view[slot] = true; h.engineView[slot] = s.engineView[i];
        h.oursView[slot] = s.oursView[i]; if (h.oursView[slot]) h.oursView[slot]->AddRef();
        if (slot < h.viewLow) h.viewLow = slot;
        if (slot + 1 > h.viewHigh) h.viewHigh = slot + 1;
    }
    for (unsigned i = 0; i < s.samplerCount; ++i) {
        const UINT slot = s.samplerSlot[i];
        h.sampler[slot] = true; h.engineSampler[slot] = s.engineSampler[i];
        h.oursSampler[slot] = s.oursSampler[i]; if (h.oursSampler[slot]) h.oursSampler[slot]->AddRef();
    }
    for (unsigned i = 0; i < s.bufferCount; ++i) {
        const UINT slot = s.bufferSlot[i];
        h.buffer[slot] = true; h.engineBuffer[slot] = s.engineBuffer[i];
        h.oursBuffer[slot] = s.oursBuffer[i]; if (h.oursBuffer[slot]) h.oursBuffer[slot]->AddRef();
    }
    // Held with a reference, so a new buffer can never reuse the address and pass as the old one.
    for (unsigned i = 0; i < s.dependencyCount; ++i) {
        const UINT slot = s.dependencySlot[i];
        if (slot >= BufferSlots || h.buffer[slot]) continue;
        h.dependency[slot] = true;
        g_original.getConstantBuffers(context, slot, 1, &h.dependencyBuffer[slot]);
    }
    context->VSGetShader(&h.vertexShader, nullptr, nullptr);
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, h.targets, &h.depth);
    context->OMGetBlendState(&h.blend, h.blendFactor, &h.blendMask);
    UINT viewports = 1;
    context->RSGetViewports(&viewports, &h.viewport);
    h.context = context;
    ++g_stats.adopted;
}

void Flush(End reason) {
    auto& h = g_held;
    if (!h.context) return;
    ID3D11DeviceContext* context = h.context;
    h.context = nullptr; // the calls below must pass straight through the detours
    if (h.shader) g_original.setShader(context, h.engineShader, nullptr, 0);
    Drop(h.engineShader); h.shader = false;
    for (unsigned slot = h.viewLow; slot < h.viewHigh;) {
        if (!h.view[slot]) { ++slot; continue; }
        const unsigned start = slot;
        while (slot < h.viewHigh && h.view[slot]) ++slot;
        g_original.setShaderResources(context, start, slot - start, h.engineView + start);
        for (unsigned i = start; i < slot; ++i) { Drop(h.engineView[i]); Drop(h.oursView[i]); h.view[i] = false; }
    }
    h.viewLow = ViewSlots; h.viewHigh = 0;
    for (unsigned slot = 0; slot < SamplerSlots; ++slot) if (h.sampler[slot]) {
        g_original.setSamplers(context, slot, 1, &h.engineSampler[slot]);
        Drop(h.engineSampler[slot]); Drop(h.oursSampler[slot]); h.sampler[slot] = false;
    }
    for (unsigned slot = 0; slot < BufferSlots;) {
        if (!h.buffer[slot]) { Drop(h.dependencyBuffer[slot]); h.dependency[slot] = false; ++slot; continue; }
        const unsigned start = slot;
        while (slot < BufferSlots && h.buffer[slot]) ++slot;
        g_original.setConstantBuffers(context, start, slot - start, h.engineBuffer + start);
        for (unsigned i = start; i < slot; ++i) { Drop(h.engineBuffer[i]); Drop(h.oursBuffer[i]); h.buffer[i] = false; }
    }
    Drop(h.vertexShader);
    for (auto& target : h.targets) Drop(target);
    Drop(h.depth); Drop(h.blend);
    ++g_stats.ended[unsigned(reason)];
}

bool OnSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader, UINT classInstances) {
    if (!Detail::Holds(context)) return false;
    // Without a program substitution the context already holds the engine's program, and the
    // lighting decision was made for it: only the same program again keeps the substitution.
    if (!classInstances && g_held.shader && shader == g_held.engineShader) return true;
    Flush(End::Shader);
    return false;
}
void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) {
    if (!Detail::Holds(context)) return;
    bool same = depth == g_held.depth;
    for (UINT i = 0; same && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        same = (i < count && targets ? targets[i] : nullptr) == g_held.targets[i];
    if (!same) Flush(End::Targets);
}
void OnSetBlendState(ID3D11DeviceContext* context, ID3D11BlendState* blend, const FLOAT factor[4], UINT mask) {
    if (!Detail::Holds(context)) return;
    static const FLOAT ones[4]{1, 1, 1, 1};
    if (blend != g_held.blend || mask != g_held.blendMask ||
        std::memcmp(factor ? factor : ones, g_held.blendFactor, sizeof(g_held.blendFactor)))
        Flush(End::Blend);
}
void OnSetViewports(ID3D11DeviceContext* context, UINT count, const D3D11_VIEWPORT* viewports) {
    if (!Detail::Holds(context)) return;
    if (count != 1 || !viewports || std::memcmp(viewports, &g_held.viewport, sizeof(D3D11_VIEWPORT)))
        Flush(End::Viewport);
}

Stats TakeStats(bool reset) {
    const Stats copy = g_stats;
    if (reset) g_stats = Stats{};
    return copy;
}
const char* EndName(End reason) {
    static const char* names[]{"program", "vertexShader", "extraStage", "targets", "blend", "viewport", "constants",
        "constants1", "commandList", "clearState", "present", "ineligible", "reset"};
    return unsigned(reason) < unsigned(End::Count) ? names[unsigned(reason)] : "?";
}

} // namespace Filmmaker::PixelStateLayer
