#define NOMINMAX
#include "AutoExposure.h"
#include "AutoExposureMath.h"

#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderCommand.h"
#include "../Render/RenderHdrCapture.h"
#include "../Render/RenderPrivateTextureScope.h"
#include "../../MirvTime.h"

#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace Filmmaker {
using Microsoft::WRL::ComPtr;
namespace {

// Center-weighted log2-luminance histogram of the pre-tonemap scene, then the mean of the bins
// between the 10th and 90th percentiles (Unreal's histogram metering: a few blown highlights or
// black corners do not swing the exposure). 64x64 sample points, one thread group.
constexpr char kMeterShader[] = R"HLSL(
cbuffer Parameters:register(b0) { float4 size; }  // width, height, aspect, unused
#if MSAA_SAMPLES > 1
Texture2DMS<float4,MSAA_SAMPLES> scene:register(t0);
#else
Texture2D<float4> scene:register(t0);
#endif
RWBuffer<float> result:register(u0);
static const uint kBins=64;
static const float kMinLog=-14,kMaxLog=10;
groupshared uint bins[kBins];
[numthreads(16,16,1)] void Meter(uint index:SV_GroupIndex,uint3 thread:SV_GroupThreadID) {
    if(index<kBins)bins[index]=0;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint y=0;y<4;++y)[unroll]for(uint x=0;x<4;++x){
        float2 uv=(float2(thread.xy*4+uint2(x,y))+.5)/64;
        int2 pixel=int2(uv*size.xy);
#if MSAA_SAMPLES > 1
        float3 c=scene.Load(pixel,0).rgb;
#else
        float3 c=scene.Load(int3(pixel,0)).rgb;
#endif
        float l=dot(c,float3(.2126,.7152,.0722));
        if(!(l>=0)||!isfinite(l))continue;
        float2 d=(uv-.5)*float2(size.z,1);
        // Centre 1, frame corners about a quarter: the subject counts most, like a camera's meter.
        float weight=exp2(-dot(d,d)*4);
        float bin=saturate((log2(max(l,1e-8))-kMinLog)/(kMaxLog-kMinLog))*(kBins-1);
        InterlockedAdd(bins[uint(bin+.5)],uint(weight*256+.5));
    }
    GroupMemoryBarrierWithGroupSync();
    if(index!=0)return;
    float total=0;for(uint i=0;i<kBins;++i)total+=bins[i];
    float low=total*.1,high=total*.9,below=0,sum=0,weight=0;
    for(uint b=0;b<kBins;++b){
        float n=bins[b];
        float inside=max(0,min(below+n,high)-max(below,low));
        sum+=inside*(kMinLog+(kMaxLog-kMinLog)*b/(kBins-1));weight+=inside;below+=n;
    }
    result[0]=weight>0?sum/weight:0;
    result[1]=weight>0?1:0;
    result[2]=total;
    result[3]=0;
}
)HLSL";

constexpr unsigned kSlots = 3;
struct RenderState {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> meter[4];
    bool failed = false;
    ComPtr<ID3D11Buffer> constants, result, staging[kSlots];
    ComPtr<ID3D11UnorderedAccessView> resultUav;
    // The scene's own SRV when it allows one; otherwise a resolved copy.
    ComPtr<ID3D11Texture2D> source, resolved;
    ComPtr<ID3D11ShaderResourceView> sourceSrv;
    UINT sourceSamples = 1;
    bool written[kSlots]{};
    unsigned frame = 0;
} g_render;

std::atomic<bool> g_enabled{false};
std::atomic<float> g_speed{kAutoExposureSpeedDefault};
std::atomic<float> g_compensation{0.f};
std::atomic<float> g_metered{0.f};
std::atomic<unsigned> g_meterCount{0};
float g_currentLog2 = NAN;
float g_appliedScale = 0.f;
LARGE_INTEGER g_lastCounter{};
double g_lastDemoTime = -1;

// Scale that maps the metered luminance to the key at 0 compensation. Calibrated so a daylit map
// lands near the engine's neutral 1.0 (see Docs/effects/night-mode.md).
constexpr float kKey = .18f;
constexpr float kMinScale = .05f, kMaxScale = 4.f;

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}

