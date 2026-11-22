#define NOMINMAX
#include "EnvironmentReflectionProbes.h"
#include "EnvironmentReflectionProbeMath.h"
#include "EnvironmentNativeMaterial.h"
#include "EnvironmentNativeMaterialDetail.h" // SunAbiInfoTag, PostCompressionTag, NativeSunAbiInfo
#include "EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Filmmaker::Environment {
namespace {
using Microsoft::WRL::ComPtr;

// Compares CS2's post-process input (raw scene light; decoded when the shipped shader stores it
// compressed) with its finished output (read through sRGB, the composite's own space) at a grid of
// central pixels, per channel, in half-stop bins of raw light. The sums go back to the CPU, which
// fills the curve (FillToneCurve).
constexpr char kToneShader[] = R"HLSL(
Texture2D<float4> rawInput : register(t0);
Texture2D<float4> finished : register(t1);
RWByteAddressBuffer bins : register(u0);
cbuffer ToneParameters : register(b0) { uint4 window; uint4 options; };
static const float kLow = -12, kHigh = 12;
static const uint kBins = 48, kGridX = 64, kGridY = 36;
[numthreads(8, 4, 1)]
void ToneMeasureCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kGridX || id.y >= kGridY) return;
    uint2 at = uint2((float2(id.xy) + .5) / float2(kGridX, kGridY) * float2(window.zw));
    float3 raw = rawInput.Load(int3(window.xy + at, 0)).rgb;
    if (options.x) raw = raw / max(1 - max(raw.r, max(raw.g, raw.b)), .00048828125);
    float3 shown = finished.Load(int3(at, 0)).rgb;
    [unroll] for (uint c = 0; c < 3; ++c) {
        float v = raw[c];
        if (!(v > 0)) continue;
        float u = (log2(v) - kLow) / (kHigh - kLow) * kBins;
        if (u < 0 || u >= kBins) continue;
        uint bin = (c * kBins + (uint)u) * 8;
        uint ignored;
        bins.InterlockedAdd(bin, (uint)(saturate(shown[c]) * 4096 + .5), ignored);
        bins.InterlockedAdd(bin + 4, 1, ignored);
    }
}
)HLSL";
constexpr unsigned kToneWords = kToneBins * 3 * 2;
constexpr unsigned kToneStaging = 3;

struct State {
    ComPtr<ID3D11Device> device;
    bool wanted = false;          // the composite drew a wet film last frame
    bool caughtThisPresent = false, measuredThisPresent = false;
    bool shadersFailed = false;
    // CS2's probes: the cube array, the cb3 copy the composite reads, and the decoded records.
    ComPtr<ID3D11ShaderResourceView> cubes;
    unsigned cubeCount = 0, cubeMips = 0;
    ComPtr<ID3D11Buffer> rows;    // a copy of CS2's cb3, bound at b8
    UINT rowsBytes = 0;
    ComPtr<ID3D11Buffer> rowsStaging;
    bool rowsReadPending = false;
    ID3D11Buffer* rowsSource = nullptr; // identity only: which engine buffer the records came from
    ID3D11ShaderResourceView* cubesSource = nullptr;
    ULONGLONG recordsReadAt = 0;
    std::vector<ProbeRecord> records;
    // The tone curve.
    ComPtr<ID3D11ComputeShader> toneShader;
    ComPtr<ID3D11Buffer> toneParameters, bins;
    ComPtr<ID3D11UnorderedAccessView> binsUav;
    ComPtr<ID3D11Buffer> binsStaging[kToneStaging];
    bool binsPending[kToneStaging]{};
    unsigned binsNext = 0;
    ComPtr<ID3D11Texture2D> window;
    ComPtr<ID3D11ShaderResourceView> windowSrv;
    D3D11_TEXTURE2D_DESC windowDesc{};
    ComPtr<ID3D11Texture2D> resolved; // CS2's multisampled output, one sample a pixel
    D3D11_TEXTURE2D_DESC resolvedDesc{};
    ComPtr<ID3D11Texture1D> tone;
    ComPtr<ID3D11ShaderResourceView> toneSrv;
    std::array<std::array<float, kToneBins>, 3> curve{};
    bool curveValid = false;
    ULONGLONG curveAt = 0;
    unsigned lastMode = 0, lastCount = 0;
} state;

