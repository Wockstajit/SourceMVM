#define NOMINMAX
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentRender.h"
#include "EnvironmentViewmodel.h"
#include "EnvironmentNativeMaterial.h"
#include "../Render/RenderPrivateTextureScope.h"
#include "EnvironmentNativeRain.h"
#include "EnvironmentWeatherMath.h"
#include "EnvironmentShaders.h"
#include "EnvironmentShaderPrograms.h"
#include "EnvironmentShadowMaps.h"
#include "EnvironmentAmbientOcclusion.h"
#include "EnvironmentSunVisibility.h"
#include "../Relight/RelightLightEditor.h"
#include "EnvironmentGi.h"
#include "EnvironmentStorm.h"
#include "EnvironmentRayProbe.h"
#include "../Debug/MvmDebugLog.h"
#include "../Debug/GpuPassProfiler.h"
#include <optional>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <cstring>
#include <future>
#include <chrono>
#include <atomic>
#include <thread>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};
Float4 Pack(Vec3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct Constants {
    Matrix vp, inverse;
    Float4 camera, dimensions, depthRange, fog, tint, weatherCenter, weatherParams, weatherOffset,
        weatherColor, weatherVelocity, right, up, lighting;
    Float4 lightPosition[16], lightColor[16], lightDirection[16];
    Float4 lightRight[16], lightUp[16], lightStudio[16], lightPatternTime[16];
    decltype(Climate::values) climate;
    Float4 climateClock, volumeDimensions;
    Float4 stormSource, stormPoints[kStormPointCount], stormLinks[kStormPointCount];
    // Coating actors: the players the world coatings skip. Must match COATING_ACTORS in the shader
    // defines and Frame::kMaxActors -- all three are the same number.
    Float4 coatingActors[12];
    Float4 coatingActorCount;
    Float4 smokeInfo; // x = smoke absorbance bound, yz = its size, w = debug view; matches the shader
    Float4 smokeVolumes[8]; // xyz centre, w radius; Frame::kMaxSmokes and SMOKE_VOLUMES
    Float4 smokeVolumeCount;
    Float4 viewmodelInfo;
    Float4 relightDebug; // x view mode, y record count, z replacement ceiling
    Float4 relightSun;      // xyz direction to the sun
    Float4 relightSunColor; // xyz sun radiance
    Float4 relightSky;      // xyz sky ambient
    // x != 0 false-colours which sun cascade each pixel's shadow lookup actually resolved to.
    // Which tier a surface lands in is invisible in a finished frame, and inferring it from mean
    // brightness got the wrong answer three times; this reads it off the shader directly.
    Float4 shadowDebug;
    // PrecipitationLattice(): {cell, across, layers, veil optical density}, {reach, reachUp, veil
    // distance, 0}; the region is the map's precipitation box (w = mapped).
    Float4 weatherLattice, weatherReach, weatherRegionLow, weatherRegionHigh;
    Float4 bakedLight; // x = Frame::bakedDaylight, y = 1 when it is valid
};
struct ObjectConstants {
    Matrix model, normal;
    Float4 color, materialParams, materialCoat, materialTextures;
};
static_assert(sizeof(Constants) % 16 == 0 && sizeof(ObjectConstants) % 16 == 0, "D3D constant alignment");
ID3D11ShaderResourceView *g_smokeView = nullptr; // render thread only; consumed by the next Render
unsigned g_smokeWidth = 0, g_smokeHeight = 0;
bool g_smokeDebug = false;
bool g_shadowCascadeDebug = false, g_rainExposureDebug = false;
struct GpuMesh {
    std::shared_ptr<const Mesh> source;
    ComPtr<ID3D11Buffer> buffer;
};
struct GpuMaterialImage {
    std::shared_ptr<const MaterialImage> source;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
};
using MaterialImageKey=std::pair<const MaterialImage*,bool>;
struct Resources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> screenVS, meshVS, weatherVS[2], stormVS, splashVS[2];
    ComPtr<ID3D11PixelShader> atmospherePS[4], atmospherePixelPS[4], meshPS[4], materialMeshPS[4], weatherPS[4], stormPS[4], splashPS[4], veilPS[4];
    ComPtr<ID3D11PixelShader> blitPS[2], volumePS[4], volumeBlurPS[4], compositePS[4], skyVolumePS[4];
    ComPtr<ID3D11PixelShader> atmosphereEdgePS;
    ComPtr<ID3D11Texture2D> atmosphereEdges;
    ComPtr<ID3D11ShaderResourceView> atmosphereEdgesSrv;
    ComPtr<ID3D11RenderTargetView> atmosphereEdgesRtv;
    ComPtr<ID3D11Texture2D> volume, volumeBlur, skyVolume, skyVolumeBlur;
    ComPtr<ID3D11ShaderResourceView> volumeSrv, volumeBlurSrv, skyVolumeSrv, skyVolumeBlurSrv;
    ComPtr<ID3D11RenderTargetView> volumeRtv, volumeBlurRtv, skyVolumeRtv, skyVolumeBlurRtv;
    ComPtr<ID3D11Texture2D> volumeGeometry;
    ComPtr<ID3D11ShaderResourceView> volumeGeometrySrv;
    ComPtr<ID3D11RenderTargetView> volumeGeometryRtv;
    UINT volumeWidth = 0, volumeHeight = 0;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants, objectConstants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth[2], noDepth;
    ComPtr<ID3D11BlendState> opaque, alpha;
    ComPtr<ID3D11Texture2D> sceneDepth, propDepth, color, working, sourceColor;
    ComPtr<ID3D11RenderTargetView> workingRtv;
    ComPtr<ID3D11Resource> displayResource;
    ComPtr<ID3D11RenderTargetView> displayRtv;
    D3D11_RENDER_TARGET_VIEW_DESC displayDesc{};
    ComPtr<ID3D11ShaderResourceView> sceneSrv, propSrv, colorSrv, sourceColorSrv;
    ComPtr<ID3D11DepthStencilView> propDsv;
    D3D11_TEXTURE2D_DESC sourceDesc{}, targetDesc{};
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    std::map<const Mesh *, GpuMesh> meshes;
    std::map<MaterialImageKey,GpuMaterialImage> materialImages;
    ComPtr<ID3D11SamplerState> materialSampler;
    ComPtr<ID3D11Query> timingStart, timingEnd, timingDisjoint, timingWeather[3], timingStages[2];
    bool timingPending = false, timingWeatherUsed = false;
    unsigned timingFrame = 0;
    Float4 timingSettings;
    bool ready = false;
} g, gHdr;
struct ResourceScope {
    bool hdr;
    explicit ResourceScope(bool useHdr) : hdr(useHdr) {
        if (hdr)
            std::swap(g, gHdr);
    }
    ~ResourceScope() {
        if (hdr)
            std::swap(g, gHdr);
    }
};
bool UploadMaterialImage(ID3D11DeviceContext *ctx,const std::shared_ptr<const MaterialImage>& image,bool srgb){
    if(!image)return true;MaterialImageKey key{image.get(),srgb};if(g.materialImages.count(key))return true;
    if(!ValidMaterialImage(*image))return false;
    GpuMaterialImage gpu;gpu.source=image;D3D11_TEXTURE2D_DESC d{};d.Width=image->width;d.Height=image->height;
    d.ArraySize=1;d.SampleDesc.Count=1;d.Format=srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if(FAILED(g.device->CreateTexture2D(&d,nullptr,&gpu.texture))||FAILED(g.device->CreateShaderResourceView(gpu.texture.Get(),nullptr,&gpu.view)))return false;
    ctx->UpdateSubresource(gpu.texture.Get(),0,nullptr,image->rgba.data(),image->width*4,0);ctx->GenerateMips(gpu.view.Get());
    MvmDebugLog_LinefAlways("environment.material","uploaded=1 width=%u height=%u srgb=%d",image->width,image->height,int(srgb));
    g.materialImages.emplace(key,std::move(gpu));return true;
}
bool CompileBytecode(const char *entry, const char *profile, int ms, ComPtr<ID3DBlob> &code, int colorMs = 0) {
    static HMODULE module = EnvironmentShaderCompiler();
    auto compile = module ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    if (!compile) {
        SetRenderStatus("Windows shader compiler unavailable");
        return false;
    }
    D3D_SHADER_MACRO macros[] = {
        {"DEPTH_MS", ms ? "1" : "0"}, {"COLOR_MS", colorMs ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const auto& source=EnvironmentShaderSource();
    bool cacheHit=false,bundledHit=false;
    const ULONGLONG started = GetTickCount64();
    MvmDebugLog_LinefAlways("environment.shader", "begin entry=%s depthMs=%d colorMs=%d compiler=%p", entry, ms, colorMs, module);
    HRESULT hr = CompileEnvironmentShader(source.data(), source.size(), "EnvironmentShaders", macros, entry, profile,
                         EnvironmentShaderFlags, &code, &errors, &cacheHit,nullptr,&bundledHit);
    MvmDebugLog_LinefAlways("environment.shader", "end entry=%s depthMs=%d colorMs=%d ms=%llu hr=%08x cached=%d bundled=%d", entry, ms, colorMs, GetTickCount64()-started, unsigned(hr), int(cacheHit),int(bundledHit));
    if (FAILED(hr)) {
        MvmDebugLog_LinefAlways("environment.shader", "entry=%s hr=%08x error=%s", entry, unsigned(hr),
                                errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        SetRenderStatus("Environment shader compilation failed; see mvm_debug");
        return false;
    }
    return true;
}
// The worker owns bytecode only. D3D resources and the scene remain on the render thread.
// Keep successful bytecode in memory as well as the disk cache, so read-only caches cannot
// force the render thread to compile again, and resizing never recompiles these shaders.
struct PreparedShader {
    std::string entry, profile;
    int ms = 0, colorMs = 0;
    ComPtr<ID3DBlob> code;
};
std::vector<PreparedShader> preparedShaders;
std::future<std::vector<PreparedShader>> pendingShaders;
bool devicePending = false;
bool shaderPreparationFailed = false;
std::atomic<bool> rendererReady{false};
std::vector<PreparedShader> PrepareShaders() {
    const ULONGLONG started=GetTickCount64();
    const auto requests=EnvironmentShaderRequests();

    // D3DCompile is thread-safe. Four bounded workers cut a cold preparation from minutes to the
    // duration of the slowest handful of variants without letting all jobs starve the game.
    std::vector<PreparedShader> result(requests.size());
    std::atomic<size_t> next{0},completed{0};
    const unsigned workers=(std::min)(4u,(std::max)(1u,std::thread::hardware_concurrency()));
    std::vector<std::future<void>> jobs;
    for(unsigned worker=0;worker<workers;++worker)jobs.push_back(std::async(std::launch::async,[&]{
        for(;;){
            size_t i=next.fetch_add(1);if(i>=requests.size())break;
            const auto request=requests[i];PreparedShader shader{request.entry,request.profile,request.ms,request.cm};
            if(!CompileBytecode(request.entry,request.profile,request.ms,shader.code,request.cm))
                throw std::runtime_error("Environment shader preparation failed");
            result[i]=std::move(shader);
            size_t done=completed.fetch_add(1)+1;
            std::string status="Preparing environment shaders ("+std::to_string(done)+"/"+std::to_string(requests.size())+")";
            SetRenderStatus(status.c_str());
        }
    }));
    for(auto &job:jobs)job.get();
    MvmDebugLog_LinefAlways("environment.shader", "batch ready count=%zu workers=%u ms=%llu",
                            result.size(),workers,GetTickCount64()-started);
    return result;
}
bool Compile(const char *entry, const char *profile, int ms, ComPtr<ID3DBlob> &code, int colorMs = 0) {
    for (const auto &shader : preparedShaders)
        if (shader.entry == entry && shader.profile == profile && shader.ms == ms && shader.colorMs == colorMs) {
            code = shader.code;
            return true;
        }
    // Standalone synchronous initialization has no prepared set. Runtime initialization does:
    // an incomplete set must fail visibly instead of running D3DCompile on the render thread.
    if (!preparedShaders.empty()) {
        SetRenderStatus("Environment shader missing from prepared set; see mvm_debug");
        MvmDebugLog_LinefAlways("environment.shader",
            "missing prepared entry=%s depthMs=%d colorMs=%d; render-thread compile blocked",
            entry, ms, colorMs);
        return false;
    }
    return CompileBytecode(entry, profile, ms, code, colorMs);
}
bool Buffer(UINT size, UINT bind, ComPtr<ID3D11Buffer> &out, const void *data = nullptr) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    return SUCCEEDED(g.device->CreateBuffer(&d, data ? &initial : nullptr, &out));
}
bool ColorTarget(const D3D11_TEXTURE2D_DESC &target, DXGI_FORMAT format) {
    if (g.color && g.workingRtv && g.propSrv && g.sourceColorSrv && g.targetDesc.Width == target.Width &&
        g.targetDesc.Height == target.Height && g.targetDesc.SampleDesc.Count == target.SampleDesc.Count &&
        g.colorFormat == format)
        return true;
    g.color.Reset();
    g.colorSrv.Reset();
    g.working.Reset();
    g.workingRtv.Reset();
    g.propDepth.Reset();
    g.propDsv.Reset();
    g.propSrv.Reset();
    g.sourceColor.Reset();
    g.sourceColorSrv.Reset();
    auto raw = target;
    raw.Usage = D3D11_USAGE_DEFAULT;
    raw.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    raw.CPUAccessFlags = 0;
    raw.MiscFlags = 0;
    raw.Format = format;
    if (FAILED(g.device->CreateTexture2D(&raw, nullptr, &g.sourceColor)) ||
        FAILED(g.device->CreateShaderResourceView(g.sourceColor.Get(), nullptr, &g.sourceColorSrv)))
        return false;
    auto d = target;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc = target.SampleDesc;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.color)) ||
        FAILED(g.device->CreateShaderResourceView(g.color.Get(), nullptr, &g.colorSrv)))
        return false;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.working)) ||
        FAILED(g.device->CreateRenderTargetView(g.working.Get(), nullptr, &g.workingRtv)))
        return false;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};
    ds.Format = DXGI_FORMAT_D32_FLOAT;
    ds.ViewDimension =
        target.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
    sr.Format = DXGI_FORMAT_R32_FLOAT;
    sr.ViewDimension =
        target.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sr.Texture2D.MipLevels = 1;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.propDepth)) ||
        FAILED(g.device->CreateDepthStencilView(g.propDepth.Get(), &ds, &g.propDsv)) ||
        FAILED(g.device->CreateShaderResourceView(g.propDepth.Get(), &sr, &g.propSrv)))
        return false;
    g.targetDesc = target;
    g.colorFormat = format;
    return true;
}
bool AtmosphereEdgeTarget(UINT width,UINT height) {
    if(g.atmosphereEdges){
        D3D11_TEXTURE2D_DESC current{};g.atmosphereEdges->GetDesc(&current);
        if(current.Width==width&&current.Height==height)return true;
    }
    g.atmosphereEdges.Reset();g.atmosphereEdgesSrv.Reset();g.atmosphereEdgesRtv.Reset();
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_R8_UINT;d.SampleDesc.Count=1;
    d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(g.device->CreateTexture2D(&d,nullptr,&g.atmosphereEdges))||
       FAILED(g.device->CreateShaderResourceView(g.atmosphereEdges.Get(),nullptr,&g.atmosphereEdgesSrv))||
       FAILED(g.device->CreateRenderTargetView(g.atmosphereEdges.Get(),nullptr,&g.atmosphereEdgesRtv))){
        g.atmosphereEdges.Reset();g.atmosphereEdgesSrv.Reset();g.atmosphereEdgesRtv.Reset();
        return false;
    }
    const char name[]="SourceMVM atmosphere MSAA edges";
    g.atmosphereEdges->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(name)-1,name);
    return true;
}
bool VolumeTarget(UINT width, UINT height) {
    if (g.volume && g.volumeBlur && g.skyVolume && g.skyVolumeBlur && g.volumeGeometry && g.volumeWidth == width && g.volumeHeight == height)
        return true;
    g.volume.Reset();
    g.volumeSrv.Reset();
    g.volumeRtv.Reset();
    g.volumeBlur.Reset();
    g.volumeBlurSrv.Reset();
    g.volumeBlurRtv.Reset();
    g.skyVolume.Reset();g.skyVolumeSrv.Reset();g.skyVolumeRtv.Reset();
    g.skyVolumeBlur.Reset();g.skyVolumeBlurSrv.Reset();g.skyVolumeBlurRtv.Reset();
    g.volumeGeometry.Reset();g.volumeGeometrySrv.Reset();g.volumeGeometryRtv.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.volume)) ||
        FAILED(g.device->CreateShaderResourceView(g.volume.Get(), nullptr, &g.volumeSrv)) ||
        FAILED(g.device->CreateRenderTargetView(g.volume.Get(), nullptr, &g.volumeRtv)) ||
        FAILED(g.device->CreateTexture2D(&d, nullptr, &g.volumeBlur)) ||
        FAILED(g.device->CreateShaderResourceView(g.volumeBlur.Get(), nullptr, &g.volumeBlurSrv)) ||
        FAILED(g.device->CreateRenderTargetView(g.volumeBlur.Get(), nullptr, &g.volumeBlurRtv)) ||
        FAILED(g.device->CreateTexture2D(&d, nullptr, &g.skyVolume)) ||
        FAILED(g.device->CreateShaderResourceView(g.skyVolume.Get(), nullptr, &g.skyVolumeSrv)) ||
        FAILED(g.device->CreateRenderTargetView(g.skyVolume.Get(), nullptr, &g.skyVolumeRtv)) ||
        FAILED(g.device->CreateTexture2D(&d, nullptr, &g.skyVolumeBlur)) ||
        FAILED(g.device->CreateShaderResourceView(g.skyVolumeBlur.Get(), nullptr, &g.skyVolumeBlurSrv)) ||
        FAILED(g.device->CreateRenderTargetView(g.skyVolumeBlur.Get(), nullptr, &g.skyVolumeBlurRtv)))
        return false;
    // One nearest-depth classification/unprojection per coarse texel, reused by
    // the 25-tap blur and all full-resolution MSAA composite samples.
    d.Format=DXGI_FORMAT_R32G32_FLOAT;
    if(FAILED(g.device->CreateTexture2D(&d,nullptr,&g.volumeGeometry))||
       FAILED(g.device->CreateShaderResourceView(g.volumeGeometry.Get(),nullptr,&g.volumeGeometrySrv))||
       FAILED(g.device->CreateRenderTargetView(g.volumeGeometry.Get(),nullptr,&g.volumeGeometryRtv)))return false;
    g.volumeWidth = width;
    g.volumeHeight = height;
    return true;
}
bool DepthTarget(const D3D11_TEXTURE2D_DESC &source) {
    if (g.sceneDepth && g.sourceDesc.Width == source.Width && g.sourceDesc.Height == source.Height &&
        g.sourceDesc.Format == source.Format && g.sourceDesc.SampleDesc.Count == source.SampleDesc.Count)
        return true;
    g.sceneDepth.Reset();
    g.sceneSrv.Reset();
    auto d = source;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    DXGI_FORMAT format;
    switch (d.Format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        format = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        d.Format = DXGI_FORMAT_R24G8_TYPELESS;
        format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        d.Format = DXGI_FORMAT_R16_TYPELESS;
        format = DXGI_FORMAT_R16_UNORM;
        break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    default:
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
    sr.Format = format;
    sr.ViewDimension =
        d.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    sr.Texture2D.MipLevels = 1;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.sceneDepth)) ||
        FAILED(g.device->CreateShaderResourceView(g.sceneDepth.Get(), &sr, &g.sceneSrv)))
        return false;
    g.sourceDesc = source;
    return true;
}
} // namespace
void EndDevice() {
    ResetViewmodel();
    ResetNativeMaterial();
    rendererReady.store(false, std::memory_order_release);
    devicePending = false;
    EndShadowMaps();
    EndAmbientOcclusion();
    EndSunVisibility();
    Relight::EndLightEditor();
    EndGi();
    g = Resources{};
    gHdr = Resources{};
    SetRenderStatus("Renderer device released");
}
void BeginDevice(ID3D11Device *device, bool background) {
    if (device && g.device.Get() == device && (g.ready || devicePending))
        return; // swapchain size changed, not the D3D device

    EndDevice();
    if (!device)
        return;
    g.device = device;
    if (background && preparedShaders.empty()) {
        if (shaderPreparationFailed)
            return;
        devicePending = true;
        if (!pendingShaders.valid())
            pendingShaders = std::async(std::launch::async, PrepareShaders);
        SetRenderStatus("Preparing environment shaders in background...");
        return;
    }
    const ULONGLONG initializeStarted=GetTickCount64();
    BeginShadowMaps(device);
    BeginAmbientOcclusion(device);
    BeginSunVisibility(device);
    BeginGi(device);
    if (FAILED(device->CreateDeferredContext(0, &g.context))) {
        SetRenderStatus("Could not create environment render context");
        return;
    }
    ComPtr<ID3DBlob> code;
    if (!Compile("ScreenVS", "vs_5_0", 0, code) ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                          &g.screenVS)))
        return;
    for (int cm = 0; cm < 2; ++cm) {
        code.Reset();
        if (!Compile("BlitPS", "ps_5_0", 0, code, cm) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                             &g.blitPS[cm])))
            return;
    }
    for (int cm = 0; cm < 2; ++cm) {
        code.Reset();
        if (!Compile("WeatherVS", "vs_5_0", 0, code, cm) ||
            FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                              &g.weatherVS[cm])))
            return;
    }
    code.Reset();
    if (!Compile("StormVS", "vs_5_0", 0, code) ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&g.stormVS)))return;
    for (int sms = 0; sms < 2; ++sms) {
        code.Reset();
        if (!Compile("SplashVS", "vs_5_0", sms, code) ||
            FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.splashVS[sms])))
            return;
    }
    code.Reset();
    if (!Compile("MeshVS", "vs_5_0", 0, code) ||
        FAILED(
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.meshVS)))
        return;
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if (FAILED(
            device->CreateInputLayout(layout, 3, code->GetBufferPointer(), code->GetBufferSize(), &g.layout)))
        return;
    for (int ms = 0; ms < 2; ++ms)
        for (int cm = 0; cm < 2; ++cm) {
            ComPtr<ID3D11PixelShader> *outputs[] = {&g.atmospherePS[ms + cm * 2], &g.atmospherePixelPS[ms + cm * 2], &g.meshPS[ms + cm * 2], &g.materialMeshPS[ms + cm * 2],
                                                    &g.weatherPS[ms + cm * 2], &g.volumePS[ms + cm * 2],
                                                    &g.compositePS[ms + cm * 2], &g.stormPS[ms + cm * 2], &g.splashPS[ms + cm * 2], &g.veilPS[ms + cm * 2],
                                                    &g.volumeBlurPS[ms + cm * 2],&g.skyVolumePS[ms + cm * 2]};
            static_assert(sizeof(outputs) / sizeof(outputs[0]) == EnvironmentPixelShaderCount,
                          "Every environment pixel shader needs an output");
            for (size_t k = 0; k < EnvironmentPixelShaderCount; ++k) {
                code.Reset();
                if (!Compile(EnvironmentPixelShaderEntries[k], "ps_5_0", ms, code, cm) ||
                    FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                     outputs[k]->GetAddressOf())))
                    return;
            }
        }
    code.Reset();
    if(!Compile("AtmosphereEdgePS","ps_5_0",1,code,1)||
       FAILED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&g.atmosphereEdgePS)))return;
    if (!Buffer(sizeof(Constants), D3D11_BIND_CONSTANT_BUFFER, g.constants) ||
        !Buffer(sizeof(ObjectConstants), D3D11_BIND_CONSTANT_BUFFER, g.objectConstants))
        return;
    D3D11_SAMPLER_DESC materialSampler{};materialSampler.Filter=D3D11_FILTER_ANISOTROPIC;
    materialSampler.AddressU=materialSampler.AddressV=materialSampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    materialSampler.MaxAnisotropy=8;materialSampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(device->CreateSamplerState(&materialSampler,&g.materialSampler)))return;
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    r.MultisampleEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&r, &g.raster)))
        return;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&d, &g.depth[0])))
        return;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&d, &g.depth[1])))
        return;
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&d, &g.noDepth)))
        return;
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&b, &g.opaque)))
        return;
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(device->CreateBlendState(&b, &g.alpha)))
        return;
    g.ready = true;
    rendererReady.store(true, std::memory_order_release);
    gHdr = g; // share immutable shaders/states; HDR and display allocate independent frame surfaces.
    SetRenderStatus("Ready");
    MvmDebugLog_LinefAlways("environment.shader", "device ready ms=%llu",GetTickCount64()-initializeStarted);
}
bool RendererReady() { return rendererReady.load(std::memory_order_acquire); }
bool PumpDevicePreparation() {
    if (devicePending && pendingShaders.valid() &&
        pendingShaders.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        devicePending = false;
        try {
            preparedShaders = pendingShaders.get();
            // Own a reference across EndDevice inside BeginDevice.
            auto device = g.device;
            BeginDevice(device.Get());
        } catch (const std::exception &e) {
            shaderPreparationFailed = true;
            SetRenderStatus("Environment shader preparation failed; see mvm_debug");
            MvmDebugLog_LinefAlways("environment.shader", "background preparation failed: %s", e.what());
        }
    }
    return g.ready;
}
void SetSmokeDebugView(bool on) { g_smokeDebug = on; }
bool SmokeDebugView() { return g_smokeDebug; }
void SetShadowCascadeView(bool on) { g_shadowCascadeDebug = on; }
bool ShadowCascadeView() { return g_shadowCascadeDebug; }
void SetRainExposureView(bool on) { g_rainExposureDebug = on; }
bool RainExposureView() { return g_rainExposureDebug; }
void SetSmokeAbsorbance(ID3D11ShaderResourceView *view, unsigned width, unsigned height) {
    const bool usable = view && width && height;
    g_smokeView = usable ? view : nullptr;
    g_smokeWidth = usable ? width : 0;
    g_smokeHeight = usable ? height : 0;
}
bool Render(ID3D11DeviceContext *immediate, ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth,
            const Frame &input, float depthMin, float depthMax, ID3D11Texture2D *nativeHdrSource) {
    RenderPrivateTextureScope privateTextures;
    Frame lightingFrame;
    if(input.nativeLightingPass){
        lightingFrame=input;Climate neutral;
        for(int i=0;i<ClimateCount;++i)if(std::string(ClimateSpecs()[i].page)=="lighting")neutral.values[i]=input.scene.climate.values[i];
        for(auto field:{CL_enabled,CL_sunAzimuth,CL_sunElevation,CL_sunPower,CL_shadowRange})neutral[field]=input.scene.climate[field];
        lightingFrame.scene.climate=neutral;lightingFrame.scene.exposure=1;lightingFrame.scene.tint={1,1,1};lightingFrame.scene.weather.type=0;
    }
    if(input.proof.enabled||input.relight.enabled){
        if(!input.nativeLightingPass)lightingFrame=input;
        lightingFrame.scene.climate[CL_relighting]=0;
    }
    const Frame& baseFrame=(input.nativeLightingPass||input.proof.enabled||input.relight.enabled)?lightingFrame:input;
    Frame stormFrame;
    const auto storm = EvaluateStorm(baseFrame.scene.climate, baseFrame.time);
    bool stormLighting = false;
    if (baseFrame.scene.climate[CL_storm] > .5f && baseFrame.scene.climate[CL_enabled] > .5f) {
        stormFrame = baseFrame;
        stormLighting = AddStormLight(stormFrame, storm);
    }
    const Frame &f = stormLighting ? stormFrame : baseFrame;
    if (MvmDebugLog_Active()) {
        static uint32_t lastEvent = ~0u;
        static int lastLit = -1;
        int lit = storm.flash > 0 ? (stormLighting ? 2 : 1) : 0;
        if (lastEvent != storm.event || lastLit != lit) {
            MvmDebugLog_Linef("environment.storm", "time=%.4f event=%u age=%.4f flash=%.3f source=(%.1f %.1f %.1f) shadowedEmitter=%d geometry=%d",
                input.time, storm.event, storm.age, storm.flash, storm.source.x, storm.source.y, storm.source.z, int(stormLighting), int(bool(input.occlusion)));
            lastEvent = storm.event; lastLit = lit;
        }
    }
    // "Off" has to mean off. The GI volumes, AO, shadow atlases and screen-sized targets stay
    // allocated after the last pass is switched off, and only a map change used to free them:
    // users reported the whole PC lagging after turning Environment on and off again (2026-10-13).
    // After kIdleReleaseFrames inactive frames, release everything through the device-loss path
    // (EndDevice). Switching back on rebuilds it the same way a new device does (BeginDevice).
    constexpr unsigned kIdleReleaseFrames = 300;
    static unsigned s_idleFrames = 0;
    static ComPtr<ID3D11Device> s_idleReleasedDevice;
    if (!f.active) {
        if (++s_idleFrames == kIdleReleaseFrames && (g.ready || devicePending) && g.device) {
            s_idleReleasedDevice = g.device;
            EndDevice();
            SetRenderStatus("Off (resources released)");
            MvmDebugLog_LinefAlways("environment.idle", "released renderer resources after %u inactive frames", kIdleReleaseFrames);
        }
        g.materialImages.clear();gHdr.materialImages.clear();
        return false;
    }
    s_idleFrames = 0;
    if (s_idleReleasedDevice) {
        auto device = std::move(s_idleReleasedDevice);
        // A device change while released already began the new device: never rebuild on the old one.
        if (!g.device && !devicePending) {
            MvmDebugLog_LinefAlways("environment.idle", "rebuilding renderer resources (switched back on)");
            BeginDevice(device.Get(), true);
        }
    }
    PumpHardwareRayProbe(g.device.Get(),f);
    if (!PumpDevicePreparation())
        return false;
    ResourceScope resourceScope(f.hdr);
    if (!g.ready || !immediate || !target) {
        SetRenderStatus("Renderer unavailable");
        return false;
    }
    if (!depth) {
        SetRenderStatus("Waiting for world depth");
        return false;
    }
    if (!std::isfinite(depthMin) || !std::isfinite(depthMax) || depthMin < 0 || depthMax > 1 ||
        depthMin >= depthMax) {
        SetRenderStatus("Waiting for valid world depth range");
        return false;
    }
    ComPtr<ID3D11Resource> targetResource, depthResource;
    target->GetResource(&targetResource);
    depth->GetResource(&depthResource);
    ComPtr<ID3D11Texture2D> targetTexture, depthTexture;
    if (FAILED(targetResource.As(&targetTexture)) || FAILED(depthResource.As(&depthTexture))) {
        SetRenderStatus("Unsupported render resources");
        return false;
    }
    D3D11_TEXTURE2D_DESC td{}, dd{};
    ProbeViewmodel(immediate,depthTexture.Get(),f.camera.x,f.camera.y,f.camera.z);
    targetTexture->GetDesc(&td);
    depthTexture->GetDesc(&dd);
    D3D11_RENDER_TARGET_VIEW_DESC rt{};
    target->GetDesc(&rt);
    // The native LDR surface already contains display-encoded pixels. Weapon
    // passes can leave either its UNORM or sRGB view bound; that binding must not
    // choose the transfer applied to our newly generated sky/cloud radiance.
    // Decode the scene and encode the final composition through consistent sRGB
    // views of typeless display surfaces. HDR and explicitly typed targets keep
    // their native interpretation.
    DXGI_FORMAT displayFormat = rt.Format;
    if (!f.hdr) {
        if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
            (rt.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rt.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
            displayFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        else if (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                 (rt.Format == DXGI_FORMAT_B8G8R8A8_UNORM || rt.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            displayFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        else if (td.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS &&
                 (rt.Format == DXGI_FORMAT_B8G8R8X8_UNORM || rt.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB))
            displayFormat = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }
    ID3D11RenderTargetView *displayTarget = target;
    if (displayFormat != rt.Format) {
        auto displayDesc = rt;
        displayDesc.Format = displayFormat;
        if (!g.displayRtv || g.displayResource.Get() != targetResource.Get() ||
            std::memcmp(&g.displayDesc, &displayDesc, sizeof(displayDesc)) != 0) {
            ComPtr<ID3D11RenderTargetView> displayView;
            if (FAILED(g.device->CreateRenderTargetView(targetTexture.Get(), &displayDesc, &displayView))) {
                SetRenderStatus("Could not create display color view");
                return false;
            }
            g.displayRtv = displayView;
            g.displayResource = targetResource;
            g.displayDesc = displayDesc;
        }
        displayTarget = g.displayRtv.Get();
    }
    static bool described = false;
    if (!described && MvmDebugLog_Active()) {
        described = true;
        MvmDebugLog_LinefAlways(
            "environment.surfaces",
            "target=%p %ux%u format=%d samples=%u array=%u depth=%p %ux%u format=%d samples=%u array=%u",
            target, td.Width, td.Height, int(td.Format), td.SampleDesc.Count, td.ArraySize, depth, dd.Width,
            dd.Height, int(dd.Format), dd.SampleDesc.Count, dd.ArraySize);
    }
    if (td.ArraySize != 1 || dd.ArraySize != 1) {
        SetRenderStatus("Array scene surfaces are unsupported");
        return false;
    }
    Constants c{};
    c.vp = f.hasWorldToClip ? f.worldToClip : ViewProjection(f.camera, f.angles, f.projection);
    if (!Inverse(c.vp, c.inverse)) {
        SetRenderStatus("Waiting for valid camera projection");
        return false;
    }
    bool reversed = Transform(f.projection, {0, 0, -10000000}).z < .5f;
    if (!ColorTarget(td, displayFormat) || !DepthTarget(dd)) {
        SetRenderStatus("Could not allocate scene/depth surfaces");
        return false;
    }
    const auto climate = EffectiveClimate(f.scene.climate);
    const bool volumes = climate[CL_clouds] > 0 || climate[CL_fogDensity] > 0;
    const int quality = f.exporting ? 2 : 1;
    // Live preview marches the clouds/fog at quarter resolution; renders keep half.
    const UINT divider = f.exporting ? 1 : 4;
    if (volumes && !VolumeTarget((td.Width + divider - 1) / divider, (td.Height + divider - 1) / divider)) {
        SetRenderStatus("Could not allocate volumetric surface");
        return false;
    }
    c.climate = climate.values;
    c.stormSource = Pack(storm.source, storm.flash);
    for (int i = 0; i < kStormPointCount; ++i) {
        c.stormPoints[i] = Pack(storm.points[i], storm.width[i]);
        c.stormLinks[i] = {float(storm.parent[i]), storm.gain[i], 0, 0};
    }
    c.climateClock = {float(std::fmod(f.time, 100000.0)),
                      float(std::fmod(f.time * climate[CL_windSpeed], 10000000.0)), 0, 0};
    c.volumeDimensions = {float(g.volumeWidth), float(g.volumeHeight),
                          float(ClimateSteps(climate, f.exporting)), f.hdr ? 1.f : 0.f};
    // Resolve all GPU meshes before recording any commands. Failure leaves the original image.
    std::set<MaterialImageKey> usedImages;
    for(const auto &o:f.scene.objects)if(o.enabled&&!o.IsLight()&&o.cameraVisible&&o.material.enabled){
        auto images=std::array<std::shared_ptr<const MaterialImage>,3>{o.material.albedoImage,o.material.normalImage,o.material.roughnessImage};
        for(int i=0;i<3;++i)if(images[i]){usedImages.insert({images[i].get(),i==0});if(!UploadMaterialImage(immediate,images[i],i==0)){SetRenderStatus("Could not upload material texture");return false;}}
    }
    // A GLB's own materials (one per part), used while no material is set on the object.
    for(const auto &o:f.scene.objects)if(o.enabled&&!o.IsLight()&&o.cameraVisible&&!o.material.enabled&&o.mesh)
        for(const auto &part:o.mesh->parts){
            auto images=std::array<std::shared_ptr<const MaterialImage>,3>{part.albedo,part.normal,part.roughnessMap};
            for(int i=0;i<3;++i)if(images[i]){usedImages.insert({images[i].get(),i==0});if(!UploadMaterialImage(immediate,images[i],i==0)){SetRenderStatus("Could not upload mesh texture");return false;}}
        }
    for(auto it=g.materialImages.begin();it!=g.materialImages.end();)if(!usedImages.count(it->first))it=g.materialImages.erase(it);else ++it;
    std::set<const Mesh *> used;
    for (auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight() && o.mesh) {
            used.insert(o.mesh.get());
            if (g.meshes.count(o.mesh.get()))
                continue;
            GpuMesh mesh;
            mesh.source = o.mesh;
            if (!Buffer(UINT(o.mesh->vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER, mesh.buffer,
                        o.mesh->vertices.data())) {
                SetRenderStatus("Could not upload prop mesh");
                return false;
            }
            g.meshes.emplace(o.mesh.get(), std::move(mesh));
        }
    for (auto it = g.meshes.begin(); it != g.meshes.end();) {
        if (!used.count(it->first))
            it = g.meshes.erase(it);
        else
            ++it;
    }
    c.camera = Pack(f.camera, reversed ? 1.f : 0.f);
    c.dimensions = {float(td.Width), float(td.Height), float(dd.Width), float(dd.Height)};
    c.depthRange = {depthMin, depthMax, 0, 0};
    c.fog = Pack(f.scene.fogColor, climate[CL_fogDensity]);
    c.tint = Pack(f.scene.tint, f.scene.exposure);
    // Publish the coating actors. Empty is the normal case (nobody on screen, or no coating on)
    // and leaves nothing excluded.
    {
        const int maxActors = int(sizeof(c.coatingActors) / sizeof(c.coatingActors[0]));
        int n = 0;
        for (const auto &a : f.actors) {
            if (n >= maxActors) break;
            c.coatingActors[n++] = {a.origin.x, a.origin.y, a.origin.z, a.radius};
        }
        for (int i = n; i < maxActors; ++i) c.coatingActors[i] = {0, 0, 0, 0};
        c.coatingActorCount = {float(n), 0, 0, 0};
    }
    // Set by the caller before every Render (the HDR grab renders twice per frame). The command
    // list below holds its own reference to the view.
    ID3D11ShaderResourceView *smokeView = g_smokeView;
    c.smokeInfo = {smokeView ? 1.f : 0.f, float(g_smokeWidth), float(g_smokeHeight), g_smokeDebug ? 1.f : 0.f};
    {
        int n = 0;
        for (const auto &s : f.smokes) {
            if (n >= Frame::kMaxSmokes) break;
            c.smokeVolumes[n++] = {s.center.x, s.center.y, s.center.z, s.radius};
        }
        for (int i = n; i < Frame::kMaxSmokes; ++i) c.smokeVolumes[i] = {0, 0, 0, 0};
        c.smokeVolumeCount = {float(n), 0, 0, 0};
    }
    const auto &w = f.scene.weather;
    Vec3 center = w.follow ? f.camera : w.center;
    c.weatherCenter = Pack(center, w.radius);
    c.weatherParams = {w.height, w.size, float(w.type), float(w.seed)};
    c.weatherOffset = {Wrap(f.time * w.windX, w.radius * 2), Wrap(f.time * w.windY, w.radius * 2),
                       Wrap(f.time * w.speed * (w.type >= 4 ? 1 : -1), w.height),
                       float(std::fmod(f.time, 628.3185307))};
    // Rain count controls lattice occupancy, including fractional final-drop coverage.
    // Rain count is lattice occupancy out of 3000; snow and ash use the same lattice with their own scale.
    const float occupancyScale = (w.type == 2 || w.type == 3) ? 6000.f : 3000.f;
    c.weatherColor = {w.color.x, w.color.y, w.color.z, std::clamp(float(w.count) / occupancyScale, 0.f, 1.f)};
    c.weatherVelocity = {w.windX, w.windY, w.speed * (w.type >= 4 ? 1.f : -1.f), w.follow ? 1.f : 0.f};
    // Map collision bounds only: the visual caster bounds also enclose the 3D skybox miniature.
    const auto precipitation = PrecipitationLattice(w.radius, c.weatherColor.w, bool(f.occlusion), f.occlusion ? f.occlusion->low : Vec3{},
                                                    f.occlusion ? f.occlusion->high : Vec3{}, w.height);
    // The veil's optical density is the particles' own: particles (one per cell) per unit volume
    // times the coverage each adds. Opacity is applied in the shader, where the particles apply it too.
    const bool precipitating = w.type >= 1 && w.type <= 3 && w.count > 0;
    const float veilDensity = precipitating
        ? PrecipitationCrossSection(w.type, w.size, climate[CL_streak]) /
              (precipitation.cell * precipitation.cell * precipitation.cell)
        : 0.f;
    c.weatherLattice = {precipitation.cell, float(precipitation.across), float(precipitation.layers), veilDensity};
    // Unmapped, the veil stops at about ten weather radii; mapped, at the map's own edge.
    c.weatherReach = {precipitation.reach, precipitation.reachUp, precipitation.mapped ? 16000.f : (std::max)(w.radius, 64.f) * 10, 0};
    c.weatherRegionLow = Pack(precipitation.low, precipitation.mapped ? 1.f : 0.f);
    c.weatherRegionHigh = Pack(precipitation.high, 0);
    Vec3 forward, right, up;
    CameraBasis(f.angles, forward, right, up);
    if (f.hasWorldToClip) {
        auto inverseView = Multiply(c.inverse, f.projection);
        right = Normalize(Transform(inverseView, {1, 0, 0}) - f.camera);
        up = Normalize(Transform(inverseView, {0, 1, 0}) - f.camera);
    }
    c.right = Pack(right);
    c.up = Pack(up);
    c.lighting.y = f.scene.ambient;
    int lightCount = 0;
    for (auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && lightCount < 16) {
            c.lightPosition[lightCount] = Pack(o.position, o.range);
            c.lightColor[lightCount] = Pack(o.color, o.intensity * StudioCue(o.studio, f.time));
            Vec3 dir, r, u;
            CameraBasis(o.angles, dir, r, u);
            c.lightDirection[lightCount] = Pack(dir, o.kind != "point" ? std::cos(Radians(o.cone * .5f)) : -1);
            c.lightRight[lightCount]=Pack(r,o.studio.width); c.lightUp[lightCount]=Pack(u,o.studio.height);
            c.lightStudio[lightCount]={o.kind=="area"?1.f:0.f,float(o.studio.pattern),float(o.studio.profile),o.studio.patternScale};
            c.lightPatternTime[lightCount].x=float(std::fmod(f.time*o.studio.rate+o.studio.phase,6283.185307));
            ++lightCount;
        }
    c.lighting.x = float(lightCount);
    if (g.timingPending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0, end = 0;
        if (immediate->GetData(g.timingDisjoint.Get(), &disjoint, sizeof(disjoint),
                               D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            immediate->GetData(g.timingStart.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                S_OK &&
            immediate->GetData(g.timingEnd.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                S_OK) {
            // Stage marks: after shadow/GI preparation, after the full-screen surface pass; and for
            // precipitation, before the particles, after them, after the veil.
            UINT64 stages[2]{}, marks[3]{};
            bool staged = g.timingWeatherUsed, passes = true;
            for (int k = 0; passes && k < 2; ++k)
                passes = immediate->GetData(g.timingStages[k].Get(), &stages[k], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            for (int k = 0; staged && k < 3; ++k)
                staged = immediate->GetData(g.timingWeather[k].Get(), &marks[k], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            auto ms = [&](UINT64 a, UINT64 b) { return b >= a ? double(b - a) * 1000 / disjoint.Frequency : -1.0; };
            if (!disjoint.Disjoint && disjoint.Frequency && end >= start)
                MvmDebugLog_Linef("environment.gpu",
                                  "ms=%.3f shadowsGi=%.3f surfaces=%.3f particles=%.3f veil=%.3f worldGi=%.0f bounce=%.2f reflections=%.2f hdr=%.0f",
                                  ms(start, end), passes ? ms(start, stages[0]) : 0.0, passes ? ms(stages[0], stages[1]) : 0.0,
                                  staged ? ms(marks[0], marks[1]) : 0.0, staged ? ms(marks[1], marks[2]) : 0.0,
                                  g.timingSettings.x, g.timingSettings.y, g.timingSettings.z, g.timingSettings.w);
            g.timingPending = false;
        }
    }
    bool measure = MvmDebugLog_Active() && !g.timingPending && ((++g.timingFrame % 30) == 1);
    if (measure && !g.timingDisjoint) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(g.device->CreateQuery(&q, &g.timingStart)) ||
            FAILED(g.device->CreateQuery(&q, &g.timingEnd)))
            measure = false;
        for (auto &mark : g.timingWeather)
            if (FAILED(g.device->CreateQuery(&q, &mark)))
                measure = false;
        for (auto &mark : g.timingStages)
            if (FAILED(g.device->CreateQuery(&q, &mark)))
                measure = false;
        q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(g.device->CreateQuery(&q, &g.timingDisjoint)))
            measure = false;
    }
    measure = measure && g.timingStart && g.timingEnd && g.timingDisjoint && g.timingStages[0] && g.timingStages[1] &&
              g.timingWeather[0] && g.timingWeather[1] && g.timingWeather[2];
    auto ctx = g.context.Get();
    ctx->ClearState();
    if (measure) {
        ctx->Begin(g.timingDisjoint.Get());
        ctx->End(g.timingStart.Get());
    }
    // The native pass/depth identity, not distance from the camera, identifies
    // first-person receivers. Prepare their environment-only shadow layer.
    {GPU_PASS_SCOPE(ctx, "composite: shadow maps");
     PrepareShadowMaps(ctx, f, BindViewmodel(ctx,depthTexture.Get()));}
    ctx->ClearState();
    // Which pixels are a person: every coating (snow, wet film, rain impacts) stays off them.
    const bool coating = climate[CL_snow] > 0 || climate[CL_wet] > 0 || climate[CL_puddles] > 0 ||
                         (w.type == 1 && w.count > 0);
    bool playerMask = false;
    if (coating) {GPU_PASS_SCOPE(ctx, "composite: player mask");
     playerMask = PreparePlayerMask(ctx, c.vp, td.Width, td.Height, reversed);}
    c.coatingActorCount.y = playerMask ? 1.f : 0.f;
    ctx->ClearState();
    {GPU_PASS_SCOPE(ctx, "composite: world GI");PrepareGi(ctx, f);}
    ctx->ClearState();
    if (measure) ctx->End(g.timingStages[0].Get());
    BindShadowMaps(ctx);
    BindGi(ctx);
    BindPlayerMask(ctx);
    ctx->CopyResource(g.sceneDepth.Get(), depthTexture.Get());
    ctx->CopyResource(g.sourceColor.Get(), targetTexture.Get());
    c.viewmodelInfo.x=BindViewmodel(ctx,depthTexture.Get())?1.f:0.f;
    c.viewmodelInfo.z=f.nativeLightingApplied?1.f:0.f;
    c.viewmodelInfo.w=(f.proof.enabled||f.relight.enabled)&&f.hdr?1.f:0.f;
    c.viewmodelInfo.y=!f.nativeLightingApplied&&BindNativeMaterial(ctx,targetTexture.Get(),depthTexture.Get(),f,nativeHdrSource)?1.f:0.f;
    c.relightDebug.x=float(f.relight.enabled?f.relight.view:0);
    c.relightDebug.y=float(BindRelightRecords(ctx));
    c.relightDebug.z=f.relight.enabled&&!f.relight.reference?f.relight.ceiling:0.f;
    c.relightDebug.w=float(RelightClusterMode());
    // The 3D skybox is a miniature standing outside the map, so only a directional source
    // reaches it meaningfully. Hand the owned sun and sky over for it.
    if(f.relight.enabled){
        const auto direction=Relight::SunDirection(f.relight.sunAngles);
        const auto radiance=Relight::SunRadiance(f.relight);
        const auto sky=Relight::SkyAmbient(f.relight,f.relight.indirect);
        c.relightSun={direction.x,direction.y,direction.z,f.relight.reference?1.f:0.f};
        c.relightSunColor={radiance.x,radiance.y,radiance.z,0};
        c.relightSky={sky.x,sky.y,sky.z,f.nativeCompressedHdr?1.f:0.f};
    }else{c.relightSun={};c.relightSunColor={};c.relightSky={};}
    c.relightSky.w=f.nativeCompressedHdr?1.f:0.f;
    c.relightSunColor.w=f.nativeSkyApplied?1.f:0.f;
    c.shadowDebug={g_shadowCascadeDebug?1.f:0.f,g_rainExposureDebug?1.f:0.f,0,0};
    c.bakedLight={(std::max)(0.f,f.bakedDaylight),f.bakedDaylight>=0?1.f:0.f,0,0};
    ID3D11RenderTargetView *workingTarget = g.workingRtv.Get();
    ctx->ClearDepthStencilView(g.propDsv.Get(), D3D11_CLEAR_DEPTH, reversed ? 0.f : 1.f, 0);
    c.lighting.z=td.SampleDesc.Count>1&&dd.SampleDesc.Count>1&&AtmosphereEdgeTarget(td.Width,td.Height)?1.f:0.f;
    if(c.lighting.z==0){
        g.atmosphereEdges.Reset();g.atmosphereEdgesSrv.Reset();g.atmosphereEdgesRtv.Reset();
    }
    ctx->UpdateSubresource(g.constants.Get(), 0, nullptr, &c, 0, 0);
    ID3D11Buffer *buffers[] = {g.constants.Get(), g.objectConstants.Get()};
    ctx->VSSetConstantBuffers(0, 2, buffers);
    ctx->PSSetConstantBuffers(0, 2, buffers);
    D3D11_VIEWPORT viewport{0, 0, float(td.Width), float(td.Height), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(g.raster.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    float blend[] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g.opaque.Get(), blend, 0xffffffff);
    int ms = dd.SampleDesc.Count > 1 ? 1 : 0;
    int variant = ms + (td.SampleDesc.Count > 1 ? 2 : 0);
    ID3D11ShaderResourceView *srvs[] = {nullptr, g.sceneSrv.Get(), nullptr, g.sourceColorSrv.Get()};
    ctx->PSSetShaderResources(0, 4, srvs);
    // Bound once: later passes only rebind slots 0-4 and 10-12, and ClearState releases it.
    ctx->PSSetShaderResources(14, 1, &smokeView);
    ctx->OMSetRenderTargets(1, &workingTarget, nullptr);
    ctx->OMSetDepthStencilState(g.noDepth.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g.screenVS.Get(), nullptr, 0);
    // The sample-frequency shader used to repeat this identical eight-depth test for every
    // covered sample. A single-sample R8 mask preserves its exact decision for both passes.
    std::optional<GpuProfile::Scope> atmosphereScope;
    {static const unsigned pass=GpuProfile::PassId("composite: atmosphere surfaces");atmosphereScope.emplace(ctx,pass);}
    if(c.lighting.z>.5f){
        auto edgeTarget=g.atmosphereEdgesRtv.Get();ctx->OMSetRenderTargets(1,&edgeTarget,nullptr);
        ctx->PSSetShader(g.atmosphereEdgePS.Get(),nullptr,0);ctx->Draw(3,0);
        ctx->OMSetRenderTargets(1,&workingTarget,nullptr);
        auto edges=g.atmosphereEdgesSrv.Get();ctx->PSSetShaderResources(34,1,&edges);
    }
    if(measure)MvmDebugLog_Linef("environment.msaa.edges",
        "cached=%d target=%ux%u depth=%ux%u samples=%u/%u native=%.0f receiver=%.0f material=%.0f relight=%.2f sun=%.2f worldGi=%.0f bounce=%.2f coating=%.2f/%.2f/%.2f rain=%.0f/%.2f debug=%.0f/%.0f/%.0f",
        int(c.lighting.z>.5f),td.Width,td.Height,dd.Width,dd.Height,td.SampleDesc.Count,dd.SampleDesc.Count,
        c.viewmodelInfo.z,c.viewmodelInfo.w,c.viewmodelInfo.y,climate[CL_relight],climate[CL_sunRelight],
        climate[CL_worldGi],climate[CL_bounce],climate[CL_snow],climate[CL_wet],climate[CL_puddles],
        c.weatherParams.z,c.weatherColor.w,c.shadowDebug.x,c.shadowDebug.y,c.relightDebug.x);
    // MSAA: interior pixels are shaded once (pixel rate), then only edge pixels per sample.
    if (td.SampleDesc.Count > 1) {
        ctx->PSSetShader(g.atmospherePixelPS[ms + 2].Get(), nullptr, 0);
        ctx->Draw(3, 0);
    }
    ctx->PSSetShader(g.atmospherePS[ms + (td.SampleDesc.Count > 1 ? 2 : 0)].Get(), nullptr, 0);
    ctx->Draw(3, 0);
    atmosphereScope.reset();
    if (measure) ctx->End(g.timingStages[1].Get());
    ctx->OMSetRenderTargets(1, &workingTarget, g.propDsv.Get());
    ctx->OMSetDepthStencilState(g.depth[reversed ? 1 : 0].Get(), 0);
    ctx->IASetInputLayout(g.layout.Get());
    ctx->VSSetShader(g.meshVS.Get(), nullptr, 0);
    auto materialSampler=g.materialSampler.Get();ctx->PSSetSamplers(4,1,&materialSampler);
    UINT stride = sizeof(Vertex), offset = 0;
    std::optional<GpuProfile::Scope> propScope;
    {static const unsigned pass=GpuProfile::PassId("composite: props");propScope.emplace(ctx,pass);}
    for (auto &o : f.scene.objects)
        if (!f.nativeLightingPass && o.enabled && !o.IsLight() && o.mesh && o.cameraVisible) {
            ctx->PSSetShader((o.material.enabled?g.materialMeshPS[variant]:g.meshPS[variant]).Get(),nullptr,0);
            ObjectConstants oc{};
            oc.model = ModelMatrix(o.position, o.angles, o.scale);
            Matrix inverse{};
            if (!Inverse(oc.model, inverse))
                continue;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    oc.normal[i * 4 + j] = inverse[j * 4 + i];
            oc.color = Pack(o.color, 1);
            const auto &m=o.material;oc.materialParams={m.roughness,m.metallic,m.emission,m.enabled?1.f:0.f};
            oc.materialCoat={m.coat,m.coatRoughness,m.anisotropy,m.normalStrength};
            oc.materialTextures={m.albedoImage?1.f:0.f,m.normalImage?1.f:0.f,m.roughnessImage?1.f:0.f,m.uvScale};
            auto images=std::array<std::shared_ptr<const MaterialImage>,3>{m.albedoImage,m.normalImage,m.roughnessImage};
            ID3D11ShaderResourceView* textures[3]{};
            for(int i=0;i<3;++i)if(m.enabled&&images[i])textures[i]=g.materialImages.at({images[i].get(),i==0}).view.Get();
            ctx->PSSetShaderResources(10,3,textures);
            auto buffer = g.meshes[o.mesh.get()].buffer.Get();
            ctx->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
            if(!m.enabled&&!o.mesh->parts.empty()){
                // A GLB: each part with the material its file gave it (textures, colour, roughness,
                // metal), tinted by the object's colour. A material set on the object overrides all.
                ctx->PSSetShader(g.materialMeshPS[variant].Get(),nullptr,0);
                for(const auto &part:o.mesh->parts){
                    if(!part.count||part.first+part.count>o.mesh->vertices.size())continue;
                    oc.color=Pack({part.color.x*o.color.x/.65f,part.color.y*o.color.y/.65f,part.color.z*o.color.z/.65f},1);
                    oc.materialParams={part.roughness,part.metallic,0,1};
                    oc.materialCoat={0,.12f,0,1};
                    oc.materialTextures={part.albedo?1.f:0.f,part.normal?1.f:0.f,part.roughnessMap?1.f:0.f,1};
                    auto partImages=std::array<std::shared_ptr<const MaterialImage>,3>{part.albedo,part.normal,part.roughnessMap};
                    ID3D11ShaderResourceView* partTextures[3]{};
                    for(int i=0;i<3;++i)if(partImages[i])partTextures[i]=g.materialImages.at({partImages[i].get(),i==0}).view.Get();
                    ctx->PSSetShaderResources(10,3,partTextures);
                    ctx->UpdateSubresource(g.objectConstants.Get(), 0, nullptr, &oc, 0, 0);
                    ctx->Draw(part.count, part.first);
                }
                continue;
            }
            ctx->UpdateSubresource(g.objectConstants.Get(), 0, nullptr, &oc, 0, 0);
            ctx->Draw(UINT(o.mesh->vertices.size()), 0);
        }
    propScope.reset();
    ctx->OMSetRenderTargets(1, &workingTarget, nullptr);
    ctx->OMSetDepthStencilState(g.noDepth.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ID3D11Buffer *noBuffer = nullptr;
    ctx->IASetVertexBuffers(0, 1, &noBuffer, &stride, &offset);
    srvs[2] = g.propSrv.Get();
    ctx->PSSetShaderResources(0, 4, srvs);
    const bool markWeather = measure && precipitating;
    if (measure) g.timingWeatherUsed = markWeather;
    if (w.type && w.count) {
        GPU_PASS_SCOPE(ctx, "composite: precipitation");
        ctx->OMSetBlendState(g.alpha.Get(), blend, 0xffffffff);
        // Particles test the roof and take their light once per particle in the vertex shader, so
        // it sees what the pixel stage sees: the shadow constants, shelter, sun maps and scene color.
        ID3D11Buffer *shadowConstants = nullptr;
        ctx->PSGetConstantBuffers(2, 1, &shadowConstants);
        ctx->VSSetConstantBuffers(2, 1, &shadowConstants);
        if (shadowConstants) shadowConstants->Release();
        ID3D11ShaderResourceView *views[31]{}; // through t30, the gust shelter maps
        ctx->PSGetShaderResources(0, 31, views);
        ctx->VSSetShaderResources(0, 31, views);
        for (auto *view : views)
            if (view) view->Release();
        ctx->VSSetShader(g.weatherVS[td.SampleDesc.Count > 1 ? 1 : 0].Get(), nullptr, 0);
        ctx->PSSetShader(g.weatherPS[variant].Get(), nullptr, 0);
        if (markWeather) ctx->End(g.timingWeather[0].Get());
        ctx->DrawInstanced(6, precipitating ? precipitation.Instances() : UINT(w.count), 0, 0);
        ID3D11ShaderResourceView *noViews[31]{};
        ctx->VSSetShaderResources(0, 31, noViews);
        if (markWeather) ctx->End(g.timingWeather[1].Get());
        // The same density past the particle window, integrated per pixel along the view ray.
        if (precipitating) {
            ctx->VSSetShader(g.screenVS.Get(), nullptr, 0);
            ctx->PSSetShader(g.veilPS[variant].Get(), nullptr, 0);
            ctx->Draw(3, 0);
        }
        if (markWeather) ctx->End(g.timingWeather[2].Get());
        // Ground splashes: rain only. The vertex shader reads the scene depth to find the floor.
        if (w.type == 1) {
            ID3D11ShaderResourceView* sceneDepthView = g.sceneSrv.Get();
            ctx->VSSetShaderResources(1, 1, &sceneDepthView);
            ctx->VSSetShader(g.splashVS[ms].Get(), nullptr, 0);
            ctx->PSSetShader(g.splashPS[variant].Get(), nullptr, 0);
            ctx->DrawInstanced(6, kSplashInstanceCount, 0, 0);
            ID3D11ShaderResourceView* noView = nullptr;
            ctx->VSSetShaderResources(1, 1, &noView);
        }
    }
    if (volumes) {
        GPU_PASS_SCOPE(ctx, "composite: fog volumes");
        ctx->OMSetBlendState(g.opaque.Get(), blend, 0xffffffff);
        ctx->VSSetShader(g.screenVS.Get(), nullptr, 0);
        ID3D11ShaderResourceView *noSkyVolume=nullptr;
        ctx->PSSetShaderResources(32,1,&noSkyVolume);
        ID3D11ShaderResourceView *noGeometry=nullptr;ctx->PSSetShaderResources(33,1,&noGeometry);
        ID3D11RenderTargetView *volumeTargets[]{g.volumeRtv.Get(),g.volumeGeometryRtv.Get()};
        ctx->OMSetRenderTargets(2, volumeTargets, nullptr);
        D3D11_VIEWPORT volumeViewport{0, 0, float(g.volumeWidth), float(g.volumeHeight), 0, 1};
        ctx->RSSetViewports(1, &volumeViewport);
        ctx->PSSetShader(g.volumePS[variant].Get(), nullptr, 0);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView *skyVolumeTarget=g.skyVolumeRtv.Get();
        ctx->OMSetRenderTargets(1,&skyVolumeTarget,nullptr);
        auto geometry=g.volumeGeometrySrv.Get();ctx->PSSetShaderResources(33,1,&geometry);
        ID3D11ShaderResourceView *marched = g.volumeSrv.Get();
        ctx->PSSetShaderResources(4,1,&marched);
        ctx->PSSetShader(g.skyVolumePS[variant].Get(),nullptr,0);
        ctx->Draw(3,0);
        // Depth-aware blur of the jittered march, still at volume resolution: the per-texel jitter
        // that hides step banding otherwise shows as grain in the light shafts.
        ID3D11RenderTargetView *blurTargets[] = {g.volumeBlurRtv.Get(),g.skyVolumeBlurRtv.Get()};
        ctx->OMSetRenderTargets(2, blurTargets, nullptr);
        ctx->PSSetShaderResources(4, 1, &marched);
        ID3D11ShaderResourceView *skyMarched=g.skyVolumeSrv.Get();
        ctx->PSSetShaderResources(32,1,&skyMarched);
        ctx->PSSetShader(g.volumeBlurPS[variant].Get(), nullptr, 0);
        ctx->Draw(3, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CopyResource(g.color.Get(), g.working.Get());
        ctx->OMSetRenderTargets(1, &workingTarget, nullptr);
        ctx->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView *composite[] = {g.colorSrv.Get(), g.sceneSrv.Get(), g.propSrv.Get(),
                                                 g.sourceColorSrv.Get(), g.volumeBlurSrv.Get()};
        ctx->PSSetShaderResources(0, 5, composite);
        ID3D11ShaderResourceView *skyComposite=g.skyVolumeBlurSrv.Get();
        ctx->PSSetShaderResources(32,1,&skyComposite);
        ctx->PSSetShader(g.compositePS[variant].Get(), nullptr, 0);
        ctx->Draw(3, 0);
    }
    if(storm.flash>0){
        GPU_PASS_SCOPE(ctx, "composite: storm");
        ctx->OMSetRenderTargets(1,&workingTarget,nullptr);
        ctx->OMSetBlendState(g.alpha.Get(),blend,0xffffffff);
        ctx->VSSetShader(g.stormVS.Get(),nullptr,0);
        ctx->PSSetShader(g.stormPS[variant].Get(),nullptr,0);
        ctx->DrawInstanced(6,kStormPointCount-1,0,0);
    }
    // Keep the original sample coverage through every pass, including final composition.
    ID3D11ShaderResourceView *none[9] = {};
    ctx->PSSetShaderResources(0, 9, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    std::optional<GpuProfile::Scope> blitScope;
    {static const unsigned pass=GpuProfile::PassId("composite: final copy");blitScope.emplace(ctx,pass);}
    ctx->CopyResource(g.color.Get(), g.working.Get());
    ctx->OMSetRenderTargets(1, &displayTarget, nullptr);
    ctx->OMSetBlendState(g.opaque.Get(), blend, 0xffffffff);
    ctx->VSSetShader(g.screenVS.Get(), nullptr, 0);
    ctx->PSSetShader(g.blitPS[td.SampleDesc.Count > 1 ? 1 : 0].Get(), nullptr, 0);
    ID3D11ShaderResourceView *finalColor = g.colorSrv.Get();

    ctx->PSSetShaderResources(0, 1, &finalColor);
    ctx->Draw(3, 0);
    blitScope.reset();
    ctx->ClearState();
    if (measure) {
        ctx->End(g.timingEnd.Get());
        ctx->End(g.timingDisjoint.Get());
    }
    ComPtr<ID3D11CommandList> commands;
    if (FAILED(ctx->FinishCommandList(FALSE, &commands))) {
        SetRenderStatus("Could not submit environment frame");
        return false;
    }
    {GPU_PASS_SCOPE(immediate, "environment composite (total)");
     immediate->ExecuteCommandList(commands.Get(), TRUE);}
    if (measure) {
        g.timingPending = true;
        g.timingSettings = {climate[CL_worldGi], climate[CL_bounce], climate[CL_sceneReflections],
                            f.hdr ? 1.f : 0.f};
    }
    // ADVISORY, not a blocker. Precipitation now draws without a shelter map (see
    // WorldPrecipitationVisible) -- preparing the map only adds roofs stopping the flakes, so the
    // wording says what is missing rather than reading as "nothing will work until you do this".
    bool missingShelter=f.scene.weather.shelter&&f.scene.weather.type>=1&&f.scene.weather.type<=3&&!f.occlusion;
    SetRenderStatus(climate[CL_storm] > .5f && !stormLighting ? "Rendering lightning (surface light needs map geometry and free light/shadow slots)" : missingShelter?"Rendering (precipitation falls through roofs until the map is prepared)":"Rendering", true);
    static unsigned probe = 0;
    if (MvmDebugLog_Active() && (++probe % 60) == 1) {
        MvmDebugLog_Linef(
            "environment.frame",
            "target=%p depth=%p targetSize=%ux%u depthSize=%ux%u samples=%u nativeViewFormat=%u displayFormat=%u hdr=%d nativeLightingApplied=%d nativeSkyApplied=%d reversed=%d time=%.5f props=%zu "
            "lights=%d weather=%d count=%d shelter=%d geometry=%d camera=(%.2f %.2f %.2f) proj=(%.4f %.4f %.6f %.6f) "
            "depthRange=(%.6f %.6f) samplePreserving=1 climate sky=%.2f clouds=%.2f volume=%.2f snow=%.2f "
            "wet=%.2f puddles=%.2f coatingNormals=filtered coatingShading=mapInherited "
            "worldGi=%.2f bounce=%.2f reflections=%.2f quality=%d",
            target, depth, td.Width, td.Height, dd.Width, dd.Height, dd.SampleDesc.Count,
            unsigned(rt.Format), unsigned(displayFormat), int(f.hdr), int(f.nativeLightingApplied), int(f.nativeSkyApplied), reversed, f.time,
            used.size(), lightCount, w.type, w.count, int(w.shelter), int(bool(f.occlusion)), f.camera.x, f.camera.y, f.camera.z, f.projection[0],
            f.projection[5], f.projection[10], f.projection[11], depthMin, depthMax, climate[CL_sky],
            climate[CL_clouds], climate[CL_fogDensity], climate[CL_snow], climate[CL_wet], climate[CL_puddles],
            climate[CL_worldGi], climate[CL_bounce], climate[CL_sceneReflections], quality);
        if (precipitating)
            MvmDebugLog_Linef("environment.precipitation",
                              "type=%d count=%d occupancy=%.3f cell=%.1f cells=%ux%ux%u instances=%u reach=%.0f/%.0f "
                              "veilDensity=%.3e veilTo=%.0f mapped=%d region=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) shelterReady=%d",
                              w.type, w.count, c.weatherColor.w, precipitation.cell, precipitation.across, precipitation.across,
                              precipitation.layers, precipitation.Instances(), precipitation.reach, precipitation.reachUp,
                              c.weatherLattice.w, c.weatherReach.z, int(precipitation.mapped), precipitation.low.x,
                              precipitation.low.y, precipitation.low.z, precipitation.high.x, precipitation.high.y,
                              precipitation.high.z, int(bool(f.occlusion)));
        for(const auto &o:f.scene.objects) if(o.enabled && o.IsLight())
            MvmDebugLog_Linef("environment.studio","id=%d kind=%s width=%.2f height=%.2f radius=%.2f pattern=%d profile=%d cue=%d input=%.3f evaluated=%.3f time=%.5f shadows=%d",
                o.id,o.kind.c_str(),o.studio.width,o.studio.height,o.studio.sourceRadius,o.studio.pattern,o.studio.profile,o.studio.cue,o.intensity,o.intensity*StudioCue(o.studio,f.time),f.time,int(o.shadows));
    }
    // Light editor markers go over the finished world (Lighting > Edit lights).
    if (Relight::LightEditorActive() && target) {
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            Relight::DrawLightMarkers(immediate, target, input.worldToClip.data(), input.camera, float(desc.Width), float(desc.Height));
        }
    }
    return true;
}
} // namespace Filmmaker::Environment