bool Initialize(ID3D11Device* device) {
    if (g_render.device.Get() == device && g_render.result) return true;
    if (g_render.failed && g_render.device.Get() == device) return false;
    g_render = RenderState{};
    g_render.device = device;
    RenderPrivateTextureScope privateTextures;
    for (unsigned i = 0; i < 4; ++i) {
        const std::string count = std::to_string(1u << i);
        const D3D_SHADER_MACRO macros[]{{"MSAA_SAMPLES", count.c_str()}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, error;
        if (FAILED(Environment::CompileEnvironmentShader(kMeterShader, sizeof(kMeterShader) - 1, "AutoExposureMeter", macros,
                "Meter", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &error)) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_render.meter[i]))) {
            MvmDebugLog_LinefAlways("world.autoexposure", "compile samples=%u error=%s", 1u << i,
                error ? static_cast<const char*>(error->GetBufferPointer()) : "create failed");
            g_render.failed = true;
            return false;
        }
    }
    D3D11_BUFFER_DESC cb{};cb.ByteWidth = 16;cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC rb{};rb.ByteWidth = 16;rb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format = DXGI_FORMAT_R32_FLOAT;uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements = 4;
    D3D11_BUFFER_DESC sb{};sb.ByteWidth = 16;sb.Usage = D3D11_USAGE_STAGING;sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &g_render.constants)) &&
              SUCCEEDED(device->CreateBuffer(&rb, nullptr, &g_render.result)) &&
              SUCCEEDED(device->CreateUnorderedAccessView(g_render.result.Get(), &uv, &g_render.resultUav));
    for (unsigned i = 0; ok && i < kSlots; ++i) ok = SUCCEEDED(device->CreateBuffer(&sb, nullptr, &g_render.staging[i]));
    if (!ok) { g_render.failed = true; g_render.result.Reset(); }
    return ok;
}

// SRV over the scene target, or over a resolved copy when the engine's target has no SRV bind.
ID3D11ShaderResourceView* SceneView(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene, const D3D11_TEXTURE2D_DESC& desc) {
    const bool direct = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    if (g_render.source.Get() != scene || !g_render.sourceSrv) {
        g_render.sourceSrv.Reset();g_render.resolved.Reset();g_render.source = scene;
        RenderPrivateTextureScope privateTextures;
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format = ViewFormat(desc.Format);
        if (direct) {
            g_render.sourceSamples = desc.SampleDesc.Count;
            sr.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
            sr.Texture2D.MipLevels = 1;
            if (FAILED(g_render.device->CreateShaderResourceView(scene, &sr, &g_render.sourceSrv))) return nullptr;
        } else {
            g_render.sourceSamples = 1;
            D3D11_TEXTURE2D_DESC d = desc;d.Format = ViewFormat(desc.Format);d.SampleDesc = {1, 0};
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;d.MiscFlags = d.CPUAccessFlags = 0;d.Usage = D3D11_USAGE_DEFAULT;d.MipLevels = d.ArraySize = 1;
            sr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels = 1;
            if (FAILED(g_render.device->CreateTexture2D(&d, nullptr, &g_render.resolved)) ||
                FAILED(g_render.device->CreateShaderResourceView(g_render.resolved.Get(), &sr, &g_render.sourceSrv))) return nullptr;
        }
        MvmDebugLog_LinefAlways("world.autoexposure", "scene %ux%u fmt=%d samples=%u path=%s",
            desc.Width, desc.Height, int(desc.Format), desc.SampleDesc.Count, direct ? "direct" : "resolve");
    }
    if (g_render.resolved) {
        if (desc.SampleDesc.Count > 1) ctx->ResolveSubresource(g_render.resolved.Get(), 0, scene, 0, ViewFormat(desc.Format));
        else ctx->CopyResource(g_render.resolved.Get(), scene);
    }
    return g_render.sourceSrv.Get();
}

} // namespace