// Why nothing was caught or measured: counted per draw, written to the mvm_debug log every 300
// presents while it runs.
struct Misses {
    unsigned presents = 0, notWanted = 0, viewport = 0, untagged = 0, unbound = 0, notArray = 0, rowsSize = 0, caught = 0;
    unsigned toneUnbound = 0, toneLayout = 0, toneResources = 0, toneMeasured = 0;
    unsigned inFormat = 0, outFormat = 0, inWidth = 0, outWidth = 0, inHeight = 0, outHeight = 0, inSamples = 0, outSamples = 0;
} misses;

bool EnsureDevice(ID3D11DeviceContext* ctx) {
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (!device) return false;
    if (state.device.Get() != device.Get()) {
        ReflectionProbes_Release();
        state.device = device;
    }
    return true;
}

DXGI_FORMAT TypelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
DXGI_FORMAT UnormOf(DXGI_FORMAT typeless) {
    return typeless == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
}
DXGI_FORMAT SrgbOf(DXGI_FORMAT typeless) {
    return typeless == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool EnsureToneResources() {
    if (state.toneShader) return true;
    if (state.shadersFailed) return false;
    RenderPrivateTextureScope privateTextures;
    ComPtr<ID3DBlob> code, error;
    if (FAILED(CompileEnvironmentShader(kToneShader, sizeof(kToneShader) - 1, "EnvironmentReflectionTone", nullptr, "ToneMeasureCS", "cs_5_0",
                                        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &error)) ||
        FAILED(state.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &state.toneShader))) {
        MvmDebugLog_LinefAlways("environment.reflection.tone", "compile failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "no compiler");
        state.shadersFailed = true;
        return false;
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 32; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC raw{};
    raw.ByteWidth = kToneWords * 4; raw.Usage = D3D11_USAGE_DEFAULT;
    raw.BindFlags = D3D11_BIND_UNORDERED_ACCESS; raw.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kToneWords; uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    D3D11_BUFFER_DESC staging = raw;
    staging.Usage = D3D11_USAGE_STAGING; staging.BindFlags = 0; staging.MiscFlags = 0; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_TEXTURE1D_DESC td{};
    td.Width = kToneBins; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(state.device->CreateBuffer(&cb, nullptr, &state.toneParameters)) ||
        FAILED(state.device->CreateBuffer(&raw, nullptr, &state.bins)) ||
        FAILED(state.device->CreateUnorderedAccessView(state.bins.Get(), &uav, &state.binsUav)) ||
        FAILED(state.device->CreateTexture1D(&td, nullptr, &state.tone)) ||
        FAILED(state.device->CreateShaderResourceView(state.tone.Get(), nullptr, &state.toneSrv))) {
        state.shadersFailed = true;
        return false;
    }
    for (auto& s : state.binsStaging)
        if (FAILED(state.device->CreateBuffer(&staging, nullptr, &s))) { state.shadersFailed = true; return false; }
    return true;
}

// The oldest measurement the GPU has finished, folded into the curve (a quarter per frame, so a
// cut or a flash eases in rather than flickering the reflections).
void ReadToneBins(ID3D11DeviceContext* ctx) {
    for (unsigned k = 0; k < kToneStaging; ++k) {
        const unsigned i = (state.binsNext + k) % kToneStaging;
        if (!state.binsPending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(state.binsStaging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
        const auto* words = static_cast<const uint32_t*>(m.pData);
        bool all = true;
        std::array<std::array<float, kToneBins>, 3> measured{};
        for (unsigned c = 0; c < 3; ++c) {
            std::array<float, kToneBins> sum{};
            std::array<uint32_t, kToneBins> count{};
            for (unsigned b = 0; b < kToneBins; ++b) {
                sum[b] = words[(c * kToneBins + b) * 2] / 4096.f;
                count[b] = words[(c * kToneBins + b) * 2 + 1];
            }
            bool any = false;
            measured[c] = FillToneCurve(sum, count, any);
            all = all && any;
        }
        ctx->Unmap(state.binsStaging[i].Get(), 0);
        state.binsPending[i] = false;
        if (!all) continue;
        const float keep = state.curveValid ? .75f : 0.f;
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned b = 0; b < kToneBins; ++b) state.curve[c][b] = state.curve[c][b] * keep + measured[c][b] * (1 - keep);
        state.curveValid = true;
        state.curveAt = GetTickCount64();
        float texels[kToneBins * 4];
        for (unsigned b = 0; b < kToneBins; ++b) {
            texels[b * 4] = state.curve[0][b]; texels[b * 4 + 1] = state.curve[1][b];
            texels[b * 4 + 2] = state.curve[2][b]; texels[b * 4 + 3] = 1;
        }
        ctx->UpdateSubresource(state.tone.Get(), 0, nullptr, texels, 0, 0);
    }
}

// The probe records, read back twice a second without waiting on the GPU (the copy made now is
// mapped on a later frame). CS2 lists only the probes its view can use, so the list follows the camera.
void ReadRecords(ID3D11DeviceContext* ctx) {
    if (!state.rowsReadPending || !state.rowsStaging) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (ctx->Map(state.rowsStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
    state.records = DecodeProbeRecords(static_cast<const float*>(m.pData), state.rowsBytes / 16, state.cubeCount);
    ctx->Unmap(state.rowsStaging.Get(), 0);
    state.rowsReadPending = false;
    // Logged when the list changes, not on every read.
    unsigned signature = unsigned(state.records.size());
    for (const auto& r : state.records) signature = signature * 131u + r.cube * 7u + r.index;
    static unsigned logged = ~0u;
    if (MvmDebugLog_Active() && signature != logged) {
        logged = signature;
        MvmDebugLog_LinefAlways("environment.reflection.probes", "records=%zu cubes=%u mips=%u rows=%u",
                                state.records.size(), state.cubeCount, state.cubeMips, state.rowsBytes / 16);
        for (size_t i = 0; i < state.records.size() && i < 24; ++i) {
            const auto& r = state.records[i];
            MvmDebugLog_LinefAlways("environment.reflection.probe", "n=%u cube=%u world=(%.0f %.0f %.0f)..(%.0f %.0f %.0f)",
                                    r.index, r.cube, r.worldMin[0], r.worldMin[1], r.worldMin[2], r.worldMax[0], r.worldMax[1], r.worldMax[2]);
        }
    }
}

} // namespace

void ReflectionProbes_Want(bool wanted) { state.wanted = wanted; }
void ReflectionProbes_EndPresent() {
    state.caughtThisPresent = state.measuredThisPresent = false;
    if (!MvmDebugLog_Active() || ++misses.presents < 300) return;
    MvmDebugLog_LinefAlways("environment.reflection.misses",
        "presents=%u notWanted=%u viewport=%u untagged=%u unbound=%u notArray=%u rowsSize=%u caught=%u "
        "toneUnbound=%u toneLayout=%u toneResources=%u toneMeasured=%u in=%u/%ux%u/%u out=%u/%ux%u/%u records=%zu curve=%u",
        misses.presents, misses.notWanted, misses.viewport, misses.untagged, misses.unbound, misses.notArray, misses.rowsSize,
        misses.caught, misses.toneUnbound, misses.toneLayout, misses.toneResources, misses.toneMeasured, misses.inFormat,
        misses.inWidth, misses.inHeight, misses.inSamples, misses.outFormat, misses.outWidth, misses.outHeight, misses.outSamples,
        state.records.size(), unsigned(state.curveValid));
    misses = Misses{};
}

void ReflectionProbes_NoteDraw(ID3D11DeviceContext* ctx) {
    if (!state.wanted) { ++misses.notWanted; return; }
    if (state.caughtThisPresent || !ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    D3D11_VIEWPORT vp{};
    UINT count = 1;
    ctx->RSGetViewports(&count, &vp);
    // The main world pass only (the 3D skybox and the first-person pass use other depth ranges).
    if (count != 1 || vp.Width < 256 || vp.MinDepth != 0 || std::abs(vp.MaxDepth - .95f) > 1e-6f) { ++misses.viewport; return; }
    ComPtr<ID3D11PixelShader> ps;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    NativeSunAbiInfo info{};
    UINT size = sizeof(info);
    if (FAILED(ps->GetPrivateData(NativeDetail::SunAbiInfoTag, &size, &info)) || !info.cubeArraySlotMask) { ++misses.untagged; return; }
    unsigned slot = 0;
    while (!(info.cubeArraySlotMask & (1u << slot))) ++slot;
    ComPtr<ID3D11ShaderResourceView> cubes;
    ctx->PSGetShaderResources(slot, 1, &cubes);
    ComPtr<ID3D11Buffer> cb3;
    ctx->PSGetConstantBuffers(3, 1, &cb3);
    if (!cubes || !cb3 || !EnsureDevice(ctx)) { ++misses.unbound; return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    cubes->GetDesc(&view);
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) { ++misses.notArray; return; }
    D3D11_BUFFER_DESC desc{};
    cb3->GetDesc(&desc);
    const UINT needed = (kProbeRecordFirstRow + kProbeRecordRows) * 16;
    if (desc.ByteWidth < needed || desc.ByteWidth > 65536 || desc.ByteWidth % 16) { ++misses.rowsSize; return; }
    state.caughtThisPresent = true;
    ++misses.caught;
    // A readback started on an earlier frame. Here, not in Bind: the composite records into a
    // deferred context, which cannot map anything for reading.
    ReadRecords(ctx);
    RenderPrivateTextureScope privateTextures;
    if (!state.rows || state.rowsBytes != desc.ByteWidth) {
        state.rows.Reset(); state.rowsStaging.Reset();
        D3D11_BUFFER_DESC copy{};
        copy.ByteWidth = desc.ByteWidth; copy.Usage = D3D11_USAGE_DEFAULT; copy.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BUFFER_DESC staging = copy;
        staging.Usage = D3D11_USAGE_STAGING; staging.BindFlags = 0; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(state.device->CreateBuffer(&copy, nullptr, &state.rows)) || FAILED(state.device->CreateBuffer(&staging, nullptr, &state.rowsStaging))) {
            state.rows.Reset(); state.rowsStaging.Reset();
            return;
        }
        state.rowsBytes = desc.ByteWidth;
        state.rowsSource = nullptr;
    }
    // The probe rows go with the frame the composite draws (the copy is on the GPU, a few dozen KB).
    ctx->CopyResource(state.rows.Get(), cb3.Get());
    state.cubes = cubes;
    state.cubeCount = view.TextureCubeArray.NumCubes;
    state.cubeMips = view.TextureCubeArray.MipLevels == UINT(-1) ? 1 : view.TextureCubeArray.MipLevels;
    if (state.cubeMips == 0 || state.cubeMips == UINT(-1)) {
        ComPtr<ID3D11Resource> resource; cubes->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) { D3D11_TEXTURE2D_DESC td{}; texture->GetDesc(&td); state.cubeMips = td.MipLevels; }
    }
    const ULONGLONG now = GetTickCount64();
    if (!state.rowsReadPending && (state.rowsSource != cb3.Get() || state.cubesSource != cubes.Get() || now - state.recordsReadAt > 500)) {
        ctx->CopyResource(state.rowsStaging.Get(), cb3.Get());
        state.rowsReadPending = true;
        state.rowsSource = cb3.Get();
        state.cubesSource = cubes.Get();
        state.recordsReadAt = now;
    }
}

void ReflectionProbes_AfterTonemap(ID3D11DeviceContext* ctx) {
    if (!state.wanted || state.measuredThisPresent || !ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !EnsureDevice(ctx)) return;
    ReadToneBins(ctx);
    ComPtr<ID3D11ShaderResourceView> input;
    ctx->PSGetShaderResources(12, 1, &input);
    ID3D11RenderTargetView* rawTarget = nullptr;
    ctx->OMGetRenderTargets(1, &rawTarget, nullptr);
    ComPtr<ID3D11RenderTargetView> target;
    target.Attach(rawTarget);
    if (!input || !target) { ++misses.toneUnbound; return; }
    ComPtr<ID3D11Resource> inputResource, outputResource;
    input->GetResource(&inputResource);
    target->GetResource(&outputResource);
    ComPtr<ID3D11Texture2D> inputTexture, output;
    if (FAILED(inputResource.As(&inputTexture)) || FAILED(outputResource.As(&output))) { ++misses.toneUnbound; return; }
    D3D11_TEXTURE2D_DESC in{}, out{};
    inputTexture->GetDesc(&in);
    output->GetDesc(&out);
    const DXGI_FORMAT typeless = TypelessOf(out.Format);
    if (in.SampleDesc.Count != 1 || in.Width != out.Width || in.Height != out.Height ||
        typeless == DXGI_FORMAT_UNKNOWN || (in.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS && in.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        ++misses.toneLayout;
        misses.inFormat = in.Format; misses.outFormat = out.Format; misses.inWidth = in.Width; misses.outWidth = out.Width;
        misses.inHeight = in.Height; misses.outHeight = out.Height;
        misses.inSamples = in.SampleDesc.Count * 100 + in.ArraySize; misses.outSamples = out.SampleDesc.Count * 100 + out.ArraySize;
        return;
    }
    if (!EnsureToneResources()) { ++misses.toneResources; return; }
    state.measuredThisPresent = true;
    ++misses.toneMeasured;
    // The middle half of the frame: the edges carry the vignette.
    const UINT x0 = out.Width / 4, y0 = out.Height / 4, w = out.Width / 2, h = out.Height / 2;
    RenderPrivateTextureScope privateTextures;
    if (!state.window || state.windowDesc.Width != w || state.windowDesc.Height != h || state.windowDesc.Format != typeless) {
        state.window.Reset(); state.windowSrv.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = typeless; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = SrgbOf(typeless); v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (FAILED(state.device->CreateTexture2D(&d, nullptr, &state.window)) ||
            FAILED(state.device->CreateShaderResourceView(state.window.Get(), &v, &state.windowSrv))) {
            state.window.Reset(); state.windowSrv.Reset();
            return;
        }
        state.windowDesc = d;
    }
    // With MSAA on, CS2 tonemaps into a multisampled target (4x measured), which cannot be copied
    // from directly: it is resolved to one sample first.
    ID3D11Texture2D* finished = output.Get();
    if (out.SampleDesc.Count > 1) {
        if (!state.resolved || state.resolvedDesc.Width != out.Width || state.resolvedDesc.Height != out.Height || state.resolvedDesc.Format != typeless) {
            state.resolved.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = out.Width; d.Height = out.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = typeless; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(state.device->CreateTexture2D(&d, nullptr, &state.resolved))) { state.resolved.Reset(); return; }
            state.resolvedDesc = d;
        }
        ctx->ResolveSubresource(state.resolved.Get(), 0, output.Get(), 0, UnormOf(typeless));
        finished = state.resolved.Get();
    }
    const D3D11_BOX box{x0, y0, 0, x0 + w, y0 + h, 1};
    ctx->CopySubresourceRegion(state.window.Get(), 0, 0, 0, 0, finished, 0, &box);
    UINT compressed = 0, tagBytes = sizeof(compressed);
    ComPtr<ID3D11PixelShader> post;
    ctx->PSGetShader(&post, nullptr, nullptr);
    if (post) post->GetPrivateData(NativeDetail::PostCompressionTag, &tagBytes, &compressed);
    const uint32_t parameters[8]{x0, y0, w, h, compressed == 1 ? 1u : 0u, 0, 0, 0};
    ctx->UpdateSubresource(state.toneParameters.Get(), 0, nullptr, parameters, 0, 0);
    // Only the compute slots used here are saved and restored (as EnvironmentAmbientOcclusion does).
    ComPtr<ID3D11ComputeShader> previousShader;
    ID3D11ClassInstance* previousInstances[256]{};
    UINT previousInstanceCount = 256;
    ctx->CSGetShader(previousShader.GetAddressOf(), previousInstances, &previousInstanceCount);
    ID3D11Buffer* previousConstants = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &previousConstants);
    ID3D11ShaderResourceView* previousViews[2]{};
    ctx->CSGetShaderResources(0, 2, previousViews);
    ID3D11UnorderedAccessView* previousUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &previousUav);
    const UINT zero[4]{};
    ctx->ClearUnorderedAccessViewUint(state.binsUav.Get(), zero);
    auto cb = state.toneParameters.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* views[2]{input.Get(), state.windowSrv.Get()};
    ctx->CSSetShaderResources(0, 2, views);
    auto uav = state.binsUav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(state.toneShader.Get(), nullptr, 0);
    ctx->Dispatch(8, 9, 1);
    ctx->CSSetShader(previousShader.Get(), previousInstances, previousInstanceCount);
    ctx->CSSetConstantBuffers(0, 1, &previousConstants);
    ctx->CSSetShaderResources(0, 2, previousViews);
    ctx->CSSetUnorderedAccessViews(0, 1, &previousUav, nullptr);
    for (UINT i = 0; i < previousInstanceCount; ++i) if (previousInstances[i]) previousInstances[i]->Release();
    if (previousConstants) previousConstants->Release();
    for (auto* v : previousViews) if (v) v->Release();
    if (previousUav) previousUav->Release();
    const unsigned slot = state.binsNext;
    if (!state.binsPending[slot]) {
        ctx->CopyResource(state.binsStaging[slot].Get(), state.bins.Get());
        state.binsPending[slot] = true;
        state.binsNext = (slot + 1) % kToneStaging;
    }
}

