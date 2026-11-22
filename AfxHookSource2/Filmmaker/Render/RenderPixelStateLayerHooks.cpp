// Device-context detours of the pixel state layer (RenderPixelStateLayer.h).
#include "RenderPixelStateLayer.h"
#include "RenderPixelStateLayerDetail.h"

#include "../../../Shared/AfxDetours.h"
#include "../Debug/MvmDebugLog.h"

#include <d3d11_1.h>

namespace Filmmaker::PixelStateLayer {
namespace {
using namespace Detail;

typedef void (STDMETHODCALLTYPE* SetShaderResources_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void (STDMETHODCALLTYPE* SetSamplers_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
typedef void (STDMETHODCALLTYPE* SetConstantBuffers_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void (STDMETHODCALLTYPE* SetConstantBuffers1_t)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void (STDMETHODCALLTYPE* GetShaderResources_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView**);
typedef void (STDMETHODCALLTYPE* GetShader_t)(ID3D11DeviceContext*, ID3D11PixelShader**, ID3D11ClassInstance**, UINT*);
typedef void (STDMETHODCALLTYPE* GetSamplers_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState**);
typedef void (STDMETHODCALLTYPE* GetConstantBuffers_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer**);
typedef void (STDMETHODCALLTYPE* GetConstantBuffers1_t)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer**, UINT*, UINT*);
typedef void (STDMETHODCALLTYPE* SetVertexShader_t)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
typedef void (STDMETHODCALLTYPE* SetStageShader_t)(ID3D11DeviceContext*, void*, ID3D11ClassInstance* const*, UINT);
typedef void (STDMETHODCALLTYPE* SetTargetsAndViews_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void (STDMETHODCALLTYPE* ExecuteCommandList_t)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void (STDMETHODCALLTYPE* ClearState_t)(ID3D11DeviceContext*);
typedef void (STDMETHODCALLTYPE* SwapState_t)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);

SetShaderResources_t g_setShaderResources = nullptr;
SetSamplers_t g_setSamplers = nullptr;
SetConstantBuffers_t g_setConstantBuffers = nullptr;
SetConstantBuffers1_t g_setConstantBuffers1 = nullptr;
GetShaderResources_t g_getShaderResources = nullptr;
GetShader_t g_getShader = nullptr;
GetSamplers_t g_getSamplers = nullptr;
GetConstantBuffers_t g_getConstantBuffers = nullptr;
GetConstantBuffers1_t g_getConstantBuffers1 = nullptr;
SetVertexShader_t g_setVertexShader = nullptr;
SetStageShader_t g_setGeometryShader = nullptr, g_setHullShader = nullptr, g_setDomainShader = nullptr;
SetTargetsAndViews_t g_setTargetsAndViews = nullptr;
ExecuteCommandList_t g_executeCommandList = nullptr;
ClearState_t g_clearState = nullptr;
SwapState_t g_swapState = nullptr;
bool g_pending = false;

void STDMETHODCALLTYPE New_SetShaderResources(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11ShaderResourceView* const* views) {
    if (Holds(context) && count <= ViewSlots) {
        ID3D11ShaderResourceView* scratch[ViewSlots];
        views = SetViews(start, count, views, scratch);
        g_setShaderResources(context, start, count, views);
        return;
    }
    g_setShaderResources(context, start, count, views);
}
void STDMETHODCALLTYPE New_SetSamplers(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11SamplerState* const* samplers) {
    if (Holds(context) && count <= SamplerSlots) {
        ID3D11SamplerState* scratch[SamplerSlots];
        samplers = SetSamplers(start, count, samplers, scratch);
        g_setSamplers(context, start, count, samplers);
        return;
    }
    g_setSamplers(context, start, count, samplers);
}
void STDMETHODCALLTYPE New_SetConstantBuffers(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11Buffer* const* buffers) {
    if (Holds(context) && count <= BufferSlots) {
        ID3D11Buffer* scratch[BufferSlots];
        bool flush = false;
        ID3D11Buffer* const* forwarded = SetBuffers(start, count, buffers, scratch, flush);
        if (flush) Flush(End::Constants); else buffers = forwarded;
    }
    g_setConstantBuffers(context, start, count, buffers);
}
void STDMETHODCALLTYPE New_SetConstantBuffers1(ID3D11DeviceContext1* context, UINT start, UINT count, ID3D11Buffer* const* buffers,
                                               const UINT* first, const UINT* constants) {
    // A windowed binding cannot be kept beside ours without its offsets: end the substitution.
    if (Holds(context) && TouchesBuffers(start, count)) Flush(End::Constants1);
    g_setConstantBuffers1(context, start, count, buffers, first, constants);
}
void STDMETHODCALLTYPE New_GetShaderResources(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11ShaderResourceView** views) {
    g_getShaderResources(context, start, count, views);
    if (Holds(context)) FixViews(start, count, views);
}
void STDMETHODCALLTYPE New_GetShader(ID3D11DeviceContext* context, ID3D11PixelShader** shader, ID3D11ClassInstance** instances, UINT* instanceCount) {
    if (Holds(context) && ShaderHeld()) { GetShader(shader, instances, instanceCount); return; }
    g_getShader(context, shader, instances, instanceCount);
}
void STDMETHODCALLTYPE New_GetSamplers(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11SamplerState** samplers) {
    g_getSamplers(context, start, count, samplers);
    if (Holds(context)) FixSamplers(start, count, samplers);
}
void STDMETHODCALLTYPE New_GetConstantBuffers(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11Buffer** buffers) {
    g_getConstantBuffers(context, start, count, buffers);
    if (Holds(context)) FixBuffers(start, count, buffers);
}
void STDMETHODCALLTYPE New_GetConstantBuffers1(ID3D11DeviceContext1* context, UINT start, UINT count, ID3D11Buffer** buffers,
                                               UINT* first, UINT* constants) {
    g_getConstantBuffers1(context, start, count, buffers, first, constants);
    if (Holds(context)) FixBuffers(start, count, buffers);
}
void STDMETHODCALLTYPE New_SetVertexShader(ID3D11DeviceContext* context, ID3D11VertexShader* shader, ID3D11ClassInstance* const* instances, UINT count) {
    if (Holds(context) && !SameVertexShader(shader)) Flush(End::VertexShader);
    g_setVertexShader(context, shader, instances, count);
}
void STDMETHODCALLTYPE New_SetGeometryShader(ID3D11DeviceContext* context, void* shader, ID3D11ClassInstance* const* instances, UINT count) {
    if (shader && Holds(context)) Flush(End::ExtraStage);
    g_setGeometryShader(context, shader, instances, count);
}
void STDMETHODCALLTYPE New_SetHullShader(ID3D11DeviceContext* context, void* shader, ID3D11ClassInstance* const* instances, UINT count) {
    if (shader && Holds(context)) Flush(End::ExtraStage);
    g_setHullShader(context, shader, instances, count);
}
void STDMETHODCALLTYPE New_SetDomainShader(ID3D11DeviceContext* context, void* shader, ID3D11ClassInstance* const* instances, UINT count) {
    if (shader && Holds(context)) Flush(End::ExtraStage);
    g_setDomainShader(context, shader, instances, count);
}
void STDMETHODCALLTYPE New_SetTargetsAndViews(ID3D11DeviceContext* context, UINT targetCount, ID3D11RenderTargetView* const* targets,
    ID3D11DepthStencilView* depth, UINT viewStart, UINT viewCount, ID3D11UnorderedAccessView* const* views, const UINT* initialCounts) {
    if (Holds(context)) Flush(End::Targets);
    g_setTargetsAndViews(context, targetCount, targets, depth, viewStart, viewCount, views, initialCounts);
}
void STDMETHODCALLTYPE New_ExecuteCommandList(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore) {
    if (Holds(context)) Flush(End::CommandList);
    g_executeCommandList(context, list, restore);
}
void STDMETHODCALLTYPE New_ClearState(ID3D11DeviceContext* context) {
    if (Holds(context)) Flush(End::ClearState);
    g_clearState(context);
}
void STDMETHODCALLTYPE New_SwapState(ID3D11DeviceContext1* context, ID3DDeviceContextState* state, ID3DDeviceContextState** previous) {
    if (Holds(context)) Flush(End::ClearState);
    g_swapState(context, state, previous);
}

} // namespace

void Attach(ID3D11DeviceContext* context) {
    if (!context || GetOriginals().attached) return;
    void** vtable = *(void***)context;
    void* targets[17]{vtable[8], vtable[10], vtable[16], vtable[73], vtable[74], vtable[75], vtable[77], vtable[11],
        vtable[23], vtable[60], vtable[64], vtable[34], vtable[58], vtable[110]};
    unsigned count = 14;
    // ID3D11DeviceContext1 is the same object on every supported system; its extra methods follow.
    ID3D11DeviceContext1* context1 = nullptr;
    void** vtable1 = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&context1)) && context1) {
        vtable1 = *(void***)context1;
        targets[count++] = vtable1[123]; targets[count++] = vtable1[129]; targets[count++] = vtable1[131];
        context1->Release();
    }
    // Functions folded onto one address cannot take separate detours: leave the layer off.
    for (unsigned a = 0; a < count; ++a) for (unsigned b = a + 1; b < count; ++b)
        if (targets[a] == targets[b]) {
            MvmDebugLog_Linef("render.pixelstate", "attach skipped: methods %u and %u share code", a, b);
            return;
        }
    g_setShaderResources = (SetShaderResources_t)vtable[8];
    g_setSamplers = (SetSamplers_t)vtable[10];
    g_setConstantBuffers = (SetConstantBuffers_t)vtable[16];
    g_getShaderResources = (GetShaderResources_t)vtable[73];
    g_getShader = (GetShader_t)vtable[74];
    g_getSamplers = (GetSamplers_t)vtable[75];
    g_getConstantBuffers = (GetConstantBuffers_t)vtable[77];
    g_setVertexShader = (SetVertexShader_t)vtable[11];
    g_setGeometryShader = (SetStageShader_t)vtable[23];
    g_setHullShader = (SetStageShader_t)vtable[60];
    g_setDomainShader = (SetStageShader_t)vtable[64];
    g_setTargetsAndViews = (SetTargetsAndViews_t)vtable[34];
    g_executeCommandList = (ExecuteCommandList_t)vtable[58];
    g_clearState = (ClearState_t)vtable[110];
    bool ok = true;
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setShaderResources, New_SetShaderResources);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setSamplers, New_SetSamplers);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setConstantBuffers, New_SetConstantBuffers);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_getShaderResources, New_GetShaderResources);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_getShader, New_GetShader);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_getSamplers, New_GetSamplers);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_getConstantBuffers, New_GetConstantBuffers);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setVertexShader, New_SetVertexShader);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setGeometryShader, New_SetGeometryShader);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setHullShader, New_SetHullShader);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setDomainShader, New_SetDomainShader);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setTargetsAndViews, New_SetTargetsAndViews);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_executeCommandList, New_ExecuteCommandList);
    ok &= NO_ERROR == DetourAttach(&(PVOID&)g_clearState, New_ClearState);
    if (vtable1) {
        g_setConstantBuffers1 = (SetConstantBuffers1_t)vtable1[123];
        g_getConstantBuffers1 = (GetConstantBuffers1_t)vtable1[129];
        g_swapState = (SwapState_t)vtable1[131];
        ok &= NO_ERROR == DetourAttach(&(PVOID&)g_setConstantBuffers1, New_SetConstantBuffers1);
        ok &= NO_ERROR == DetourAttach(&(PVOID&)g_getConstantBuffers1, New_GetConstantBuffers1);
        ok &= NO_ERROR == DetourAttach(&(PVOID&)g_swapState, New_SwapState);
    }
    // The trampolines are valid once the caller commits its transaction; the layer holds nothing
    // before the first lit draw, long after that. Without the windowed-constant detours the engine
    // could rebind a constant buffer unseen, so the layer stays off.
    g_pending = ok && vtable1;
}

void Commit(void (STDMETHODCALLTYPE* setShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)) {
    if (!g_pending || !setShader || GetOriginals().attached) return;
    Originals originals;
    originals.attached = true;
    originals.setShader = setShader;
    originals.setShaderResources = g_setShaderResources;
    originals.setSamplers = g_setSamplers;
    originals.setConstantBuffers = g_setConstantBuffers;
    originals.getConstantBuffers = g_getConstantBuffers;
    SetOriginals(originals);
}

} // namespace Filmmaker::PixelStateLayer