void AutoExposure_RenderThread(ID3D11DeviceContext* immediate) {
    if (!immediate || !g_enabled.load(std::memory_order_relaxed)) return;
    ComPtr<ID3D11Device> device;immediate->GetDevice(&device);
    if (!device || !Initialize(device.Get())) return;

    // Collect the meter written kSlots-1 frames ago; never wait on the GPU.
    const unsigned slot = g_render.frame % kSlots, readSlot = (g_render.frame + 1) % kSlots;
    if (g_render.written[readSlot]) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(immediate->Map(g_render.staging[readSlot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            const float* v = static_cast<const float*>(mapped.pData);
            if (v[1] > 0 && std::isfinite(v[0])) {
                g_metered.store(v[0], std::memory_order_relaxed);
                g_meterCount.fetch_add(1, std::memory_order_relaxed);
            }
            immediate->Unmap(g_render.staging[readSlot].Get(), 0);
            g_render.written[readSlot] = false;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    ComPtr<ID3D11Texture2D> scene;scene.Attach(RenderHdrCapture_AcquireSceneTarget(immediate, &desc));
    if (!scene) return;
    // A resolved copy costs a full-screen resolve; metering every other frame is plenty.
    if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 && (g_render.frame & 1)) { ++g_render.frame; return; }
    ID3D11ShaderResourceView* view = SceneView(immediate, scene.Get(), desc);
    unsigned shader = 0;while (shader < 4 && (1u << shader) != g_render.sourceSamples) ++shader;
    if (!view || shader == 4) return;

    const float size[4]{float(desc.Width), float(desc.Height), desc.Height ? float(desc.Width) / desc.Height : 1.f, 0};
    auto ctx = immediate;
    // Immediate context with only the compute slots used here saved and restored (as the AO pass).
    ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount = 256;
    ctx->CSGetShader(previousShader.GetAddressOf(), previousInstances, &previousInstanceCount);
    ID3D11Buffer* previousConstants = nullptr;ctx->CSGetConstantBuffers(0, 1, &previousConstants);
    ID3D11ShaderResourceView* previousView = nullptr;ctx->CSGetShaderResources(0, 1, &previousView);
    ID3D11UnorderedAccessView* previousUav = nullptr;ctx->CSGetUnorderedAccessViews(0, 1, &previousUav);
    ctx->UpdateSubresource(g_render.constants.Get(), 0, nullptr, size, 0, 0);
    auto cb = g_render.constants.Get();ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &view);
    auto uav = g_render.resultUav.Get();ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(g_render.meter[shader].Get(), nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    uav = nullptr;ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ID3D11ShaderResourceView* noView = nullptr;ctx->CSSetShaderResources(0, 1, &noView);
    UINT keepCounter = ~0u;ctx->CSSetUnorderedAccessViews(0, 1, &previousUav, &keepCounter);
    ctx->CSSetShaderResources(0, 1, &previousView);ctx->CSSetConstantBuffers(0, 1, &previousConstants);
    ctx->CSSetShader(previousShader.Get(), previousInstances, previousInstanceCount);
    for (UINT i = 0; i < previousInstanceCount; ++i) if (previousInstances[i]) previousInstances[i]->Release();
    if (previousView) previousView->Release();
    if (previousConstants) previousConstants->Release();
    if (previousUav) previousUav->Release();

    ctx->CopyResource(g_render.staging[slot].Get(), g_render.result.Get());
    g_render.written[slot] = true;
    ++g_render.frame;
}

bool AutoExposure_MainThread(float& outScale) {
    LARGE_INTEGER now{}, frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    double dt = g_lastCounter.QuadPart ? double(now.QuadPart - g_lastCounter.QuadPart) / double(frequency.QuadPart) : 0;
    g_lastCounter = now;
    // A render advances the demo by a fixed step per frame however long the frame takes to
    // capture; adapt on that clock so an export matches what playback would show.
    double demoTime = 0;
    const bool haveDemoTime = g_MirvTime.GetCurrentDemoTime(demoTime);
    if (Render_IsJobActive() && haveDemoTime && g_lastDemoTime >= 0) dt = demoTime - g_lastDemoTime;
    g_lastDemoTime = haveDemoTime ? demoTime : -1;
    if (!g_enabled.load(std::memory_order_relaxed)) { g_currentLog2 = NAN; return false; }
    if (!g_meterCount.load(std::memory_order_relaxed)) return false;

    const float target = std::log2(AutoExposureMath::TargetScale(g_metered.load(std::memory_order_relaxed), kKey,
        g_compensation.load(std::memory_order_relaxed), kMinScale, kMaxScale));
    // First meter, a seek or a long stall: start at the target instead of sweeping in from the past.
    if (!std::isfinite(g_currentLog2) || dt < 0 || dt > .5) g_currentLog2 = target;
    // dt 0: a second call in the same frame (scene keys re-run NightMode) holds the value.
    else if (dt > 0) g_currentLog2 = AutoExposureMath::Adapt(g_currentLog2, target, float(dt), g_speed.load(std::memory_order_relaxed));
    g_appliedScale = std::exp2(g_currentLog2);
    outScale = g_appliedScale;
    if (MvmDebugLog_Active()) {
        static ULONGLONG last = 0;const auto t = GetTickCount64();
        if (t - last >= 1000) { last = t;
            MvmDebugLog_LinefAlways("world.autoexposure", "meteredLog2=%.3f target=%.3f scale=%.3f speed=%.2f comp=%.2f dt=%.4f render=%d",
                g_metered.load(), std::exp2(target), g_appliedScale, g_speed.load(), g_compensation.load(), dt, int(Render_IsJobActive())); }
    }
    return true;
}

bool AutoExposure_Enabled() { return g_enabled.load(); }
void AutoExposure_SetEnabled(bool enabled) {
    g_enabled.store(enabled);
    if (!enabled) { g_currentLog2 = NAN; g_appliedScale = 0; g_meterCount.store(0); }
}
float AutoExposure_Speed() { return g_speed.load(); }
void AutoExposure_SetSpeed(float v) {
    if (!std::isfinite(v)) return;
    g_speed.store((std::min)(kAutoExposureSpeedMax, (std::max)(kAutoExposureSpeedMin, v)));
}
float AutoExposure_Compensation() { return g_compensation.load(); }
void AutoExposure_SetCompensation(float v) {
    if (!std::isfinite(v)) return;
    g_compensation.store((std::min)(kAutoExposureCompensationMax, (std::max)(-kAutoExposureCompensationMax, v)));
}
float AutoExposure_CurrentScale() { return g_appliedScale; }
bool AutoExposure_MeteredLog2(float& out) {
    if (!g_meterCount.load()) return false;
    out = g_metered.load();return true;
}

} // namespace Filmmaker