ReflectionProbeConstants ReflectionProbes_Bind(ID3D11DeviceContext* ctx, const float camera[3], bool environmentLighting) {
    ReflectionProbeConstants c;
    if (!ctx) return c;
    // A curve not refreshed for a second (a menu, a paused present loop) is not trusted.
    const bool tone = state.curveValid && GetTickCount64() - state.curveAt < 1000;
    ID3D11ShaderResourceView* cubes = nullptr;
    unsigned mode = 0, mips = 0;
    if (tone && environmentLighting) {
        cubes = NativeLiveReflection(&mips);
        if (cubes) mode = 2;
    } else if (tone && state.cubes && !state.records.empty()) {
        cubes = state.cubes.Get();
        mips = state.cubeMips;
        const auto chosen = SelectProbes(state.records, camera, kProbesPerFrame);
        for (size_t i = 0; i < chosen.size(); ++i) c.index[i] = float(chosen[i]);
        c.info[2] = float(chosen.size());
        mode = chosen.empty() ? 0 : 1;
    }
    c.info[0] = float(mode);
    c.info[1] = mips > 1 ? float(mips - 1) : 0.f;
    c.info[3] = tone ? 1.f : 0.f;
    ID3D11ShaderResourceView* views[2]{mode ? cubes : nullptr, tone ? state.toneSrv.Get() : nullptr};
    ctx->PSSetShaderResources(51, 2, views);
    ID3D11Buffer* rows = mode == 1 ? state.rows.Get() : nullptr;
    ctx->PSSetConstantBuffers(8, 1, &rows);
    if (MvmDebugLog_Active() && (mode != state.lastMode || unsigned(c.info[2]) != state.lastCount)) {
        MvmDebugLog_LinefAlways("environment.reflection.mode", "mode=%u probes=%u records=%zu tone=%u mips=%u",
                                mode, unsigned(c.info[2]), state.records.size(), unsigned(tone), mips);
    }
    state.lastMode = mode;
    state.lastCount = unsigned(c.info[2]);
    return c;
}

void ReflectionProbes_Release() {
    const bool wanted = state.wanted;
    state = State{};
    state.wanted = wanted;
}

std::string ReflectionProbes_Status() {
    char line[160];
    std::snprintf(line, sizeof(line), "off-screen reflections: %s, %zu CS2 probes, tone curve %s",
                  state.lastMode == 2 ? "live capture" : state.lastMode == 1 ? "CS2 probes" : "none",
                  state.records.size(), state.curveValid ? "measured" : "not measured");
    return line;
}

} // namespace Filmmaker::Environment
