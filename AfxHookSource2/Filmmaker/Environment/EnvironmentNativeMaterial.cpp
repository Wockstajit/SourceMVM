#define NOMINMAX
#include "EnvironmentNativeMaterial.h"
#include "OwnedInterface.h"
#include "EnvironmentAnimatedCasters.h"
#include "EnvironmentNativeMaterialMath.h"
#include "EnvironmentClimateMath.h"
#include "EnvironmentConstantPatchMath.h"
#include "EnvironmentNativePostMath.h"
#include "../Relight/RelightMath.h"
#include "../Relight/RelightClusterMath.h"
#include "../Relight/RelightClusterShaders.h"
#include "../Relight/RelightNativeCluster.h"
#include "../Relight/RelightProbes.h"
#include "../Relight/RelightStainedGlass.h"
#include "../Relight/RelightIrradianceMath.h"
#include "../Relight/RelightLightmap.h"
#include "EnvironmentShadowMath.h"
#include "EnvironmentNativeSunCsmMath.h"
#include "EnvironmentLightingProofShaders.h"
#include "EnvironmentNativeSkyShaders.h"
#include "EnvironmentNativeSmokeShaders.h"
#include "EnvironmentNativeSmokeMath.h"
#include "EnvironmentVfxLightingMath.h"
#include "EnvironmentGiMath.h"
#include "EnvironmentNativeCoverageMath.h"
#include "EnvironmentNativeAtmosphereMath.h"
#include "EnvironmentNativeAoMath.h"
#include "EnvironmentNativeLightingMath.h"
#include "EnvironmentNativeClusterMath.h"
#include "EnvironmentLightingViewMath.h"
#include "EnvironmentAmbientOcclusion.h"
#include "EnvironmentAmbientOcclusionMath.h"
#include "EnvironmentSunVisibility.h"
#include "EnvironmentShadowMaps.h"
#include "EnvironmentSkyVisibilityMath.h"
#include "EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../World/SkyGpuProbe.h"
#include "../Render/RenderPrivateTextureScope.h"
#include "../Render/RenderPixelResourceBindings.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID SunAbiInfoTag={0xc93635ba,0x7dca,0x49ab,{0xb5,0xee,0xab,0x60,0x17,0x0d,0x7c,0x94}};
constexpr GUID CoverageShaderTag={0x6c1edb13,0xeca5,0x4e44,{0x89,0x37,0x62,0x2e,0xae,0x1a,0xf5,0xf7}};
constexpr GUID BlackCoverageShaderTag={0x39e29345,0x4b16,0x4534,{0xac,0x89,0x7a,0x24,0xc0,0xea,0x5b,0x17}};
constexpr GUID SunAbiTag={0x3f61bdd1,0x622a,0x49f0,{0xae,0x7f,0x71,0xbc,0xda,0x42,0x66,0x21}};
constexpr GUID PostCompressionTag={0x61859533,0x6b4a,0x45b0,{0x90,0x5f,0x2e,0x5b,0x12,0xa1,0xc3,0xe7}};
constexpr GUID SharedAoShaderTag={0x72546290,0xe394,0x4a17,{0x9c,0xf0,0x17,0xb9,0x8a,0xfa,0x3e,0x11}};
constexpr GUID DynamicIrradianceTag={0x1fd049d5,0x52d1,0x4337,{0xa9,0x65,0x71,0x21,0x44,0xa4,0x48,0x94}};
constexpr GUID NativeAtmosphereTag={0x5c3f07b1,0xc2b7,0x4a6f,{0x91,0x23,0x60,0x48,0x45,0xe1,0x9a,0xb2}};
constexpr GUID NativeSmokeTag={0x07ca4708,0x7687,0x41c4,{0x90,0x4e,0xc7,0xb3,0x58,0x6d,0x0b,0x92}};
constexpr GUID SharedLightingShaderTag={0xd706814b,0xb394,0x405b,{0x95,0x66,0x35,0xd2,0x1c,0xa7,0x97,0xac}};
constexpr GUID SharedLightingInfoTag={0x80f77a69,0x4e85,0x437b,{0x9e,0x14,0xc4,0x62,0x2f,0x80,0x64,0x19}};
constexpr GUID SparseClusterInfoTag={0xf5c0c203,0x47a8,0x49e0,{0xb0,0x90,0xa4,0x47,0x41,0xd3,0x53,0x17}};
constexpr GUID SparseClusterShaderTag={0x4ac1e69b,0x03cf,0x4eb0,{0x95,0x6d,0x81,0xfa,0x6c,0x45,0x7c,0x42}};
struct Sun {float direction[4]{},radiance[4]{};};
std::map<uint64_t,std::vector<uint8_t>>& programRecord(){static std::map<uint64_t,std::vector<uint8_t>> record;return record;}
struct MaterialTiming {
    static constexpr unsigned Capacity=256;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> disjoint,start[Capacity],end[Capacity];
    bool measuring=false,pending=false,post[Capacity]{},live[Capacity]{};
    unsigned count=0,eligible=0;int active=-1;ULONGLONG last=0;
};
NativeMaterialHdrPass hdrPass=nullptr;
// Shader creation can precede renderer resource preparation. Keep this ABI
// observation across resource resets; the already tagged shaders remain alive.
std::atomic<bool> nativePostCompressedHdr{false};
LightingProofShadowPass proofShadowPass=nullptr;
SkyVisibilitySource skyVisibilitySource=nullptr;
ViewmodelShadowPass viewmodelShadowPass=nullptr;
GlassBeamShadowPass glassBeamShadowPass=nullptr;
MapLightShadowPass mapShadowPass=nullptr;
// Live lighting's draw-setup CPU time, split into laps: 0 everything else, 1 light preparation,
// 2 ambient occlusion. Render thread only; NextNativeMaterialFrame folds it into averages the
// main-thread heartbeat reads ("live ... prepare ... ao").
std::array<long long,3> liveLaps{};LARGE_INTEGER liveLapClock{};
std::array<std::atomic<float>,3> liveCostMs{}; // prepare, ambient occlusion, total; ~20-frame averages
void LiveLapStart(){QueryPerformanceCounter(&liveLapClock);}
void LiveLap(unsigned i){LARGE_INTEGER n;QueryPerformanceCounter(&n);liveLaps[i]+=n.QuadPart-liveLapClock.QuadPart;liveLapClock=n;}
struct SharedLightingCopy {UINT offset,bytes;ID3D11Buffer* source;};
struct SharedLightingBuffer {
    ComPtr<ID3D11Buffer> raw,constants;
    std::vector<uint8_t> key;
};
struct State {
    ConstantPatchBatch constantPatches;
    std::vector<SharedLightingCopy> lightingCopies;
    std::vector<SharedLightingBuffer> sharedLightingPool;
    std::vector<ConstantPatchBatch::Range> lightingCovered;
    std::vector<uint8_t> lightingKey;
    unsigned sharedLightingUsed=0;
    ComPtr<ID3D11Buffer> previousSharedLighting;bool sharedLightingActive=false;
    bool wanted=false,requested=false,active=false,captured=false,postActive=false,toned=false;
    // Live lighting: the engine's own draw is lit with our inputs in place, no replay.
    bool live=false;
    ComPtr<ID3D11PixelShader> skyShader;
    ComPtr<ID3D11Buffer> skyConstants;
    bool skyApplied=false;uint64_t skyGeneration=0;
    unsigned draws=0,worldDraws=0,fixtureDraws=0;bool worldActive=false;Sun next{},rendered{};uint64_t generation=0,renderedGeneration=0;
    ComPtr<ID3D11Texture2D> color,source,depth,resolved,tonedColor,tonedSource;
    ComPtr<ID3D11ShaderResourceView> resolvedSrv,tonedSrv,previousInput;
    ComPtr<ID3D11RenderTargetView> tonedRtv;
    ComPtr<ID3D11RenderTargetView> rtv,previousRtv;
    ComPtr<ID3D11DepthStencilView> previousDsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Buffer> clone,raw,previousConstants,sourceSunRaw,sourceSunClone;
    ComPtr<ID3D11DepthStencilState> depthState,previousDepthState;
    D3D11_DEPTH_STENCIL_DESC depthDesc{};UINT previousStencil=0;
    ComPtr<ID3D11BlendState> maskBlend,unlitOverlayBlend;D3D11_BLEND_DESC maskBlendDesc{};
    ComPtr<ID3D11BlendState> previousBlend;float previousFactors[4]{};UINT previousMask=~0u;
    ComPtr<ID3D11Texture2D> coverage, coverageResolved;
    ComPtr<ID3D11RenderTargetView> coverageRtv;
    ComPtr<ID3D11ShaderResourceView> coverageSrv, coverageResolvedSrv;
    ComPtr<ID3D11PixelShader> coveragePs, previousPs;
    bool coverageActive=false,composited=false;Frame frame;
    ComPtr<ID3D11Texture2D> baseline,baselineResolved,composite;
    ComPtr<ID3D11RenderTargetView> baselineRtv,compositeRtv;
    ComPtr<ID3D11ShaderResourceView> baselineSrv,baselineResolvedSrv,compositeSrv;
    ComPtr<ID3D11DepthStencilView> sourceDsv;
    bool fixtureAbi=false,fixturePass=false;
    ComPtr<ID3D11Texture2D> withoutFixtures,withoutFixturesResolved;
    ComPtr<ID3D11RenderTargetView> withoutFixturesRtv;
    ComPtr<ID3D11ShaderResourceView> withoutFixturesSrv,withoutFixturesResolvedSrv;
    ComPtr<ID3D11Buffer> previousProbeConstants;
    bool probeActive=false;
    bool proofActive=false;UINT proofLightSlot=20;RenderPixelResourceBindings proofResources;
    ComPtr<ID3D11Buffer> proofLights,proofMasks,proofAuthored,proofRebaseParams;
    ComPtr<ID3D11Buffer> proofLocalLights,sunCascadeParams,sunCascadeColumns,characterSunCascadeColumns;
    ComPtr<ID3D11UnorderedAccessView> characterSunCascadeUav;
    // Copy of the native view constants (b4) with the AO addressing row repaired; see BindOwnedInputs.
    // The traced bounce volume carries its own sky visibility; the screen-space sky term must
    // then stay out of the AO, which also scales the bounce it would wrongly darken indoors.
    bool probeTraced=false;
    ComPtr<ID3D11Buffer> viewClone,viewCloneRaw,previousView;bool viewActive=false;
    ComPtr<ID3D11Buffer> sharedAo,previousSharedAo;bool sharedAoActive=false;
    std::array<float,4> sharedAoRow{};
    ComPtr<ID3D11ShaderResourceView> proofLocalLightSrv;
    ComPtr<ID3D11UnorderedAccessView> proofLocalLightUav,sunCascadeUav;
    UINT proofRecordCount=0,proofMaskWords=1;
    ComPtr<ID3D11DeviceContext> proofContext;
    ComPtr<ID3D11Buffer> proofPreparedView,proofPreparedViewport;
    D3D11_VIEWPORT proofViewport{};
    uint64_t proofPreparedGeneration=0;
    bool proofResourcesReady=false;
    unsigned proofUploads=0,proofAuthoredUploads=0;size_t proofAuthoredBytes=0;
    std::vector<Relight::LightRecord> proofUploadedRecords;
    LightingViewGate proofViewGate;
    ComPtr<ID3D11Buffer> proofViewGateReadback;bool proofViewGateReadbackPending=false;
    Relight::ClusterGrid clusterGrid{};
    Relight::NativeClusterWork nativeClusters;
    unsigned clusterMode=0;bool clusterSummaries=false;
    ComPtr<ID3D11Buffer> clusterParams,clusterExtents,clusterViewReadback,clusterViewportReadback;
    ComPtr<ID3D11ShaderResourceView> clusterExtentSrv;
    ComPtr<ID3D11UnorderedAccessView> clusterExtentUav,clusterMaskUav;
    ComPtr<ID3D11ComputeShader> clusterBounds,clusterFill;
    ComPtr<ID3D11Buffer> clusterRows;
    ComPtr<ID3D11UnorderedAccessView> clusterRowsUav;
    ComPtr<ID3D11ShaderResourceView> clusterRowsSrv;
    bool clusterReadbackPending=false,clusterLogged=false;
    ComPtr<ID3D11ComputeShader> proofRebase;ComPtr<ID3D11UnorderedAccessView> proofLightUav;
    ComPtr<ID3D11ShaderResourceView> proofLightSrv,proofMaskSrv,proofAuthoredSrv,proofBlack2d,proofBlackCube,proofBlackCubeArray,proofBlackArray;
    ComPtr<ID3D11Texture2D> relightAmbient;
    ComPtr<ID3D11ShaderResourceView> relightAmbientSrv;
    ComPtr<ID3D11RenderTargetView> relightAmbientRtv;
    // The atlas drawn from the map's own lightmap layout; when present it replaces the flat one.
    ID3D11ShaderResourceView* lightmapAtlas=nullptr;
    unsigned relightAmbientWidth=0,relightAmbientHeight=0;
    bool relightAmbientDiag=false;
    uint64_t relightAmbientKey=~0ull;
    unsigned engineLightmapWidth=0,engineLightmapHeight=0,engineLightmapArray=0;
    // Baked lighting off. NativeMaterialUnbakeScope reads these; it owns its own save/restore
    // per draw, so nothing about the replay's bindings is shared with it.
    bool unbake=false;float unbakeFillValue=0,unbakeFillUploaded=-1;
    ComPtr<ID3D11Texture2D> unbakeFill;ComPtr<ID3D11ShaderResourceView> unbakeFillSrv;
    // The second carrier: props, characters, foliage, cables and projected decals take their
    // baked lighting from the probe volume rather than the lightmap, so the world unbakes and
    // they do not. One 1x1x1 volume answers every sample position identically, which makes the
    // map's packing transform irrelevant -- whatever cell the shader resolves, it reads this.
    ComPtr<ID3D11Texture3D> unbakeProbe;ComPtr<ID3D11ShaderResourceView> unbakeProbeSrv;
    // The third carrier: a baked reflection cubemap is a 360 capture of the map under its
    // original lights, so a glossy surface keeps mirroring lighting that has otherwise been
    // removed. One flat cube at the same fill replaces the picture with uniform ambient.
    ComPtr<ID3D11ShaderResourceView> unbakeCubeSrv,unbakeCubeArraySrv;
    ComPtr<ID3D11Texture2D> unbakeCube,unbakeCubeArray;
    // This frame's stained-glass cookie volume, bound at each program's cookie slot.
    ID3D11ShaderResourceView* glassCookies=nullptr;
    Relight::ProbeBinding probeBinding{};
    Relight::ProbeBinding skyboxBinding{};
    ComPtr<ID3D11Buffer> smokeLights,smokeConstants,smokeProbes;
    ComPtr<ID3D11ShaderResourceView> smokeLightSrv;
    std::vector<Relight::LightRecord> smokeRecords;
    uint64_t smokeFrame=~0ull;
    std::map<UINT,std::array<ComPtr<ID3D11Buffer>,2>> constantClonePool;
    ComPtr<ID3D11Buffer> irradianceConstants;
    std::array<float,40> irradiancePacking{};
    uint64_t probeFrame=0;
    bool proofPrepared=false;Matrix proofMatrices[Relight::ShadowTiles]{};UINT proofShadowLights=0;
    std::vector<unsigned> proofNearby,proofCached,proofFirstTiles;
    UINT proofStaticLights=0;bool proofSunShadowed=false;
    ComPtr<ID3D11ShaderResourceView> proofAtlas;ComPtr<ID3D11SamplerState> proofComparison,proofPreviousSampler;UINT proofSamplerSlot=6;
    MaterialTiming timing;
} state;
// Local-light receiver bias for player bodies (cb3[1953].z is shared by every light record).
float CharacterLocalBias(const Relight::Scene& set){return (std::max)(set.shadowBias,.002f);}
void BeginTiming(ID3D11DeviceContext* ctx,bool post,bool live=false){
    auto& t=state.timing;t.active=-1;
    if(!MvmDebugLog_Active()||t.pending||(!t.measuring&&GetTickCount64()-t.last<3000))return;
    if(!t.disjoint){
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);MaterialTiming created;
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP,0};
        for(unsigned i=0;i<MaterialTiming::Capacity;++i)if(FAILED(device->CreateQuery(&q,&created.start[i]))||FAILED(device->CreateQuery(&q,&created.end[i])))return;
        q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;
        if(FAILED(device->CreateQuery(&q,&created.disjoint)))return;
        t=std::move(created);t.context=ctx;
    }
    if(!t.measuring){t.count=t.eligible=0;t.last=GetTickCount64();ctx->Begin(t.disjoint.Get());t.measuring=true;}
    ++t.eligible;
    if(t.count==MaterialTiming::Capacity)return;
    t.active=int(t.count++);t.post[t.active]=post;t.live[t.active]=live;ctx->End(t.start[t.active].Get());
}
void EndTiming(ID3D11DeviceContext* ctx){auto& t=state.timing;if(t.active>=0){ctx->End(t.end[t.active].Get());t.active=-1;}}
void NextTimingFrame(){
    auto& t=state.timing;if(!t.context)return;
    if(t.measuring){t.context->End(t.disjoint.Get());t.measuring=false;t.pending=true;}
    if(!t.pending)return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    if(t.context->GetData(t.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
    double material=0,post=0,live=0;unsigned liveCount=0;
    for(unsigned i=0;i<t.count;++i){
        UINT64 start=0,end=0;
        if(t.context->GetData(t.start[i].Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||
           t.context->GetData(t.end[i].Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
        if(!d.Disjoint&&d.Frequency&&end>=start)(t.post[i]?post:t.live[i]?live:material)+=double(end-start)*1000/d.Frequency;
        if(t.live[i])++liveCount;
    }
    if(!d.Disjoint)MvmDebugLog_LinefAlways("environment.native.material.gpu","sampledReplayMs=%.3f sampledLiveMs=%.3f sampledTonemapMs=%.3f sampledLivePasses=%u sampledPasses=%u eligiblePasses=%u maxSampledPasses=%u",material,live,post,liveCount,t.count,t.eligible,MaterialTiming::Capacity);
    t.pending=false;
}
void Evidence(const char* reason,UINT bytes=0){
    if(!MvmDebugLog_Active())return;
    static std::set<std::string> reasons; if(reasons.size()>64||!reasons.insert(reason).second)return;
    MvmDebugLog_LinefAlways("environment.native.material","reason=%s bufferBytes=%u draws=%u sun=(%.3f %.3f %.3f) radiance=(%.3f %.3f %.3f)",
        reason,bytes,state.draws,state.next.direction[0],state.next.direction[1],state.next.direction[2],state.next.radiance[0],state.next.radiance[1],state.next.radiance[2]);
}
bool Resources(ID3D11Device* device,const D3D11_TEXTURE2D_DESC& source,const D3D11_RENDER_TARGET_VIEW_DESC& view,UINT bufferBytes){
    RenderPrivateTextureScope privateTextures;
    D3D11_BUFFER_DESC previousBuffer{};if(state.clone)state.clone->GetDesc(&previousBuffer);
    if(!state.clone||!state.raw||previousBuffer.ByteWidth!=bufferBytes){
        state.clone.Reset();state.raw.Reset();
        D3D11_BUFFER_DESC b{};b.ByteWidth=bufferBytes;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&b,nullptr,&state.clone)))return false;
        // A plain GPU buffer permits byte-range updates. Constant buffers
        // require whole-buffer updates, so preserve the original bytes with two GPU copies.
        b.BindFlags=0;
        if(FAILED(device->CreateBuffer(&b,nullptr,&state.raw)))return false;
    }
    D3D11_TEXTURE2D_DESC previous{};if(state.color)state.color->GetDesc(&previous);
    if(!state.color||previous.Width!=source.Width||previous.Height!=source.Height||previous.Format!=view.Format||
       previous.SampleDesc.Count!=source.SampleDesc.Count||previous.SampleDesc.Quality!=source.SampleDesc.Quality){
        state.color.Reset();state.rtv.Reset();state.srv.Reset();
        auto d=source;d.Format=view.Format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.color))||FAILED(device->CreateRenderTargetView(state.color.Get(),nullptr,&state.rtv))||
           FAILED(device->CreateShaderResourceView(state.color.Get(),nullptr,&state.srv)))return false;
    }
    D3D11_TEXTURE2D_DESC cd{};if(state.coverage)state.coverage->GetDesc(&cd);
    D3D11_TEXTURE2D_DESC bd{};if(state.baseline)state.baseline->GetDesc(&bd);
    if(!state.baseline||bd.Width!=source.Width||bd.Height!=source.Height||bd.Format!=view.Format||
        bd.SampleDesc.Count!=source.SampleDesc.Count||bd.SampleDesc.Quality!=source.SampleDesc.Quality){
        state.baseline.Reset();state.baselineRtv.Reset();state.baselineSrv.Reset();
        auto d=source;d.Format=view.Format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.baseline))||FAILED(device->CreateRenderTargetView(state.baseline.Get(),nullptr,&state.baselineRtv))||
            FAILED(device->CreateShaderResourceView(state.baseline.Get(),nullptr,&state.baselineSrv)))return false;
    }
    if(state.fixturePass){
        D3D11_TEXTURE2D_DESC fd{};if(state.withoutFixtures)state.withoutFixtures->GetDesc(&fd);
        if(!state.withoutFixtures||fd.Width!=source.Width||fd.Height!=source.Height||fd.Format!=view.Format||
           fd.SampleDesc.Count!=source.SampleDesc.Count||fd.SampleDesc.Quality!=source.SampleDesc.Quality){
            state.withoutFixtures.Reset();state.withoutFixturesRtv.Reset();state.withoutFixturesSrv.Reset();
            auto d=source;d.Format=view.Format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&state.withoutFixtures))||
               FAILED(device->CreateRenderTargetView(state.withoutFixtures.Get(),nullptr,&state.withoutFixturesRtv))||
               FAILED(device->CreateShaderResourceView(state.withoutFixtures.Get(),nullptr,&state.withoutFixturesSrv)))return false;
        }
    }
    if(!state.coverage||cd.Width!=source.Width||cd.Height!=source.Height||
        cd.SampleDesc.Count!=source.SampleDesc.Count||cd.SampleDesc.Quality!=source.SampleDesc.Quality){
        state.coverage.Reset();state.coverageRtv.Reset();state.coverageSrv.Reset();
        auto d=source;d.Format=DXGI_FORMAT_R8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.coverage))||
            FAILED(device->CreateRenderTargetView(state.coverage.Get(),nullptr,&state.coverageRtv))||
            FAILED(device->CreateShaderResourceView(state.coverage.Get(),nullptr,&state.coverageSrv)))return false;
    }
    return state.rtv&&state.srv&&state.coverageSrv;
}
// All private cluster work shares the rebase deferred context. Each output word has
// one writer, and the read-only ones regions remain valid even if preparation fails.
bool ClusterResources(ID3D11Device* device) {
    if(!state.clusterBounds||!state.clusterFill) {
        for(unsigned pass=0;pass<2;++pass) {
            auto& shader=pass?state.clusterFill:state.clusterBounds;
            if(shader)continue;
            ComPtr<ID3DBlob> code,error;
            if(FAILED(CompileEnvironmentShader(Relight::ClusterShader,sizeof(Relight::ClusterShader)-1,
                "RelightCluster",nullptr,pass?"Fill":"Bounds","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||
               FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader))) {
                Evidence("relight:cluster-compile-failed");return false;
            }
        }
    }
    if(!state.clusterParams) {
        D3D11_BUFFER_DESC d{};d.ByteWidth=96;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.clusterParams)))return false;
    }
    if(!state.clusterRows) {
        D3D11_BUFFER_DESC d{};d.ByteWidth=96;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.clusterRows))||
           FAILED(device->CreateUnorderedAccessView(state.clusterRows.Get(),nullptr,&state.clusterRowsUav))||
           FAILED(device->CreateShaderResourceView(state.clusterRows.Get(),nullptr,&state.clusterRowsSrv))) {
            state.clusterRows.Reset();return false;
        }
    }
    if(!state.clusterExtents) {
        D3D11_BUFFER_DESC d{};d.ByteWidth=state.proofRecordCount*sizeof(Relight::ClusterExtent);
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(Relight::ClusterExtent);
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.clusterExtents))||
           FAILED(device->CreateShaderResourceView(state.clusterExtents.Get(),nullptr,&state.clusterExtentSrv))||
           FAILED(device->CreateUnorderedAccessView(state.clusterExtents.Get(),nullptr,&state.clusterExtentUav))) {
            state.clusterExtents.Reset();return false;
        }
    }
    if(!state.clusterMaskUav&&FAILED(device->CreateUnorderedAccessView(state.proofMasks.Get(),nullptr,&state.clusterMaskUav)))return false;
    return true;
}
void ClusterEvidence(ID3D11DeviceContext* ctx) {
    if(!state.clusterReadbackPending)return;
    D3D11_MAPPED_SUBRESOURCE v{},p{};
    if(FAILED(ctx->Map(state.clusterViewReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&v)))return;
    if(FAILED(ctx->Map(state.clusterViewportReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&p))) {
        ctx->Unmap(state.clusterViewReadback.Get(),0);return;
    }
    const auto view=static_cast<const float*>(v.pData),viewport=static_cast<const float*>(p.pData);
    UINT branch=0;std::memcpy(&branch,view+8,4);
    MvmDebugLog_LinefAlways("relight.cluster.abi","cb4[2].x=0x%08x cb1[20]=(%.3f %.3f %.3f %.3f) affineSlices=1",
        branch,viewport[80],viewport[81],viewport[82],viewport[83]);
    ctx->Unmap(state.clusterViewportReadback.Get(),0);ctx->Unmap(state.clusterViewReadback.Get(),0);
    state.clusterReadbackPending=false;state.clusterLogged=true;
}
// One black 1x1 Texture2DArray stands in for a baked direct-light input. Shared by the owned
// relighter and the plain unbake, which replace the same slots.
bool EnsureBlackArray(ID3D11Device* device){
    if(state.proofBlackArray)return true;
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const float black[4]{};D3D11_SUBRESOURCE_DATA initial{black,16,16};ComPtr<ID3D11Texture2D> t;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;
    return SUCCEEDED(device->CreateTexture2D(&d,&initial,&t))&&SUCCEEDED(device->CreateShaderResourceView(t.Get(),&srv,&state.proofBlackArray));
}
// The unbake's replacement irradiance: one grey texel the whole map samples, re-uploaded only
// when the climate value moves. Sixteen bytes, so it rides the immediate context like the
// constant patches do.
bool EnsureUnbakeResources(ID3D11Device* device,ID3D11DeviceContext* ctx){
    RenderPrivateTextureScope privateTextures;
    if(!EnsureBlackArray(device))return false;
    if(!state.unbakeFill){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.unbakeFill))||FAILED(device->CreateShaderResourceView(state.unbakeFill.Get(),&srv,&state.unbakeFillSrv)))return false;
        state.unbakeFillUploaded=-1;
    }
    if(!state.unbakeProbe){
        D3D11_TEXTURE3D_DESC d{};d.Width=d.Height=d.Depth=d.MipLevels=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateTexture3D(&d,nullptr,&state.unbakeProbe))||
           FAILED(device->CreateShaderResourceView(state.unbakeProbe.Get(),nullptr,&state.unbakeProbeSrv)))return false;
        state.unbakeFillUploaded=-1;
    }
    if(!state.unbakeCube||!state.unbakeCubeArray){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=1;d.ArraySize=6;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;srv.TextureCube.MipLevels=1;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.unbakeCube))||
           FAILED(device->CreateShaderResourceView(state.unbakeCube.Get(),&srv,&state.unbakeCubeSrv)))return false;
        // A cube ARRAY view needs its own resource: the dimension is part of the view, and a
        // shader declaring texturecubearray rejects a plain cube.
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MipLevels=1;srv.TextureCubeArray.NumCubes=1;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.unbakeCubeArray))||
           FAILED(device->CreateShaderResourceView(state.unbakeCubeArray.Get(),&srv,&state.unbakeCubeArraySrv)))return false;
        state.unbakeFillUploaded=-1;
    }
    if(state.unbakeFillUploaded!=state.unbakeFillValue){
        const float fill=(std::max)(0.f,state.unbakeFillValue);const float texel[4]{fill,fill,fill,1};
        for(UINT face=0;face<6;++face){
            ctx->UpdateSubresource(state.unbakeCube.Get(),face,nullptr,texel,sizeof(texel),sizeof(texel));
            ctx->UpdateSubresource(state.unbakeCubeArray.Get(),face,nullptr,texel,sizeof(texel),sizeof(texel));
        }
        ctx->UpdateSubresource(state.unbakeFill.Get(),0,nullptr,texel,sizeof(texel),sizeof(texel));
        // Same value into the probe volume, so a prop and the wall behind it receive the same
        // ambient and read as one surface rather than two lighting models meeting at an edge.
        ctx->UpdateSubresource(state.unbakeProbe.Get(),0,nullptr,texel,sizeof(texel),sizeof(texel));
        state.unbakeFillUploaded=state.unbakeFillValue;
    }
    return true;
}
void LightingViewEvidence(ID3D11Device* device,ID3D11DeviceContext* ctx){
    if(!MvmDebugLog_Active())return;
    if(state.proofViewGateReadbackPending){D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(ctx->Map(state.proofViewGateReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))){
            const auto* values=static_cast<const UINT*>(mapped.pData)+28;
            MvmDebugLog_LinefAlways("relight.view.reuse","runs=%u hits=%u forced=%u",values[0],values[1],values[2]);
            ctx->Unmap(state.proofViewGateReadback.Get(),0);state.proofViewGateReadbackPending=false;
        }
    }
    static ULONGLONG last=0;const auto now=GetTickCount64();auto source=state.proofViewGate.Counters();
    if(source&&!state.proofViewGateReadbackPending&&now-last>=3000){last=now;
        if(!state.proofViewGateReadback){D3D11_BUFFER_DESC d{};d.ByteWidth=128;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateBuffer(&d,nullptr,&state.proofViewGateReadback)))return;}
        ctx->CopyResource(state.proofViewGateReadback.Get(),source);state.proofViewGateReadbackPending=true;
    }
}
bool ProofResources(ID3D11Device* device,ID3D11DeviceContext* ctx,const D3D11_VIEWPORT& viewport){
    RenderPrivateTextureScope privateTextures;
    // Whole-map relighting supplies one record per authored light. The older
    // single-fixture proof supplies its six shadow faces plus the sun instead.
    const bool relight=state.frame.relight.enabled;
    ClusterEvidence(ctx);LightingViewEvidence(device,ctx);
    // Created before the records: the stained glass beams test sunlight on the glass with it.
    if(!state.proofComparison){D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;d.BorderColor[0]=d.BorderColor[1]=d.BorderColor[2]=d.BorderColor[3]=1;d.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;d.MaxLOD=D3D11_FLOAT32_MAX;if(FAILED(device->CreateSamplerState(&d,&state.proofComparison)))return false;}
    ComPtr<ID3D11Buffer> view;ctx->PSGetConstantBuffers(4,1,&view);
    ComPtr<ID3D11Buffer> nativeViewport;ctx->PSGetConstantBuffers(1,1,&nativeViewport);
    const uint64_t generation=relight?state.frame.relight.generation:state.frame.proof.generation;
    // PrepareNativeMaterialFrame invalidates this for every camera/frame. A pass or
    // viewport change invalidates the shared rebased buffer, including world <-> weapon.
    if(relight&&state.frame.relight.hoist&&state.proofResourcesReady&&
       state.proofPreparedGeneration==generation&&state.proofPreparedView.Get()==view.Get()&&
       state.proofPreparedViewport.Get()==nativeViewport.Get()&&
       !std::memcmp(&state.proofViewport,&viewport,sizeof(viewport)))return true;
    D3D11_BUFFER_DESC vd{};if(view)view->GetDesc(&vd);if(vd.ByteWidth<36*16)return false;
    D3D11_BUFFER_DESC viewportDesc{};if(nativeViewport)nativeViewport->GetDesc(&viewportDesc);
    state.proofResourcesReady=false;state.clusterMode=0;state.probeBinding={};state.skyboxBinding={};
    std::vector<Relight::LightRecord> records;
    if(relight){
        const auto& set=state.frame.relight;
        // A shadowed light becomes six records, one per cube face, because the native
        // shadow lookup is a single projection into a shared atlas rather than a cube
        // sample. Two tiers of them: a small budget of nearby lights that get full
        // resolution and moving casters, and a much larger cached tier drawn once from
        // static geometry. Anything left over is a single shadowless record, and a
        // shadowless record is culled only by a box that walls do not interrupt -- which
        // is why the cached tier exists at all.
        const bool localEnergy=Relight::HasLocalRadiance(set);
        const bool sunShadow=set.sunShadows&&Relight::HasRadiance(Relight::SunRadiance(set));
        if(!state.proofPrepared){
            state.proofNearby=Relight::StableShadowBudget(set.lights,state.frame.camera,
                set.shadows&&localEnergy?Relight::ShadowBudgetLimit:0u,state.proofNearby);
            state.proofCached=set.shadows&&localEnergy?Relight::StaticShadowSet(set.lights,state.proofNearby,
                (std::min)(set.staticShadows,Relight::StaticShadowLimit)):std::vector<unsigned>{};
            const auto& shadowed=state.proofNearby;const auto& cached=state.proofCached;
            state.proofShadowLights=0;state.proofSunShadowed=false;state.proofStaticLights=0;
            // The pass also draws the sun tile, so it runs whenever shadows are on even
            // if no local light won a slot.
            if(set.shadows&&(localEnergy||sunShadow)&&mapShadowPass){
                CompletedSceneCasterScope completedScene;
                std::vector<Vec3> positions;std::vector<float> ranges;
                for(auto index:shadowed){positions.push_back(set.lights[index].position);ranges.push_back(set.lights[index].range);}
                if(MvmDebugLog_Active()){
                    static ULONGLONG last=0;const auto now=GetTickCount64();
                    if(now-last>=3000){last=now;
                        for(auto index:shadowed){const auto& light=set.lights[index];
                            MvmDebugLog_LinefAlways("relight.shadow.selection","index=%u position=(%.3f %.3f %.3f) range=%.3f sceneVertices=%u actors=%u",
                                index,light.position.x,light.position.y,light.position.z,light.range,
                                AnimatedCasterVertexCount(AnimatedCasterSubset::Scene),AnimatedCasterVertexCount(AnimatedCasterSubset::Actors));}
                    }
                }
                std::vector<Vec3> cachedPositions;std::vector<float> cachedRanges;
                for(auto index:cached){cachedPositions.push_back(set.lights[index].position);cachedRanges.push_back(set.lights[index].range);}
                ID3D11ShaderResourceView* atlas=nullptr;unsigned staticReady=0;
                const unsigned rendered=mapShadowPass(ctx,state.frame,positions.empty()?nullptr:positions.data(),
                                                      positions.empty()?nullptr:ranges.data(),
                                                      unsigned(positions.size()),
                                                      cachedPositions.empty()?nullptr:cachedPositions.data(),
                                                      cachedRanges.empty()?nullptr:cachedRanges.data(),
                                                      unsigned(cachedPositions.size()),
                                                      Relight::SunDirection(set.sunAngles),
                                                      state.proofMatrices,&staticReady,&atlas);
                if(atlas){state.proofAtlas=atlas;state.proofShadowLights=rendered;state.proofSunShadowed=sunShadow;
                          state.proofStaticLights=staticReady;}
            }
            state.proofPrepared=true;
            state.proofFirstTiles.assign(set.lights.size(),~0u);
            for(unsigned s=0;s<state.proofStaticLights&&s<cached.size();++s)
                state.proofFirstTiles[cached[s]]=Relight::StaticTile+s*Relight::ShadowFaces;
            for(unsigned s=0;s<state.proofShadowLights&&s<shadowed.size();++s)
                state.proofFirstTiles[shadowed[s]]=s*Relight::ShadowFaces;
            if(MvmDebugLog_Active()){
                static ULONGLONG lastVfx=0;const auto now=GetTickCount64();
                if(now-lastVfx>=100){for(unsigned i=0;i<set.lights.size();++i)if(set.lights[i].transient){
                    lastVfx=now;MvmDebugLog_LinefAlways("environment.vfx.render","time=%.3f kind=%s index=%u energy=%.3f shadowTile=%u",
                        state.frame.time,set.lights[i].identity.c_str(),i,Relight::Power(set.lights[i],set.intensity),state.proofFirstTiles[i]);}}
            }
        }
        records.reserve(set.lights.size()+
                        size_t(state.proofShadowLights+state.proofStaticLights)*Relight::ShadowFaces);
        for(unsigned index=0;index<set.lights.size()&&localEnergy;++index){
            auto light=set.lights[index];
            if(viewport.MaxDepth<=.1f+1e-6f){light.brightnessScale*=VfxViewmodelEnergy(light);
                if(light.transient)light.minimumRoughness=1;}
            // The first tile of this light's six faces, or none if it is shadowless. A
            // cached light counts only once all six of its faces have been drawn, because
            // a record pointing at a tile nothing was rendered into samples stale depth.
            const unsigned first=state.proofFirstTiles[index];
            const bool ownWick=viewport.MaxDepth<=.1f+1e-6f&&(light.identity=="vfx:held"||light.identity=="vfx:bottle");
            // The first-person bottle/hands use a different pose from the world
            // proxy. Its world shadow moving across the hands is a false self shadow.
            // The local muzzle uses the first-person pose, not the world proxy.
            // Other sources retain wall occlusion, including stained glass.
            if(ownWick||(viewport.MaxDepth<=.1f+1e-6f&&VfxOwnMuzzle(light))){records.push_back(Relight::ToRecord(light,set.intensity));continue;}
            if(first==~0u){if(!set.shadows||!light.transient)records.push_back(Relight::ToRecord(light,set.intensity));continue;}
            for(unsigned face=0;face<Relight::FixtureFaces(light);++face)
                records.push_back(Relight::ToShadowedRecord(light,set.intensity,
                    state.proofMatrices[first+face].data(),first+face));
        }
        // The map's sun, as one directional record. Sunlight reaching through a doorway
        // is the shadow map doing its job; what it cannot do is the bounce that fills
        // the rest of the room, which is indirect light and is not implemented.
        if(set.sun>0&&state.frame.occlusion){
            auto low=state.frame.occlusion->low,high=state.frame.occlusion->high;
            if(state.frame.occlusion->visual){const auto& v=*state.frame.occlusion->visual;
                low={(std::min)(low.x,v.low.x),(std::min)(low.y,v.low.y),(std::min)(low.z,v.low.z)};
                high={(std::max)(high.x,v.high.x),(std::max)(high.y,v.high.y),(std::max)(high.z,v.high.z)};}
            const Vec3 direction=Relight::SunDirection(set.sunAngles);
            const Vec3 radiance=Relight::SunRadiance(set);
            const auto matrix=SunShadowMatrix(low,high,direction);
            records.push_back(Relight::SunRecord(direction,radiance,matrix.data(),state.proofSunShadowed&&set.sunShadows));
            // Stained glass: one coloured beam per window the sun reaches (RelightStainedGlass).
            state.glassCookies=nullptr;
            if(set.stainedGlass>0&&state.frame.occlusion->visual){
                const auto beams=Relight::PrepareStainedGlass(device,ctx,state.frame.occlusion->visual,&records.back(),
                    state.proofAtlas.Get(),state.proofComparison.Get(),direction,radiance*set.stainedGlass,state.probeFrame);
                if(beams.cookies&&!beams.records.empty()){
                    // Each beam is stopped by the world behind its glass (its own shadow map). A beam
                    // without one is dropped rather than lighting through walls.
                    std::vector<Matrix> matrices;
                    for(const auto& beam:beams.records){Matrix m{};std::memcpy(m.data(),beam.data(),sizeof(Matrix));matrices.push_back(m);}
                    const unsigned shadowed=glassBeamShadowPass?glassBeamShadowPass(ctx,matrices.data(),unsigned(matrices.size())):0u;
                    for(unsigned i=0;i<shadowed;++i){auto beam=beams.records[i];Relight::ShadowGlassBeam(beam,i);records.push_back(beam);}
                    if(shadowed)state.glassCookies=beams.cookies;
                }
            }
        }
    }else records.resize(7);
    // Zero emitters is a valid blackout. Keep ownership of the native material and
    // its baked-input suppression by binding one inert record instead of falling through.
    if(records.empty())records.emplace_back();
    if(relight&&state.frame.relight.cost==2)records.resize(1);
    const UINT recordCount=UINT(records.size());
    state.proofMaskWords=Relight::MaskWords(recordCount);
    float farDistance=10000;
    if(state.frame.occlusion) {
        const auto low=state.frame.occlusion->low,high=state.frame.occlusion->high;
        const auto delta=high-low;farDistance=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    }
    auto grid=Relight::MakeClusterGrid(UINT(viewport.Width),UINT(viewport.Height),recordCount,farDistance);
    // Bound allocations for unusually large exports/viewports; full-mask mode remains usable.
    const bool wantSummaries=grid.lights.words<=32;
    const bool gridBudget=grid.size<=16*1024*1024/(sizeof(UINT)*(wantSummaries?2:1));
    if(!gridBudget)grid=Relight::MakeClusterGrid(1,1,recordCount,farDistance);
    const bool summaries=gridBudget&&wantSummaries;
    if(grid.size!=state.clusterGrid.size||summaries!=state.clusterSummaries) {
        state.proofMasks.Reset();state.proofMaskSrv.Reset();state.clusterMaskUav.Reset();
        state.proofViewGate.Invalidate();
    }
    state.clusterGrid=grid;
    state.clusterSummaries=summaries;
    if(state.proofRecordCount!=recordCount){
        state.proofRecordCount=recordCount;
        state.proofLightUav.Reset();state.proofLights.Reset();state.proofLightSrv.Reset();
        state.proofLocalLights.Reset();state.proofLocalLightSrv.Reset();state.proofLocalLightUav.Reset();
        state.proofMasks.Reset();state.proofMaskSrv.Reset();state.clusterMaskUav.Reset();
        state.clusterExtents.Reset();state.clusterExtentSrv.Reset();state.clusterExtentUav.Reset();
        state.proofAuthored.Reset();state.proofAuthoredSrv.Reset();
        state.proofUploadedRecords.clear();state.proofViewGate.Invalidate();
    }
    if(!state.proofLights||!state.proofMasks||!state.proofAuthored||!state.proofLightSrv||!state.proofMaskSrv||!state.proofAuthoredSrv){
        state.proofLightUav.Reset();state.proofLights.Reset();state.proofMasks.Reset();state.proofAuthored.Reset();
        state.proofUploadedRecords.clear();state.proofViewGate.Invalidate();
        state.proofLightSrv.Reset();state.proofMaskSrv.Reset();state.proofAuthoredSrv.Reset();
        auto buffer=[&](UINT stride,UINT count,const void* data,bool unordered,ComPtr<ID3D11Buffer>& out,ComPtr<ID3D11ShaderResourceView>& srv){
            D3D11_BUFFER_DESC d{};d.ByteWidth=stride*count;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(unordered?D3D11_BIND_UNORDERED_ACCESS:0);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=stride;
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;return SUCCEEDED(device->CreateBuffer(&d,data?&initial:nullptr,&out))&&SUCCEEDED(device->CreateShaderResourceView(out.Get(),nullptr,&srv));
        };
        // Every record is live in every cluster: the shader's own per-record
        // frustum and box test culls, which is correct but costs a test per
        // light per pixel. Real cluster addressing is the performance answer.
        const auto masks=state.clusterSummaries?Relight::ClusterSummaryFallback(state.clusterGrid):Relight::ClusterFallback(state.clusterGrid);
        if(!buffer(480,recordCount,nullptr,true,state.proofLights,state.proofLightSrv)||
           !buffer(480,recordCount,nullptr,false,state.proofAuthored,state.proofAuthoredSrv)||
           !buffer(4,UINT(masks.size()),masks.data(),true,state.proofMasks,state.proofMaskSrv))return false;
    }
    if(!state.proofBlackCube||!state.proofBlackCubeArray||!state.proofBlack2d||!state.proofBlackArray){
        const float black[4]{};D3D11_SUBRESOURCE_DATA initial[6]{};for(auto& i:initial){i.pSysMem=black;i.SysMemPitch=16;}
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> t;if(FAILED(device->CreateTexture2D(&d,initial,&t))||FAILED(device->CreateShaderResourceView(t.Get(),nullptr,&state.proofBlack2d)))return false;
        d.ArraySize=6;d.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;t.Reset();if(FAILED(device->CreateTexture2D(&d,initial,&t))||FAILED(device->CreateShaderResourceView(t.Get(),nullptr,&state.proofBlackCube)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC cube{};cube.Format=d.Format;cube.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        cube.TextureCubeArray.MipLevels=1;cube.TextureCubeArray.NumCubes=1;
        if(FAILED(device->CreateShaderResourceView(t.Get(),&cube,&state.proofBlackCubeArray)))return false;
    }
    if(!EnsureBlackArray(device))return false;
    // Owned irradiance for world t2. Prefer the engine atlas size once observed (Anubis is
    // 8192Â² BC6); fall back to the UV-diag size until the first lightmapped bind. Diag mode
    // always uses the small atlas so the checker stays a cheap CPU upload.
    state.lightmapAtlas=nullptr;
    if(relight&&!state.live&&!state.frame.relight.lightmapFile.empty()){
        Relight::LightmapParams params;
        params.mode=state.frame.relight.lightmapMode;
        params.fill=Relight::GroundAmbient(Relight::SunRadiance(state.frame.relight),.55f,state.frame.relight.indirect);
        params.generation=state.frame.relight.generation;
        state.lightmapAtlas=Relight::LightmapAtlas(device,ctx,params);
    }
    if(!state.lightmapAtlas&&!state.live){
        const bool diag=state.frame.relight.lightmapDiag;
        const unsigned wantW=diag?Relight::LightmapDiagSize:
            (state.engineLightmapWidth?state.engineLightmapWidth:Relight::LightmapDiagSize);
        const unsigned wantH=diag?Relight::LightmapDiagSize:
            (state.engineLightmapHeight?state.engineLightmapHeight:Relight::LightmapDiagSize);
        if(relight&&(!state.relightAmbient||state.relightAmbientWidth!=wantW||state.relightAmbientHeight!=wantH||
                     state.relightAmbientDiag!=diag)){
            state.relightAmbient.Reset();state.relightAmbientSrv.Reset();state.relightAmbientRtv.Reset();
            state.relightAmbientWidth=0;state.relightAmbientHeight=0;state.relightAmbientKey=~0ull;
            D3D11_TEXTURE2D_DESC d{};d.Width=wantW;d.Height=wantH;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            // Full engine atlases are huge; half-float keeps the SRV under a gigabyte while the
            // native shader still samples float4 irradiance.
            d.Format=wantW*wantH>Relight::LightmapDiagSize*Relight::LightmapDiagSize
                ?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&state.relightAmbient))||
               FAILED(device->CreateShaderResourceView(state.relightAmbient.Get(),&srv,&state.relightAmbientSrv))||
               FAILED(device->CreateRenderTargetView(state.relightAmbient.Get(),nullptr,&state.relightAmbientRtv)))return false;
            state.relightAmbientWidth=wantW;state.relightAmbientHeight=wantH;state.relightAmbientDiag=diag;
        }
    }
    if(!state.proofRebase){
        ComPtr<ID3DBlob> code,error;
        if(FAILED(CompileEnvironmentShader(LightingProofRebaseShader,sizeof(LightingProofRebaseShader)-1,"LightingProofRebase",nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&state.proofRebase)))return false;
    }
    if(!state.proofRebaseParams){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(device->CreateBuffer(&d,nullptr,&state.proofRebaseParams)))return false;}
    if(!state.proofLightUav&&FAILED(device->CreateUnorderedAccessView(state.proofLights.Get(),nullptr,&state.proofLightUav)))return false;
    if(!relight&&!state.proofPrepared){
        ID3D11ShaderResourceView* atlas=nullptr;
        if(!proofShadowPass||!proofShadowPass(ctx,state.frame,state.proofMatrices,&atlas)||!atlas)return false;
        state.proofAtlas=atlas;state.proofPrepared=true;
    }
    // Linear comparison filtering gives each tap a bilinear 2x2 of its own, so the nine
    // taps cover a smooth gradient rather than nine hard yes/no answers.
    if(!relight)for(unsigned i=0;i<7;++i){
        auto& record=records[i];record=ProofOmniRecord(state.frame.proof);
        std::copy(state.proofMatrices[i].begin(),state.proofMatrices[i].end(),record.begin());
        const unsigned x=i==6?3:i%3,y=i==6?0:i/3;
        record[52]=(x+.5f)/4;record[53]=(y+.5f)/2;record[54]=.5f/4;record[55]=-.5f/2;
        record[65]=state.frame.proof.shadows?1.f:0.f;
        if(i==6){record[32]=state.next.direction[0];record[33]=state.next.direction[1];record[34]=state.next.direction[2];record[35]=0;record[36]=1;record[37]=0;record[40]=state.next.radiance[0];record[41]=state.next.radiance[1];record[42]=state.next.radiance[2];record[65]=1;}
    }
    // Record private compute work on a deferred context. Executing with restore=TRUE
    // preserves all engine bindings without per-draw CSGetShader/class-instance traffic.
    if(!state.proofContext&&FAILED(device->CreateDeferredContext(0,&state.proofContext)))return false;
    auto work=state.proofContext.Get();
    if(relight&&!state.lightmapAtlas&&!state.live){
        const auto fill=Relight::GroundAmbient(Relight::SunRadiance(state.frame.relight),.55f,state.frame.relight.indirect);
        const bool diag=state.frame.relight.lightmapDiag;
        // Content key: generation covers dials; diag flips the UV pattern without waiting for
        // a light edit. Size tracks the owned atlas (engine match or diag).
        const uint64_t key=(state.frame.relight.generation<<1)|uint64_t(diag)|
            (uint64_t(state.relightAmbientWidth)<<16)|(uint64_t(state.relightAmbientHeight)<<32);
        if(state.relightAmbientKey!=key&&state.relightAmbient&&state.relightAmbientRtv){
            const unsigned width=state.relightAmbientWidth,height=state.relightAmbientHeight;
            if(diag){
                std::vector<float> texels(size_t(width)*height*4);
                for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                    float* rgba=&texels[(size_t(y)*width+x)*4];
                    Relight::LightmapDiagTexel(x,y,width,rgba);
                }
                work->UpdateSubresource(state.relightAmbient.Get(),0,nullptr,texels.data(),width*16,width*height*16);
            }else{
                // Engine atlases are thousands of texels on a side â€” clear on the GPU instead of
                // uploading a gigabyte of uniform float4s every generation.
                const float clear[4]{fill.x,fill.y,fill.z,1.f};
                work->ClearRenderTargetView(state.relightAmbientRtv.Get(),clear);
            }
            state.relightAmbientKey=key;state.relightAmbientDiag=diag;
            if(MvmDebugLog_Active())
                MvmDebugLog_LinefAlways("relight.lightmap","generation=%llu diag=%d size=%ux%u fill=(%.3f %.3f %.3f) engine=%ux%ux%u",
                    state.frame.relight.generation,int(diag),width,height,fill.x,fill.y,fill.z,
                    state.engineLightmapWidth,state.engineLightmapHeight,state.engineLightmapArray);
        }
    }
    if(!state.proofLocalLights){D3D11_BUFFER_DESC d{};d.ByteWidth=recordCount*480;d.StructureByteStride=480;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.proofLocalLights))||
           FAILED(device->CreateShaderResourceView(state.proofLocalLights.Get(),nullptr,&state.proofLocalLightSrv))||
           FAILED(device->CreateUnorderedAccessView(state.proofLocalLights.Get(),nullptr,&state.proofLocalLightUav)))return false;}
    if(!state.sunCascadeParams){D3D11_BUFFER_DESC d{};d.ByteWidth=4*sizeof(Matrix);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.sunCascadeParams)))return false;
        d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements=d.ByteWidth/4;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.sunCascadeColumns))||
           FAILED(device->CreateUnorderedAccessView(state.sunCascadeColumns.Get(),&u,&state.sunCascadeUav))||
           FAILED(device->CreateBuffer(&d,nullptr,&state.characterSunCascadeColumns))||
           FAILED(device->CreateUnorderedAccessView(state.characterSunCascadeColumns.Get(),&u,&state.characterSunCascadeUav)))return false;}
    Matrix sunMatrices[4]{};for(unsigned i=0;i<4;++i)sunMatrices[i]=state.proofMatrices[OwnedSunTiles[i]];
    Matrix skyboxMatrix{};
    if(viewport.MinDepth>0&&SkyboxSunShadow(&skyboxMatrix))
        for(auto& matrix:sunMatrices)matrix=skyboxMatrix;
    // Native fallback has no receiver-plane correction (weapons, blended draws
    // and invalid screen footprints). Five percent of a texel drew self-shadow
    // bands on Nuke ground and changed distant surfaces when the cache switched.
    // Keep the requested comparison bias up to a full texel for this path.
    const float sunTolerance=1.f; // native fallback must also tolerate curved world receivers
    for(auto& matrix:sunMatrices)matrix=NativeSunReceiverMatrix(matrix,state.frame.relight.shadowBias,float(Relight::SunCascadeSize),sunTolerance);
    const UINT rebaseParams[4]{recordCount,relight?1u:0u,0,0};
    work->UpdateSubresource(state.proofRebaseParams.Get(),0,nullptr,rebaseParams,0,0);
    const bool authoredChanged=!state.frame.relight.hoist||state.proofUploadedRecords.size()!=records.size()||
        std::memcmp(state.proofUploadedRecords.data(),records.data(),records.size()*sizeof(records[0]))!=0;
    if(authoredChanged)work->UpdateSubresource(state.proofAuthored.Get(),0,nullptr,records.data(),0,0);
    auto cb=state.proofRebaseParams.Get();auto native=view.Get();
    auto uav=state.proofLightUav.Get();auto authored=state.proofAuthoredSrv.Get();const UINT keep=~0u;
    // The same cascades for player bodies, moved toward the sun so a body never shadows itself (see
    // RelightSettings::characterSelfShadowUnits). Run FIRST: the rebase pass also writes the light
    // records, and the normal pass below must be the one whose values remain.
    Matrix characterMatrices[4]{};for(unsigned i=0;i<4;++i)characterMatrices[i]=state.proofMatrices[OwnedSunTiles[i]];
    if(viewport.MinDepth>0&&SkyboxSunShadow(&skyboxMatrix))for(auto& matrix:characterMatrices)matrix=skyboxMatrix;
    for(auto& matrix:characterMatrices)
        matrix=NativeSunTowardSun(NativeSunReceiverMatrix(matrix,CharacterLocalBias(state.frame.relight),float(Relight::SunCascadeSize)),
        state.frame.relight.characterSelfShadowUnits);
    const unsigned clusterMode=state.frame.relight.clusters==3?1:state.frame.relight.clusters;
    const bool safe=gridBudget&&relight&&state.frame.relight.cost==0&&clusterMode&&
        viewport.TopLeftX==0&&viewport.TopLeftY==0&&viewportDesc.ByteWidth>=21*16;
    const bool clustered=safe&&ClusterResources(device);
    const bool engineClustered=clustered&&state.frame.relight.clusters!=3&&state.nativeClusters.Ensure(device,recordCount);
    struct Params {Relight::ClusterUintRow grid,list;Relight::ClusterFloatRow depth;Relight::ClusterUintRow control;
        Relight::ClusterFloatRow nearby;Relight::ClusterUintRow nearbyList;};
    const bool firstPerson=viewport.MaxDepth<=.1f;
    const Params params{{grid.shift,grid.tilesX,grid.tilesY,grid.sliceCount},
        {recordCount,grid.lights.words,grid.lights.tiles,grid.lights.slices},
        {grid.sliceScale,float(grid.sliceCount-1),viewport.Width,viewport.Height},
        {clusterMode,grid.lights.ones,firstPerson?1u:0u,state.clusterSummaries?grid.size:0u},
        {state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,Relight::FirstPersonLightReach},
        {firstPerson?grid.lights.nearby:0u,0,0,0}};
    const UINT fillGroups=(grid.TileCount()+grid.sliceCount+1+63)/64;
    std::vector<uint8_t> viewKey;
    auto key=[&](const void* data,size_t bytes){const auto* p=static_cast<const uint8_t*>(data);viewKey.insert(viewKey.end(),p,p+bytes);};
    key(sunMatrices,sizeof(sunMatrices));key(characterMatrices,sizeof(characterMatrices));key(rebaseParams,sizeof(rebaseParams));
    key(&params,sizeof(params));key(&viewport,sizeof(viewport));key(&clustered,sizeof(clustered));key(&engineClustered,sizeof(engineClustered));
    const bool gated=relight&&state.frame.relight.hoist&&state.proofViewGate.Prepare(device,work,view.Get(),nativeViewport.Get(),viewKey,
        {(recordCount+63)/64,clustered?(recordCount+63)/64:0u,clustered?fillGroups:0u},authoredChanged,engineClustered?Relight::NativeClusterDispatches(grid):std::array<UINT,4>{});
    if(!gated)state.proofViewGate.Invalidate();
    auto dispatch=[&](unsigned pass,UINT groups){if(gated)work->DispatchIndirect(state.proofViewGate.Arguments(),pass*12);else work->Dispatch(groups,1,1);};
    {
        work->UpdateSubresource(state.sunCascadeParams.Get(),0,nullptr,characterMatrices,0,0);
        work->CSSetShader(state.proofRebase.Get(),nullptr,0);
        cb=state.proofRebaseParams.Get();work->CSSetConstantBuffers(0,1,&cb);
        cb=state.sunCascadeParams.Get();work->CSSetConstantBuffers(2,1,&cb);
        work->CSSetConstantBuffers(4,1,&native);work->CSSetShaderResources(0,1,&authored);
        ID3D11UnorderedAccessView* characterOutputs[]{state.characterSunCascadeUav.Get(),state.proofLocalLightUav.Get()};
        work->CSSetUnorderedAccessViews(1,2,characterOutputs,nullptr);
        auto lightUav=state.proofLightUav.Get();work->CSSetUnorderedAccessViews(0,1,&lightUav,&keep);
        dispatch(0,(recordCount+63)/64);
        ID3D11UnorderedAccessView* clearCharacter[2]{};work->CSSetUnorderedAccessViews(1,2,clearCharacter,nullptr);
    }
    work->UpdateSubresource(state.sunCascadeParams.Get(),0,nullptr,sunMatrices,0,0);
    cb=state.proofRebaseParams.Get();
    work->CSSetShader(state.proofRebase.Get(),nullptr,0);work->CSSetConstantBuffers(0,1,&cb);
    work->CSSetConstantBuffers(4,1,&native);work->CSSetShaderResources(0,1,&authored);
    cb=state.sunCascadeParams.Get();work->CSSetConstantBuffers(2,1,&cb);
    ID3D11UnorderedAccessView* sunOutputs[]{state.sunCascadeUav.Get(),state.proofLocalLightUav.Get()};
    work->CSSetUnorderedAccessViews(1,2,sunOutputs,nullptr);
    work->CSSetUnorderedAccessViews(0,1,&uav,&keep);dispatch(0,(recordCount+63)/64);
    ID3D11UnorderedAccessView* noSunOutputs[2]{};work->CSSetUnorderedAccessViews(1,2,noSunOutputs,nullptr);
    if(clustered) {
        work->UpdateSubresource(state.clusterParams.Get(),0,nullptr,&params,0,0);
        cb=state.clusterParams.Get();auto b1=nativeViewport.Get();
        work->CSSetConstantBuffers(0,1,&cb);work->CSSetConstantBuffers(1,1,&b1);
        uav=state.clusterExtentUav.Get();work->CSSetUnorderedAccessViews(0,1,&uav,&keep);
        auto rowsUav=state.clusterRowsUav.Get();work->CSSetUnorderedAccessViews(2,1,&rowsUav,&keep);
        work->CSSetShader(state.clusterBounds.Get(),nullptr,0);dispatch(1,(recordCount+63)/64);
        uav=nullptr;work->CSSetUnorderedAccessViews(0,1,&uav,&keep);
        auto extents=state.clusterExtentSrv.Get();work->CSSetShaderResources(1,1,&extents);
        if(engineClustered){
            state.nativeClusters.Record(work,grid,state.clusterParams.Get(),authored,extents,state.clusterMaskUav.Get(),gated?state.proofViewGate.Arguments():nullptr);
        }else{
            uav=state.clusterMaskUav.Get();work->CSSetUnorderedAccessViews(1,1,&uav,&keep);
            work->CSSetShader(state.clusterFill.Get(),nullptr,0);dispatch(2,fillGroups);
        }
        if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();
            if(now-last>=3000){last=now;MvmDebugLog_LinefAlways("relight.cluster.engine","native=%u records=%u words=%u tiles=%ux%u slices=%u gated=%u",
                unsigned(engineClustered),recordCount,grid.lights.words,grid.tilesX,grid.tilesY,grid.sliceCount,unsigned(gated));}}
        state.clusterMode=clusterMode;
    }
    if(MvmDebugLog_Active()&&!state.clusterLogged&&!state.clusterReadbackPending&&viewportDesc.ByteWidth>=21*16) {
        auto staging=[&](UINT bytes,ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            return SUCCEEDED(device->CreateBuffer(&d,nullptr,out.ReleaseAndGetAddressOf()));
        };
        if(staging(vd.ByteWidth,state.clusterViewReadback)&&staging(viewportDesc.ByteWidth,state.clusterViewportReadback)) {
            work->CopyResource(state.clusterViewReadback.Get(),view.Get());
            work->CopyResource(state.clusterViewportReadback.Get(),nativeViewport.Get());
            state.clusterReadbackPending=true;
        }
    }
    ComPtr<ID3D11CommandList> commands;
    if(FAILED(work->FinishCommandList(FALSE,&commands))){state.proofViewGate.Invalidate();return false;}
    ctx->ExecuteCommandList(commands.Get(),TRUE);
    if(engineClustered)state.nativeClusters.Commit();
    if(gated)state.proofViewGate.Commit();
    if(authoredChanged){state.proofUploadedRecords=records;++state.proofAuthoredUploads;state.proofAuthoredBytes+=records.size()*sizeof(records[0]);}
    if(relight){
        const Vec3 low=state.frame.occlusion?state.frame.occlusion->low:Vec3{-32768,-32768,-32768};
        const Vec3 high=state.frame.occlusion?state.frame.occlusion->high:Vec3{32768,32768,32768};
        state.probeBinding=Relight::AmbientProbe(device,low,high,
            Relight::SkyAmbient(state.frame.relight,state.frame.relight.indirect),{});
    }
    if(relight&&state.frame.relight.probeCheck==2&&state.frame.occlusion) {
        const auto margin=Vec3{32,32,32};
        state.probeBinding=Relight::NeutralProbe(device,state.frame.occlusion->low-margin,state.frame.occlusion->high+margin);
    }
    state.probeTraced=false;
    if(relight&&state.frame.relight.gi&&(!state.frame.relight.probeCheck||state.frame.relight.probeCheck>=3)&&state.frame.relight.cost==0) {
        const auto traced=Relight::PrepareProbeGi(device,ctx,state.frame.occlusion,state.frame.relight,
            records,state.proofAtlas.Get(),state.proofComparison.Get(),state.probeFrame,state.proofStaticLights,state.frame.camera,state.glassCookies,
            ViewForward(state.frame.angles));
        if(traced.constants){state.probeBinding=traced;state.probeTraced=true;}
        // Diagnostics 5/6: the traced binding with the ambient probe's texture, or with its constants.
        if(traced.constants&&state.frame.relight.probeCheck>=5){
            const Vec3 low=state.frame.occlusion?state.frame.occlusion->low:Vec3{-32768,-32768,-32768},high=state.frame.occlusion?state.frame.occlusion->high:Vec3{32768,32768,32768};
            const auto ambient=Relight::AmbientProbe(device,low,high,Relight::SkyAmbient(state.frame.relight,state.frame.relight.indirect),{});
            if(state.frame.relight.probeCheck==5)state.probeBinding.irradiance=ambient.irradiance;
            else{state.probeBinding.constants=ambient.constants;state.probeBinding.packing=ambient.packing;}
        }
    }
    if(relight){
        const auto skybox=state.frame.occlusion?state.frame.occlusion->skybox:nullptr;
        const Vec3 low=skybox?skybox->low:Vec3{-32768,-32768,-32768},high=skybox?skybox->high:Vec3{32768,32768,32768};
        state.skyboxBinding=Relight::SkyboxAmbientProbe(device,low,high,
            Relight::SkyAmbient(state.frame.relight,state.frame.relight.indirect),{});
    }
    state.proofPreparedView=view;state.proofPreparedViewport=nativeViewport;state.proofViewport=viewport;
    state.proofPreparedGeneration=generation;state.proofResourcesReady=true;++state.proofUploads;
    return true;
}
bool ResolveMaterial(ID3D11DeviceContext* ctx){
    RenderPrivateTextureScope privateTextures;
    if(!state.color)return false;
    D3D11_TEXTURE2D_DESC d{},old{};state.color->GetDesc(&d);if(state.resolved)state.resolved->GetDesc(&old);
    if(!state.resolved||old.Width!=d.Width||old.Height!=d.Height||old.Format!=d.Format){
        state.resolved.Reset();state.resolvedSrv.Reset();auto single=d;single.SampleDesc={1,0};single.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateTexture2D(&single,nullptr,&state.resolved))||FAILED(device->CreateShaderResourceView(state.resolved.Get(),nullptr,&state.resolvedSrv)))return false;
    }
    if(d.SampleDesc.Count>1)ctx->ResolveSubresource(state.resolved.Get(),0,state.color.Get(),0,d.Format);
    else ctx->CopyResource(state.resolved.Get(),state.color.Get());
    D3D11_TEXTURE2D_DESC oldBase{};if(state.baselineResolved)state.baselineResolved->GetDesc(&oldBase);
    if(!state.baselineResolved||oldBase.Width!=d.Width||oldBase.Height!=d.Height||oldBase.Format!=d.Format){
        state.baselineResolved.Reset();state.baselineResolvedSrv.Reset();auto single=d;single.SampleDesc={1,0};single.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateTexture2D(&single,nullptr,&state.baselineResolved))||FAILED(device->CreateShaderResourceView(state.baselineResolved.Get(),nullptr,&state.baselineResolvedSrv)))return false;
    }
    if(d.SampleDesc.Count>1)ctx->ResolveSubresource(state.baselineResolved.Get(),0,state.baseline.Get(),0,d.Format);
    else ctx->CopyResource(state.baselineResolved.Get(),state.baseline.Get());
    if(state.fixturePass){
        D3D11_TEXTURE2D_DESC prior{};if(state.withoutFixturesResolved)state.withoutFixturesResolved->GetDesc(&prior);
        if(!state.withoutFixturesResolved||prior.Width!=d.Width||prior.Height!=d.Height||prior.Format!=d.Format){
            state.withoutFixturesResolved.Reset();state.withoutFixturesResolvedSrv.Reset();
            auto single=d;single.SampleDesc={1,0};single.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            if(FAILED(device->CreateTexture2D(&single,nullptr,&state.withoutFixturesResolved))||
               FAILED(device->CreateShaderResourceView(state.withoutFixturesResolved.Get(),nullptr,&state.withoutFixturesResolvedSrv)))return false;
        }
        if(d.SampleDesc.Count>1)ctx->ResolveSubresource(state.withoutFixturesResolved.Get(),0,state.withoutFixtures.Get(),0,d.Format);
        else ctx->CopyResource(state.withoutFixturesResolved.Get(),state.withoutFixtures.Get());
    }
    D3D11_TEXTURE2D_DESC mask{},prior{};state.coverage->GetDesc(&mask);
    if(state.coverageResolved)state.coverageResolved->GetDesc(&prior);
    if(!state.coverageResolved||prior.Width!=mask.Width||prior.Height!=mask.Height){
        state.coverageResolved.Reset();state.coverageResolvedSrv.Reset();mask.SampleDesc={1,0};mask.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateTexture2D(&mask,nullptr,&state.coverageResolved))||
            FAILED(device->CreateShaderResourceView(state.coverageResolved.Get(),nullptr,&state.coverageResolvedSrv)))return false;
    }
    state.coverage->GetDesc(&mask);
    if(mask.SampleDesc.Count>1)ctx->ResolveSubresource(state.coverageResolved.Get(),0,state.coverage.Get(),0,mask.Format);
    else ctx->CopyResource(state.coverageResolved.Get(),state.coverage.Get());
    return true;
}
bool BeginTonemap(ID3D11DeviceContext* ctx,const D3D11_VIEWPORT& vp){
    RenderPrivateTextureScope privateTextures;
    if(!state.captured||state.toned)return false;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);char name[128]{};UINT size=127;
    if(!ps||FAILED(ps->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name))||strcmp(name,"post_process.vfx_ps"))return false;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);size=127;std::memset(name,0,sizeof(name));
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name))||strcmp(name,"post_process.vfx_vs"))return false;
    // The shipped post_process shader consumes the scene's resolved HDR at t12.
    // It performs exposure, filmic tonemapping, the color LUT and output transfer.
    ComPtr<ID3D11ShaderResourceView> input;ctx->PSGetShaderResources(12,1,&input);if(!input)return false;
    ComPtr<ID3D11Resource> inputResource;input->GetResource(&inputResource);ComPtr<ID3D11Texture2D> inputTexture;
    if(FAILED(inputResource.As(&inputTexture)))return false;
    D3D11_TEXTURE2D_DESC inputDesc{},hdr{};inputTexture->GetDesc(&inputDesc);state.source->GetDesc(&hdr);
    if(inputDesc.Width!=hdr.Width||inputDesc.Height!=hdr.Height||inputDesc.SampleDesc.Count!=1||inputDesc.ArraySize!=1||
       (inputDesc.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS&&inputDesc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)||
       vp.TopLeftX!=0||vp.TopLeftY!=0||vp.Width!=hdr.Width||vp.Height!=hdr.Height){Evidence("post-input-layout");return false;}
    ComPtr<ID3D11RenderTargetView> targets[8];ComPtr<ID3D11DepthStencilView> depth;ID3D11RenderTargetView* raw[8]{};
    ctx->OMGetRenderTargets(8,raw,&depth);for(unsigned i=0;i<8;++i)targets[i].Attach(raw[i]);
    if(!targets[0])return false;for(unsigned i=1;i<8;++i)if(targets[i])return false;
    ComPtr<ID3D11Resource> outputResource;targets[0]->GetResource(&outputResource);ComPtr<ID3D11Texture2D> output;
    if(FAILED(outputResource.As(&output)))return false;
    D3D11_TEXTURE2D_DESC td{},old{};D3D11_RENDER_TARGET_VIEW_DESC rt{};output->GetDesc(&td);targets[0]->GetDesc(&rt);
    if(td.Width!=hdr.Width||td.Height!=hdr.Height||td.ArraySize!=1||td.MipLevels!=1||
       (rt.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&rt.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)){Evidence("post-output-layout");return false;}
    ComPtr<ID3D11DepthStencilState> depthState;UINT stencil=0;ctx->OMGetDepthStencilState(&depthState,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};if(depthState)depthState->GetDesc(&dd);
    if(depth&&(!depthState||dd.DepthEnable||dd.StencilEnable)){Evidence("post-depth-enabled");return false;}
    ComPtr<ID3D11BlendState> blend;float factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);
    if(blend){D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);if(bd.AlphaToCoverageEnable||bd.RenderTarget[0].BlendEnable)return false;}
    if(!ResolveMaterial(ctx))return false;
    if(state.frame.relight.reference&&MvmDebugLog_Active()) {
        static uint64_t sampled=~0ull;
        if(sampled!=state.frame.relight.generation) {
            sampled=state.frame.relight.generation;
            auto probe=[&](ID3D11Texture2D* texture,const char* label) {
                D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
                d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
                ComPtr<ID3D11Device> device;ctx->GetDevice(&device);ComPtr<ID3D11Texture2D> copy;
                if(FAILED(device->CreateTexture2D(&d,nullptr,&copy)))return;
                ctx->CopyResource(copy.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
                if(FAILED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
                for(unsigned x:{d.Width/4,3*d.Width/4}) {
                    const auto* pixel=reinterpret_cast<const unsigned short*>(static_cast<const char*>(mapped.pData)+(d.Height/2)*mapped.RowPitch+x*8);
                    MvmDebugLog_LinefAlways("relight.pipeline.reference","stage=%s x=%u y=%u format=%u half=(%04x %04x %04x %04x)",label,x,d.Height/2,d.Format,pixel[0],pixel[1],pixel[2],pixel[3]);
                }
                ctx->Unmap(copy.Get(),0);
            };
            probe(inputTexture.Get(),"native-post-input");probe(state.resolved.Get(),"native-material-replay");
        }
    }
    if((state.frame.proof.enabled||state.frame.relight.enabled)&&MvmDebugLog_Active()){
        static uint64_t sampled=~0ull;
        const uint64_t coverageGeneration=state.frame.relight.enabled?state.frame.relight.generation:state.frame.proof.generation;
        if(sampled!=coverageGeneration){
            sampled=coverageGeneration;
            RenderPrivateTextureScope scope;D3D11_TEXTURE2D_DESC d{};state.coverageResolved->GetDesc(&d);
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);ComPtr<ID3D11Texture2D> copy;
            if(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&copy))){
                ctx->CopyResource(copy.Get(),state.coverageResolved.Get());D3D11_MAPPED_SUBRESOURCE m{};
                if(SUCCEEDED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&m))){
                    UINT pixels=0;for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x)pixels+=static_cast<const unsigned char*>(m.pData)[y*m.RowPitch+x]>0;
                    ctx->Unmap(copy.Get(),0);MvmDebugLog_LinefAlways("environment.proof.coverage","generation=%llu coveredPixels=%u totalPixels=%u",sampled,pixels,d.Width*d.Height);
                }
            }
        }
    }

    if(hdrPass){
        D3D11_TEXTURE2D_DESC prior{};if(state.composite)state.composite->GetDesc(&prior);
        if(!state.composite||prior.Width!=inputDesc.Width||prior.Height!=inputDesc.Height){
            state.composite.Reset();state.compositeRtv.Reset();state.compositeSrv.Reset();auto d=inputDesc;
            d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            if(FAILED(device->CreateTexture2D(&d,nullptr,&state.composite))||FAILED(device->CreateRenderTargetView(state.composite.Get(),nullptr,&state.compositeRtv))||
                FAILED(device->CreateShaderResourceView(state.composite.Get(),nullptr,&state.compositeSrv)))return false;
        }
        ctx->CopyResource(state.composite.Get(),inputTexture.Get());
        auto frame=state.frame;frame.hdr=true;frame.nativeLightingPass=true;
        UINT compressed=0,tagBytes=sizeof(compressed);
        frame.nativeCompressedHdr=SUCCEEDED(ps->GetPrivateData(PostCompressionTag,&tagBytes,&compressed))&&compressed==1;
        if(MvmDebugLog_Active()){static int last=-1;if(last!=int(frame.nativeCompressedHdr)){last=int(frame.nativeCompressedHdr);MvmDebugLog_LinefAlways("relight.pipeline","nativePostCompressedHdr=%d",last);}}
        if(!hdrPass(ctx,state.compositeRtv.Get(),state.sourceDsv.Get(),state.source.Get(),frame))return false;
        state.previousRtv=targets[0];state.previousDsv=depth;state.previousInput=input;state.tonedSource=output;
        auto replacement=state.compositeSrv.Get();ctx->PSSetShaderResources(12,1,&replacement);
        state.postActive=state.active=state.composited=true;BeginTiming(ctx,true);return true;
    }
    if(state.tonedColor)state.tonedColor->GetDesc(&old);
    if(!state.tonedColor||old.Width!=td.Width||old.Height!=td.Height||old.Format!=rt.Format||old.SampleDesc.Count!=td.SampleDesc.Count||old.SampleDesc.Quality!=td.SampleDesc.Quality){
        state.tonedColor.Reset();state.tonedRtv.Reset();state.tonedSrv.Reset();auto d=td;d.Format=rt.Format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateTexture2D(&d,nullptr,&state.tonedColor))||FAILED(device->CreateRenderTargetView(state.tonedColor.Get(),nullptr,&state.tonedRtv))||FAILED(device->CreateShaderResourceView(state.tonedColor.Get(),nullptr,&state.tonedSrv)))return false;
    }
    state.previousRtv=targets[0];state.previousDsv=depth;state.previousInput=input;state.tonedSource=output;
    BeginTiming(ctx,true);
    auto target=state.tonedRtv.Get();auto replacement=state.resolvedSrv.Get();ctx->OMSetRenderTargets(1,&target,depth.Get());ctx->PSSetShaderResources(12,1,&replacement);
    state.postActive=state.active=true;return true;
}

}
void NoteNativeMaterialPixelShader(ID3D11PixelShader* shader,const void* code,size_t bytes){
    static thread_local bool creatingCoverage=false;
    if(!shader||creatingCoverage)return;NativeSunAbiInfo info{};
    if(NativePostUsesCompressedHdr(code,bytes)){const UINT encoded=1;shader->SetPrivateData(PostCompressionTag,sizeof(encoded),&encoded);nativePostCompressedHdr.store(true,std::memory_order_relaxed);}
    const bool sunAbi=HasNativeSunMaterialAbi(code,bytes,&info);
    auto shareLighting=[&](ID3D11Device* device,ID3D11PixelShader* original,const std::vector<uint8_t>& program){
        auto create=[&](const std::vector<uint8_t>& candidate,ComPtr<ID3D11PixelShader>& shared){
            std::vector<uint8_t> sharedCode;NativeSharedLightingInfo layout;
            if(!NativeSharedLightingProgram(candidate.data(),candidate.size(),sharedCode,layout))return false;
            creatingCoverage=true;const auto hr=device->CreatePixelShader(sharedCode.data(),sharedCode.size(),nullptr,&shared);creatingCoverage=false;
            if(FAILED(hr))return false;shared->SetPrivateData(SharedLightingInfoTag,sizeof(layout),&layout);return true;
        };
        ComPtr<ID3D11PixelShader> shared;
        if(create(program,shared))::Filmmaker::SetOwnedInterface(original,SharedLightingShaderTag,shared.Get());
        std::vector<uint8_t> sparseCode;
        if(NativeSparseClusterProgram(program.data(),program.size(),sparseCode)){
            shared.Reset();if(create(sparseCode,shared)){
                const UINT enabled=1;shared->SetPrivateData(SparseClusterInfoTag,sizeof(enabled),&enabled);
                ::Filmmaker::SetOwnedInterface(original,SparseClusterShaderTag,shared.Get());
            }
        }
    };
    auto shareAo=[&](ID3D11Device* device,ID3D11PixelShader* original,const std::vector<uint8_t>& program){
        std::vector<uint8_t> sharedCode;
        if(!NativeSharedAoProgram(program.data(),program.size(),sharedCode)){shareLighting(device,original,program);return;}
        ComPtr<ID3D11PixelShader> shared;creatingCoverage=true;
        const auto hr=device->CreatePixelShader(sharedCode.data(),sharedCode.size(),nullptr,&shared);creatingCoverage=false;
        if(SUCCEEDED(hr)){shareLighting(device,shared.Get(),sharedCode);::Filmmaker::SetOwnedInterface(original,SharedAoShaderTag,shared.Get());}
    };
    if(sunAbi||info.lightSlot){
        std::vector<uint8_t> atmosphereCode;
        bool ownedCascades=false;uint32_t screenShadowSlot=0;
        const bool atmosphere=NativeAtmosphereProgram(code,bytes,atmosphereCode,true,sunAbi?&ownedCascades:nullptr,&screenShadowSlot);
        if(atmosphere){
            ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> owned;
            creatingCoverage=true;const auto hr=device->CreatePixelShader(atmosphereCode.data(),atmosphereCode.size(),nullptr,&owned);creatingCoverage=false;
            if(SUCCEEDED(hr)){shareAo(device.Get(),owned.Get(),atmosphereCode);::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),NativeAtmosphereTag,owned.Get());info.ownedCascades=ownedCascades;info.screenShadowSlot=screenShadowSlot;}
            MvmDebugLog_LinefAlways("relight.atmosphere.shader","hash=%016llx valid=%d hr=%08x",info.programHash,SUCCEEDED(hr),unsigned(hr));
        }
        if((info.lightmapArrayMask&2)||info.probeSlot){
            std::vector<uint8_t> dynamicCode;Relight::IrradianceProgramInfo query;
            if(Relight::DynamicIrradianceProgram(atmosphere?atmosphereCode.data():code,
                atmosphere?atmosphereCode.size():bytes,dynamicCode,&query,info.probeSlot)){
                ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> dynamic;
                creatingCoverage=true;
                const auto hr=device->CreatePixelShader(dynamicCode.data(),dynamicCode.size(),nullptr,&dynamic);
                creatingCoverage=false;
                if(SUCCEEDED(hr)){
                    info.ownedCascades=ownedCascades;info.screenShadowSlot=screenShadowSlot;
                    shareAo(device.Get(),dynamic.Get(),dynamicCode);
                    dynamic->SetPrivateData(SunAbiInfoTag,sizeof(info),&info);
                    ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),DynamicIrradianceTag,dynamic.Get());
                }
                MvmDebugLog_LinefAlways("relight.irradiance.shader","hash=%016llx queries=%u evaluations=%u position=%u normal=%u valid=%d hr=%08x",
                    info.programHash,query.queries,query.evaluations,query.position,query.normal,SUCCEEDED(hr),unsigned(hr));
            }else MvmDebugLog_LinefAlways("relight.irradiance.shader","hash=%016llx unsupported=1 bakedFallback=0",info.programHash);
        }
        std::vector<uint8_t> coverageCode;
        if(NativeCoverageProgram(code,bytes,coverageCode)){
            ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> coverage;
            creatingCoverage=true;
            const auto hr=device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coverage);
            creatingCoverage=false;
            if(SUCCEEDED(hr)){
                ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CoverageShaderTag,coverage.Get());
                const UINT valid=1;shader->SetPrivateData(SunAbiTag,sizeof(valid),&valid);
            }
            if(NativeCoverageProgram(code,bytes,coverageCode,true)){
                coverage.Reset();creatingCoverage=true;
                const auto blackHr=device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coverage);
                creatingCoverage=false;
                if(SUCCEEDED(blackHr))::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),BlackCoverageShaderTag,coverage.Get());
            }
        }
    }
    shader->SetPrivateData(SunAbiInfoTag,sizeof(info),&info);
    if(info.cb3Count==1990&&!info.lightSlot&&info.flags==31){
        ComPtr<ID3DBlob> kernel,error;
        if(SUCCEEDED(CompileEnvironmentShader(NativeSmokeLightingShader,sizeof(NativeSmokeLightingShader)-1,
            "EnvironmentNativeSmoke",nullptr,"main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&kernel,&error))){
            std::vector<uint8_t> patched;unsigned queries=0;
            if(NativeSmokeProgram(code,bytes,kernel->GetBufferPointer(),kernel->GetBufferSize(),patched,&queries)){
                ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> smoke;
                creatingCoverage=true;const auto hr=device->CreatePixelShader(patched.data(),patched.size(),nullptr,&smoke);creatingCoverage=false;
                if(SUCCEEDED(hr))::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),NativeSmokeTag,smoke.Get());
                MvmDebugLog_LinefAlways("environment.smoke.shader","hash=%016llx queries=%u hr=%08x",info.programHash,queries,unsigned(hr));
            }
        }else MvmDebugLog_LinefAlways("environment.smoke.shader","compile=%s",error?static_cast<const char*>(error->GetBufferPointer()):"failed");
    }
    // Development aid: keep the bytecode of every clustered-light program so it can be written out
    // and disassembled (mirv_relight dumpprograms). Capped, and only programs that carry the ABI.
    if(info.lightSlot){
        static std::mutex lock;std::lock_guard<std::mutex> guard(lock);
        static size_t total=0;
        if(programRecord().size()<8192&&total<512u*1024*1024&&!programRecord().count(info.programHash)){
            programRecord()[info.programHash]=std::vector<uint8_t>(static_cast<const uint8_t*>(code),static_cast<const uint8_t*>(code)+bytes);
            total+=bytes;
        }
    }
}
unsigned DumpNativePrograms(const char* directory){
    unsigned written=0;
    for(const auto& entry:programRecord()){
        char path[512];sprintf_s(path,"%s/%016llx.dxbc",directory,static_cast<unsigned long long>(entry.first));
        FILE* file=nullptr;if(fopen_s(&file,path,"wb")||!file)continue;
        fwrite(entry.second.data(),1,entry.second.size(),file);fclose(file);++written;
    }
    return written;
}
void ResetNativeMaterial(){Relight::EndProbes();state=State{};}
void SetNativeMaterialHdrPass(NativeMaterialHdrPass pass){hdrPass=pass;}
void SetLightingProofShadowPass(LightingProofShadowPass pass){proofShadowPass=pass;}
void SetMapLightShadowPass(MapLightShadowPass pass){mapShadowPass=pass;}
void SetSkyVisibilitySource(SkyVisibilitySource source){skyVisibilitySource=source;}
void SetViewmodelShadowPass(ViewmodelShadowPass pass){viewmodelShadowPass=pass;}
void SetGlassBeamShadowPass(GlassBeamShadowPass pass){glassBeamShadowPass=pass;}
unsigned RelightClusterMode(){return state.frame.relight.enabled?state.clusterMode:0;}
unsigned BindRelightRecords(ID3D11DeviceContext* ctx){
    // The diagnostic views evaluate the records against reconstructed world
    // positions, so they need the authored copy rather than the camera-relative
    // one the material shaders consume.
    const bool ready=state.frame.relight.enabled&&state.proofAuthoredSrv&&state.proofRecordCount;
    ID3D11ShaderResourceView* view=ready?state.proofAuthoredSrv.Get():nullptr;
    if(ctx) {
        ctx->PSSetShaderResources(26,1,&view);
        ID3D11ShaderResourceView* clusterViews[2]{ready&&state.clusterMode?state.proofMaskSrv.Get():nullptr,
                                              ready&&state.clusterMode?state.clusterRowsSrv.Get():nullptr};
        ctx->PSSetShaderResources(27,2,clusterViews);
        auto volume=state.probeBinding.irradiance;auto constants=state.probeBinding.constants;
        auto sampler=state.probeBinding.sampler;
        ctx->PSSetShaderResources(29,1,&volume);ctx->PSSetConstantBuffers(6,1,&constants);ctx->PSSetSamplers(5,1,&sampler);
    }
    return ready?state.proofRecordCount:0u;
}
bool NativeMaterialComposited(const Frame& frame){return state.composited&&frame.generation==state.renderedGeneration;}
bool NativeSkyApplied(const Frame& frame){return state.skyApplied&&frame.generation==state.skyGeneration;}
bool SkipNativeSkyEffect(ID3D11DeviceContext* ctx,unsigned vertices,unsigned instances){
    if(!ctx||!NativeSkyApplied(state.frame))return false;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    if(count!=1||(vp.MinDepth<.95f&&!MvmDebugLog_Active()))return false;
    if(!SkyGpuProbe_IsPlayersOnlyGreenSkyDraw(ctx,vertices,instances))return false;
    const bool skyInterval=vp.MinDepth>=.95f&&vp.MaxDepth==1.f;
    if(MvmDebugLog_Active()){
        static bool logged[2]{};auto& once=logged[skyInterval?1:0];
        if(!once){once=true;MvmDebugLog_LinefAlways("environment.native.sky-effect",
            "vertices=%u instances=%u viewportDepth=%.6f..%.6f skip=%d",vertices,instances,vp.MinDepth,vp.MaxDepth,skyInterval);}
    }
    return skyInterval;
}
void NextNativeMaterialFrame(){
    {
        static double ms=0;if(ms==0){LARGE_INTEGER f;QueryPerformanceFrequency(&f);ms=1000.0/double(f.QuadPart);}
        const float frame[3]{float(liveLaps[1]*ms),float(liveLaps[2]*ms),float((liveLaps[0]+liveLaps[1]+liveLaps[2])*ms)};
        for(unsigned i=0;i<3;++i){const float old=liveCostMs[i].load(std::memory_order_relaxed);liveCostMs[i].store(old+(frame[i]-old)*.05f,std::memory_order_relaxed);}
        liveLaps.fill(0);
    }
    if(MvmDebugLog_Active()&&state.proofUploads){
        static ULONGLONG last=0;const auto now=GetTickCount64();
        if(now-last>3000){last=now;MvmDebugLog_LinefAlways("relight.prepare",
            "generation=%llu uploads=%u records=%u bytes=%zu draws=%u worldDraws=%u hoist=%u cost=%u authoredUploads=%u",
            state.proofPreparedGeneration,state.proofUploads,state.proofRecordCount,
            state.proofAuthoredBytes,state.draws,state.worldDraws,
            unsigned(state.frame.relight.hoist),state.frame.relight.cost,state.proofAuthoredUploads);}
    }
    state.sharedLightingUsed=0;
    NextTimingFrame();state.wanted=state.requested;state.requested=false;state.captured=false;state.toned=false;state.composited=false;state.draws=state.worldDraws=state.fixtureDraws=0;}
NativeMaterialSunScope::NativeMaterialSunScope(ID3D11DeviceContext* ctx,bool enabled) {
    if(!enabled||!ctx||!state.wanted||state.active||state.frame.relight.enabled||state.frame.proof.enabled||
       ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    const bool viewmodel=std::abs(vp.MaxDepth-.1f)<1e-6f;
    if(count!=1||vp.Width<256||vp.Height<256||vp.MinDepth!=0||(!viewmodel&&std::abs(vp.MaxDepth-.95f)>1e-6f))return;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);char name[128]{};UINT size=sizeof(name)-1;
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name)))return;
    const bool supported=viewmodel?(!strcmp(name,"csgo_weapon.vfx_vs")||!strcmp(name,"csgo_character.vfx_vs")):
        (!strcmp(name,"csgo_lightmappedgeneric.vfx_vs")||!strcmp(name,"csgo_vertexlitgeneric.vfx_vs")||
         !strcmp(name,"csgo_complex.vfx_vs")||!strcmp(name,"csgo_simple.vfx_vs")||!strcmp(name,"csgo_simple_2way_blend.vfx_vs")||!strcmp(name,"csgo_static_overlay.vfx_vs"));
    if(!supported)return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);UINT tag=0;size=sizeof(tag);
    if(!ps||FAILED(ps->GetPrivateData(SunAbiTag,&size,&tag))||tag!=1)return;
    ComPtr<ID3D11PixelShader> coverage;size=sizeof(ID3D11PixelShader*);
    if(FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),CoverageShaderTag,&size,(void**)coverage.GetAddressOf()))||!coverage)return;
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
    ComPtr<ID3D11HullShader> hs;ctx->HSGetShader(&hs,nullptr,nullptr);
    ComPtr<ID3D11DomainShader> ds;ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds)return;
    ComPtr<ID3D11BlendState> blend;ctx->OMGetBlendState(&blend,nullptr,nullptr);
    D3D11_BLEND_DESC blending{};if(blend)blend->GetDesc(&blending);
    if(blending.IndependentBlendEnable)return;
    ComPtr<ID3D11RenderTargetView> targets[8];ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* rawTargets[8]{};ctx->OMGetRenderTargets(8,rawTargets,depth.GetAddressOf());
    for(unsigned i=0;i<8;++i)targets[i].Attach(rawTargets[i]);
    if(!targets[0]||!depth)return;
    for(unsigned i=1;i<8;++i)if(targets[i])return;
    ComPtr<ID3D11Resource> resource;targets[0]->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.ArraySize!=1||td.MipLevels!=1||td.Width!=UINT(vp.Width)||td.Height!=UINT(vp.Height))return;
    ComPtr<ID3D11DepthStencilState> depthState;ctx->OMGetDepthStencilState(&depthState,nullptr);
    if(depthState){D3D11_DEPTH_STENCIL_DESC depthDesc{};depthState->GetDesc(&depthDesc);if(!depthDesc.DepthEnable)return;}
    ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);D3D11_BUFFER_DESC desc{};
    if(!native)return;
    native->GetDesc(&desc);
    if(desc.ByteWidth<NativeSunBufferBytes||desc.ByteWidth>65536||desc.ByteWidth%16)return;
    D3D11_BUFFER_DESC prior{};if(state.sourceSunClone)state.sourceSunClone->GetDesc(&prior);
    if(!state.sourceSunRaw||!state.sourceSunClone||prior.ByteWidth!=desc.ByteWidth){
        state.sourceSunRaw.Reset();state.sourceSunClone.Reset();
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        D3D11_BUFFER_DESC copy{};copy.ByteWidth=desc.ByteWidth;
        if(FAILED(device->CreateBuffer(&copy,nullptr,&state.sourceSunRaw)))return;
        copy.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&copy,nullptr,&state.sourceSunClone)))return;
    }
    ctx->CopyResource(state.sourceSunRaw.Get(),native.Get());
    const float zero[3]{};const D3D11_BOX range{NativeSunRadianceOffset,0,0,NativeSunRadianceOffset+12,1,1};
    ctx->UpdateSubresource(state.sourceSunRaw.Get(),0,&range,zero,0,0);
    ctx->CopyResource(state.sourceSunClone.Get(),state.sourceSunRaw.Get());
    auto replacement=state.sourceSunClone.Get();ctx->PSSetConstantBuffers(3,1,&replacement);
    context=ctx;original=native.Detach();
}
NativeMaterialSunScope::~NativeMaterialSunScope() {
    if(context){context->PSSetConstantBuffers(3,1,&original);original->Release();}
}
NativeMaterialUnbakeScope::NativeMaterialUnbakeScope(ID3D11DeviceContext* ctx,bool enabled) {
    // One bool decides the common case: with baked lighting on, this costs a single test.
    if(!enabled||!ctx||!state.unbake||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!ps)return;
    // Not the sun ABI: only the declared array slots, which every program publishes. A material
    // that declares no baked array has no lightmap to take away and is left untouched.
    NativeSunAbiInfo info{};UINT size=sizeof(info);
    if(FAILED(ps->GetPrivateData(SunAbiInfoTag,&size,&info))||size!=sizeof(info))return;
    if(!info.lightmapArrayMask&&!info.probeSlot){
        // A program with no baked array is not necessarily unbaked: props carry theirs through
        // the light-probe path instead, so the world unbakes while they keep their baked look.
        // Record what such a program DOES declare, so the second carrier can be addressed.
        if(MvmDebugLog_Active()){
            static std::set<uint64_t> missed;
            if(missed.size()<128&&missed.insert(info.programHash).second){
                char name[128]{};UINT length=sizeof(name)-1;
                ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
                if(vs)vs->GetPrivateData(WKPDID_D3DDebugObjectName,&length,name);
                MvmDebugLog_LinefAlways("environment.unbake.miss","hash=%016llx vs=%s probeSlot=%u probeOcclusionSlot=%u lightSlot=%u flags=%u",
                    info.programHash,name,info.probeSlot,info.probeOcclusionSlot,info.lightSlot,info.flags);
            }
        }
        return;
    }
    const auto neutral=NeutralizeLightmapArrays(info.lightmapArrayMask);
    if(!neutral.Any()&&!info.probeSlot)return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!device||!EnsureUnbakeResources(device.Get(),ctx))return;
    for(UINT slot=1;slot<=3;++slot){
        const bool black=(neutral.blackSlots&(1u<<slot))!=0,fill=(neutral.fillSlots&(1u<<slot))!=0;
        if(!black&&!fill)continue;
        auto view=fill?state.unbakeFillSrv.Get():state.proofBlackArray.Get();
        slots[count]=slot;
        ctx->PSGetShaderResources(slot,1,&previous[count]);
        ctx->PSSetShaderResources(slot,1,&view);
        ++count;
    }
    if(info.probeSlot){
        // Only the irradiance volume. The occlusion volume beside it describes geometry rather
        // than the light we are removing, and the screen-space AO lesson applies: forcing it
        // unoccluded deletes every contact shadow and reads as fullbright.
        auto view=state.unbakeProbeSrv.Get();
        slots[count]=info.probeSlot;
        ctx->PSGetShaderResources(info.probeSlot,1,&previous[count]);
        ctx->PSSetShaderResources(info.probeSlot,1,&view);
        ++count;
    }
    // Reflections, but only on a material that was already carrying baked lighting. Restricting
    // it that way keeps the substitution on world and prop surfaces instead of every shader that
    // happens to declare a cube for some unrelated purpose.
    for(UINT slot=0;slot<32&&count<kMaxSlots;++slot){
        const bool cube=(info.cubeSlotMask&(1u<<slot))!=0,array=(info.cubeArraySlotMask&(1u<<slot))!=0;
        if(!cube&&!array)continue;
        auto view=array?state.unbakeCubeArraySrv.Get():state.unbakeCubeSrv.Get();
        slots[count]=slot;
        ctx->PSGetShaderResources(slot,1,&previous[count]);
        ctx->PSSetShaderResources(slot,1,&view);
        ++count;
    }
    if(!count)return;
    context=ctx;
    if(MvmDebugLog_Active()){
        // Once per program: what it declared and which slots it handed over. A family that never
        // appears here is one the unbake does not reach, which is the first thing to check when
        // a surface keeps its baked light.
        static std::set<uint64_t> seen;
        if(seen.size()<128&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("environment.unbake","hash=%016llx lightmapMask=%u blackSlots=0x%x fillSlots=0x%x probeSlot=%u cubeMask=0x%x cubeArrayMask=0x%x bound=%u fill=%g sunAbi=%u",
                info.programHash,info.lightmapArrayMask,neutral.blackSlots,neutral.fillSlots,info.probeSlot,
                info.cubeSlotMask,info.cubeArraySlotMask,count,state.unbakeFillValue,info.lightSlot);
    }
}
NativeMaterialUnbakeScope::~NativeMaterialUnbakeScope() {
    if(!context)return;
    for(unsigned i=0;i<count;++i){
        context->PSSetShaderResources(slots[i],1,&previous[i]);
        if(previous[i])previous[i]->Release();
    }
}
namespace {
// Temporary census: why each draw was or was not taken, by vertex shader and viewport depth.
void Census(ID3D11DeviceContext* ctx,const char* reason,const D3D11_VIEWPORT& vp){
    if(!MvmDebugLog_Active())return;
    static std::map<std::string,unsigned> counts;static ULONGLONG last=0;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);char name[128]{};UINT size=127;
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name)))strcpy_s(name,"<unnamed>");
    char abi[96]="ps=none";
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(ps){NativeSunAbiInfo info{};UINT infoSize=sizeof(info);
        if(SUCCEEDED(ps->GetPrivateData(SunAbiInfoTag,&infoSize,&info)))
            sprintf_s(abi,"ps[flags=%u cb3=%u light=%u lm=%u probe=%u cube=%x/%x]",info.flags,info.cb3Count,info.lightSlot,info.lightmapArrayMask,info.probeSlot,info.cubeSlotMask,info.cubeArraySlotMask);
        else strcpy_s(abi,"ps=untagged");}
    // Constant-buffer window: the engine may bind a sub-range of a larger buffer, and a patched copy
    // that ignores the window would be read from the wrong place.
    char window[64]="";
    ComPtr<ID3D11DeviceContext1> ctx1;
    if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))){
        ID3D11Buffer* cbs[1]{};UINT first=0,count=0;ctx1->PSGetConstantBuffers1(3,1,cbs,&first,&count);
        if(cbs[0]){D3D11_BUFFER_DESC bd{};cbs[0]->GetDesc(&bd);sprintf_s(window," cb3[bytes=%u first=%u count=%u]",bd.ByteWidth,first,count);cbs[0]->Release();}
    }
    char key[520];sprintf_s(key,"%s | %s | %s%s | vp=%gx%g depth=%g..%g",reason,name,abi,window,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth);
    ++counts[key];
    const auto now=GetTickCount64();
    if(now-last>4000){last=now;for(auto& kv:counts)MvmDebugLog_LinefAlways("relight.census","%u x %s",kv.second,kv.first.c_str());counts.clear();}
}
// The engine's lighting constant buffer is read-only to us, so a patched copy is bound in its
// place for the length of one draw. Two GPU-side buffers: a plain one that takes byte-range
// patches, and a constant-buffer-bindable one that receives the result.
bool EnsureConstantClones(ID3D11Device* device,UINT bytes){
    RenderPrivateTextureScope privateTextures;
    D3D11_BUFFER_DESC prior{};if(state.clone)state.clone->GetDesc(&prior);
    if(state.clone&&state.raw&&prior.ByteWidth==bytes)return true;
    const auto saved=state.constantClonePool.find(bytes);
    if(saved!=state.constantClonePool.end()) {
        state.clone=saved->second[0];state.raw=saved->second[1];return true;
    }
    state.clone.Reset();state.raw.Reset();
    D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.clone)))return false;
    b.BindFlags=0;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.raw)))return false;
    state.constantClonePool.emplace(bytes,std::array<ComPtr<ID3D11Buffer>,2>{state.clone,state.raw});
    return true;
}
void RestoreOwnedInputs(ID3D11DeviceContext* ctx){
    if(state.sharedLightingActive){auto buffer=state.previousSharedLighting.Get();ctx->PSSetConstantBuffers(NativeLightingConstantSlot,1,&buffer);state.previousSharedLighting.Reset();state.sharedLightingActive=false;}
    if(state.sharedAoActive){auto view=state.previousSharedAo.Get();ctx->PSSetConstantBuffers(NativeAoConstantSlot,1,&view);state.previousSharedAo.Reset();state.sharedAoActive=false;}
    if(state.viewActive){auto view=state.previousView.Get();ctx->PSSetConstantBuffers(4,1,&view);state.previousView.Reset();state.viewActive=false;}
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){
        state.proofResources.Restore(ctx);
        auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;
    }
}
void InspectLiveInputs(ID3D11DeviceContext* ctx,ID3D11Device* device,const NativeSunAbiInfo& info,bool replaced){
    if(!MvmDebugLog_Active())return;
    static uint64_t generation=~0ull;
    static unsigned seen=0;
    if(generation!=state.frame.relight.generation){generation=state.frame.relight.generation;seen=0;}
    const unsigned kind=info.lightmapArrayMask?1:2;
    if(seen&kind)return;seen|=kind;
    // Debug logging can remain enabled while time-of-day animates every frame.
    // Bound synchronous GPU readbacks independently of settings generations.
    static ULONGLONG previous[2]{};
    const unsigned index=kind==1?0:1;
    const ULONGLONG now=GetTickCount64();
    if(previous[index]&&now-previous[index]<1000)return;
    previous[index]=now;
    ComPtr<ID3D11Buffer> bound;ctx->PSGetConstantBuffers(3,1,&bound);
    if(!bound)return;
    D3D11_BUFFER_DESC desc{};bound->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> read;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&read)))return;
    ctx->CopyResource(read.Get(),bound.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
    const auto values=static_cast<const float*>(mapped.pData);
    const auto integers=static_cast<const UINT*>(mapped.pData);
    MvmDebugLog_LinefAlways("relight.live.inputs",
        "generation=%llu hash=%016llx deviceFlags=%08x replaced=%u clone=%u sun=(%g %g %g) ambient=(%g %g %g) floor=(%g %g %g) words=%u probes=%u intensity=%g indirect=%g",
        generation,info.programHash,device->GetCreationFlags(),unsigned(replaced),unsigned(bound.Get()==state.clone.Get()),
        values[1946*4],values[1946*4+1],values[1946*4+2],values[7*4+3],values[8*4+3],values[9*4+3],
        values[18*4],values[18*4+1],values[18*4+2],integers[19*4+2],integers[21*4+2],state.frame.relight.intensity,state.frame.relight.indirect);
    ctx->Unmap(read.Get(),0);
    ComPtr<ID3D11ShaderResourceView> volume;ctx->PSGetShaderResources(replaced?Relight::IrradianceTextureSlot:info.probeSlot,1,&volume);
    ComPtr<ID3D11Resource> resource;if(volume)volume->GetResource(&resource);
    ComPtr<ID3D11Texture3D> texture;
    if(!resource||FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE3D_DESC td{};texture->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return;
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture3D> copy;if(FAILED(device->CreateTexture3D(&td,nullptr,&copy)))return;
    ctx->CopyResource(copy.Get(),texture.Get());
    if(FAILED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
    const auto radiance=static_cast<const float*>(mapped.pData);
    MvmDebugLog_LinefAlways("relight.live.volume","hash=%016llx size=%ux%ux%u rgb=(%g %g %g) owned=%u",
        info.programHash,td.Width,td.Height,td.Depth,radiance[0],radiance[1],radiance[2],unsigned(volume.Get()==state.probeBinding.irradiance));
    ctx->Unmap(copy.Get(),0);
}
// Replaces the map's own lighting inputs for one engine draw with ours: sun and lights as
// clustered records with their shadow atlas, the cluster masks, the ambient rows, and every
// baked input (lightmap, probe list, reflection cubes) neutralised. The engine's shader does the
// shading; this only changes what it reads. Leaves the patched buffer in state.clone.
// Player bodies: CS2's skinned character vertex shader. Cached per shader object.
bool CharacterDraw(ID3D11DeviceContext* ctx){
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
    if(!vs)return false;
    static ID3D11VertexShader* lastShader=nullptr;static bool lastResult=false;
    if(vs.Get()==lastShader)return lastResult;
    char name[128]{};UINT size=sizeof(name)-1;
    lastShader=vs.Get();
    lastResult=SUCCEEDED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name))&&strstr(name,"csgo_character")!=nullptr;
    return lastResult;
}
// Repair only the AO addressing triple. Verified shaders read it from a shared
// buffer uploaded when resolution/viewport changes; unfamiliar programs retain
// the full native-view clone so mutable engine constants are never cached by pointer.
void BindAoView(ID3D11DeviceContext* ctx,ID3D11Device* device,const D3D11_VIEWPORT& vp){
    if(vp.MinDepth>0)return;
    UINT aoWidth=0,aoHeight=0;AmbientOcclusionSize(aoWidth,aoHeight);
    if(!aoWidth||!aoHeight)return;
    ComPtr<ID3D11PixelShader> shader,shared;ctx->PSGetShader(&shader,nullptr,nullptr);
    UINT bytes=sizeof(ID3D11PixelShader*);
    static unsigned sharedDraws=0,uploads=0,fallbackDraws=0;
    if(shader&&SUCCEEDED(::Filmmaker::GetOwnedInterface(shader.Get(),SharedAoShaderTag,&bytes,(void**)shared.GetAddressOf()))&&shared){
        const std::array<float,4> row{1.f/float(aoWidth),1.f/float(aoHeight),float(aoWidth)/vp.Width,0};
        if(!state.sharedAo){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=row.data();
            if(SUCCEEDED(device->CreateBuffer(&d,&initial,&state.sharedAo))){state.sharedAoRow=row;++uploads;}}
        if(state.sharedAo){
            if(row!=state.sharedAoRow){ctx->UpdateSubresource(state.sharedAo.Get(),0,nullptr,row.data(),0,0);state.sharedAoRow=row;++uploads;}
            ctx->PSGetConstantBuffers(NativeAoConstantSlot,1,state.previousSharedAo.ReleaseAndGetAddressOf());
            auto constants=state.sharedAo.Get();ctx->PSSetConstantBuffers(NativeAoConstantSlot,1,&constants);
            ctx->PSSetShader(shared.Get(),nullptr,0);state.sharedAoActive=true;++sharedDraws;
        }
    }
    if(!state.sharedAoActive){
        ++fallbackDraws;
        ComPtr<ID3D11Buffer> nativeView;ctx->PSGetConstantBuffers(4,1,&nativeView);
        if(aoWidth&&aoHeight&&nativeView){
            D3D11_BUFFER_DESC vd{};nativeView->GetDesc(&vd);
            if(vd.ByteWidth>=17*16&&!(vd.ByteWidth%16)){
                D3D11_BUFFER_DESC have{};if(state.viewClone)state.viewClone->GetDesc(&have);
                if(!state.viewClone||!state.viewCloneRaw||have.ByteWidth!=vd.ByteWidth){state.viewClone.Reset();state.viewCloneRaw.Reset();
                    D3D11_BUFFER_DESC d{};d.ByteWidth=vd.ByteWidth;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;device->CreateBuffer(&d,nullptr,&state.viewClone);
                    d.BindFlags=0;device->CreateBuffer(&d,nullptr,&state.viewCloneRaw);}
                if(state.viewClone&&state.viewCloneRaw){
                    // A constant buffer takes only whole updates: patch row 16 in a plain buffer and copy
                    // it across (a partial UpdateSubresource on the constant buffer itself is dropped).
                    // Re-copied per draw: the engine rewrites its per-view buffer between passes.
                    ctx->CopyResource(state.viewCloneRaw.Get(),nativeView.Get());
                    const float row[3]{1.f/float(aoWidth),1.f/float(aoHeight),float(aoWidth)/vp.Width};
                    const D3D11_BOX box{16*16,0,0,16*16+12,1,1};ctx->UpdateSubresource(state.viewCloneRaw.Get(),0,&box,row,0,0);
                    ctx->CopyResource(state.viewClone.Get(),state.viewCloneRaw.Get());
                    state.previousView=nativeView;auto repaired=state.viewClone.Get();ctx->PSSetConstantBuffers(4,1,&repaired);state.viewActive=true;
                }
            }
        }
    }
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.ao.shared","draws=%u uploads=%u fallback=%u",sharedDraws,uploads,fallbackDraws);
        sharedDraws=uploads=fallbackDraws=0;}}
}
bool BindOwnedInputs(ID3D11DeviceContext* ctx,ID3D11Device* device,const D3D11_VIEWPORT& vp,ID3D11Buffer* native,const NativeSunAbiInfo& info){
    LiveLap(0);
    if(!ProofResources(device,ctx,vp)){LiveLap(1);return false;}
    LiveLap(1);
    const auto& set=state.frame.relight;
    auto& patches=state.constantPatches;patches.Reset();state.lightingCopies.clear();
    auto patchRange=[&](UINT offset,UINT bytes,const void* value){patches.Write(offset,bytes,value);};
    auto copyRange=[&](UINT offset,ID3D11Buffer* source,UINT bytes){
        patches.Overwrite(offset,bytes);
        state.lightingCopies.push_back({offset,bytes,source});
    };
    const UINT zeros[4]{};
    // The native sun goes dark; the sun is one of our clustered records instead, with its own
    // shadow tile, so the engine's cascades must not shadow it a second time.
    patchRange(NativeSunRadianceOffset,12,zeros);
    patchRange(NativeSunCascadesOffset,4,zeros);
    // The 3D skybox (depth .95..1) is a scaled miniature in its own coordinate space. Map
    // shadow maps and map light positions do not describe it. Its exported world node has
    // a separate sun depth map in those miniature coordinates, and no foreground local records.
    const bool skybox=vp.MinDepth>0;
    auto skyboxShadow=skybox?SkyboxSunShadow(nullptr):nullptr;
    const bool ownedSun=info.ownedCascades;
    ID3D11ShaderResourceView* sunVisibility=nullptr;
    if(ownedSun){
        const auto direction=Relight::SunDirection(set.sunAngles),radiance=Relight::SunRadiance(set);
        const float sunDirection[4]{direction.x,direction.y,direction.z,0};
        patchRange(NativeSunDirectionOffset,sizeof(sunDirection),sunDirection);
        patchRange(NativeSunRadianceOffset,sizeof(radiance),&radiance);
        const UINT count=set.shadows&&set.sunShadows?(skybox?(skyboxShadow?1u:0u):(state.proofSunShadowed?4u:0u)):0u;
        patchRange(NativeSunCascadesOffset,4,&count);
        // Physically sized sun penumbra: a per-pixel visibility pass takes over the cascade filter for
        // programs that expose the engine's screen-space shadow hook (info.screenShadowSlot). The flag
        // row says whether this draw supplies the texture; without it the native filter runs.
        // A forced cascade (mirv_relight suncascade, diagnostic) runs the native loop unchanged.
        // Players keep the native filter: the visibility pass rebuilds normals from the depth buffer,
        // which on a moving body turned every self-shadow terminator into stair-stepped teeth that
        // crawled frame to frame, and its receiver tolerance cannot carry the body's pose-lag bias.
        // The native loop uses the vertex normal and the character bias (cb3[1953].z) instead.
        if(count&&!skybox&&info.screenShadowSlot&&set.sunCascade<0&&!CharacterDraw(ctx)){
            Matrix cascades[4];for(unsigned i=0;i<4;++i)cascades[i]=state.proofMatrices[OwnedSunTiles[i]];
            const auto rects=NativeSunRects();Matrix toWorld{};
            if(state.frame.hasWorldToClip&&Inverse(state.frame.worldToClip,toWorld)){
                SunVisibilityInput input;input.cascades=cascades;input.rects=rects.data();input.atlas=state.proofAtlas.Get();
                input.atlasWidth=float(Relight::AtlasWidth);input.atlasHeight=float(Relight::AtlasHeight);
                input.tileSize=float(Relight::SunCascadeSize);input.camera=state.frame.camera;input.clipToWorld=&toWorld;
                input.tanHalfAngle=SunTanHalfAngle(set.sunAngle)*set.shadowSoftness;input.bias=set.shadowBias;
                if(input.atlas)sunVisibility=SunVisibility(ctx,vp,input);
            }
        }
        const UINT screenShadowFlag=sunVisibility?1u:0u;
        patchRange(ScreenShadowFlagRow*16+4,4,&screenShadowFlag);
        // Begin blending in the outer 12% of each light-space square. The
        // shader already blends the next tier; the final tier covers the map.
        float thresholds[4]{.985f,.985f,.985f,1.f};
        if(set.sunCascade>=0)for(int i=0;i<4;++i)if(i!=set.sunCascade)thresholds[i]=0;
        const float blend[4]{-7.333333f,8.333333f,0,0};
        patchRange(1954*16,sizeof(thresholds),thresholds);patchRange(1955*16,sizeof(blend),blend);
        auto columnSource=CharacterDraw(ctx)&&state.characterSunCascadeColumns?state.characterSunCascadeColumns.Get():state.sunCascadeColumns.Get();
        copyRange(1956*16,columnSource,4*UINT(sizeof(Matrix)));
        auto rects=NativeSunRects();
        if(skyboxShadow)for(unsigned i=0;i<4;++i)std::copy(NativeSkyboxSunRect.begin(),NativeSkyboxSunRect.end(),rects.begin()+i*4);
        patchRange(1988*16,sizeof(rects),rects.data());
        // Native probe contact modulation belongs to baked lighting; the owned
        // sun's geometry visibility is entirely in the current shadow atlas.
        patchRange(1947*16,16,zeros);
        if(MvmDebugLog_Active()){static ULONGLONG last[2]{};const auto now=GetTickCount64();auto& reported=last[skybox?1:0];if(now-reported>=3000){reported=now;
            MvmDebugLog_LinefAlways(skybox?"environment.skybox.sun.cascades":"environment.relight.sun.cascades","count=%u resolution=%u hash=%016llx camera=(%.2f %.2f %.2f) nativeViewRebase=1 localSunSuppressed=1",
                count,Relight::SunCascadeSize,info.programHash,state.frame.camera.x,state.frame.camera.y,state.frame.camera.z);}}
    }
    // cb3[0..3] carry the shadow filter kernel; zeroing them collapses every tap onto one texel.
    float filterRows[16];Relight::ShadowFilterRows(Relight::LampFilterTexels(set.lampAngle,set.shadowSoftness),filterRows);
    filterRows[0]=2*std::tan(std::clamp(set.lampAngle,0.f,5.f)*.00872664626f)*std::clamp(set.shadowSoftness,0.f,4.f);
    filterRows[1]=Relight::FaceGuardTexels/Relight::AtlasWidth;
    filterRows[2]=Relight::FaceGuardTexels/Relight::AtlasHeight;
    if(skyboxShadow){
        Relight::ShadowFilterRows(Relight::NativeFilterTexels*set.sunAngle/.53f*set.shadowSoftness,filterRows);
        filterRows[9]*=float(Relight::AtlasWidth)/Relight::SunCascadeSize;filterRows[10]=-filterRows[9];
        filterRows[13]*=float(Relight::AtlasHeight)/Relight::SunCascadeSize;filterRows[14]=-filterRows[13];
    }
    patchRange(0,sizeof(filterRows),filterRows);
    const float bias=-(CharacterDraw(ctx)?CharacterLocalBias(set):set.shadowBias);patchRange(1953*16+8,4,&bias);
    // cb3[19].z is how many 32-bit mask words the clustered loop reads: capacity is 32 * words.
    const UINT words=skybox&&ownedSun?0u:set.cost==1?1:state.proofMaskWords;
    auto cull=state.clusterGrid.ListRow(state.clusterGrid.lights,state.clusterMode);cull[2]=words;
    const auto tiles=state.clusterGrid.TileConstants(state.clusterMode);
    const auto slices=state.clusterGrid.SliceConstants(state.clusterMode);
    // cb3[20] and cb3[21] are the reflection-cube and irradiance-probe lists. Emptying them takes
    // the baked cubemaps and the baked probe volume out of the shader's reach entirely.
    patchRange(19*16,16,cull.data());patchRange(20*16,16,zeros);patchRange(21*16,16,zeros);
    patchRange(22*16,16,tiles.data());patchRange(23*16,16,slices.data());
    if(state.clusterMode&&!(skybox&&ownedSun))copyRange(19*16,state.clusterRows.Get(),80);
    // Sky-over-ground hemisphere rows; the shader multiplies them by albedo itself.
    float ambient[12]{};
    Relight::AmbientRows(Relight::SkyAmbient(set,set.indirect),{},ambient);
    if(set.probeCheck==1||set.probeCheck==2)Relight::AmbientRows(Relight::ProbeNeutral,Relight::ProbeNeutral,ambient);
    patchRange(7*16,sizeof(ambient),ambient);
    // cb3[18] is a flat floor added regardless of orientation or occlusion; only the ground term
    // may go there, and never where the lightmap already carries the fill.
    const float floorTerm[4]{ambient[3]-ambient[2],ambient[7]-ambient[6],ambient[11]-ambient[10],0};
    if(!(info.lightmapArrayMask&2))patchRange(18*16,16,floorTerm);else patchRange(18*16,16,zeros);
    // Live reflections: one map-wide record for the capture marched from the camera through the
    // lit bounce volume, in the clustered reflection list the baked cubes used to occupy.
    const auto& live=state.probeBinding;
    const bool reflections=!skybox&&state.probeTraced&&set.reflections>0&&live.reflection&&live.reflectionNorm&&info.cubeArraySlotMask;
    if(reflections){
        const auto& grid=state.clusterGrid;
        const UINT list[4]{grid.reflections.slices,grid.reflections.tiles,1,0};patchRange(20*16,16,list);
        const auto rows=Relight::ReflectionRecordRows(live.reflectionOrigin,set.reflections);
        patchRange(25*16,UINT(sizeof(rows)),rows.data());
        copyRange(33*16,live.reflectionNorm,16);
        const float mips=float(live.reflectionMips?live.reflectionMips-1:0);patchRange(24*16+4,4,&mips);
        patchRange(6*16,8,Relight::ReflectionNormalizationCap.data());
        if(MvmDebugLog_Active()){static std::set<uint64_t> seen;
            if(seen.size()<256&&seen.insert(info.programHash).second)
                MvmDebugLog_LinefAlways("relight.reflection.program","hash=%016llx cubeArrayMask=0x%x cubeMask=0x%x mips=%u origin=(%.0f %.0f %.0f) strength=%.2f viewportDepth=%g..%g",
                    info.programHash,info.cubeArraySlotMask,info.cubeSlotMask,live.reflectionMips,live.reflectionOrigin.x,live.reflectionOrigin.y,live.reflectionOrigin.z,set.reflections,vp.MinDepth,vp.MaxDepth);}
    }else if(MvmDebugLog_Active()&&info.cubeArraySlotMask){static std::set<uint64_t> seen;
        if(seen.size()<256&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("relight.reflection.skip","hash=%016llx skybox=%u traced=%u strength=%.2f capture=%u",
                info.programHash,unsigned(skybox),unsigned(state.probeTraced),set.reflections,unsigned(live.reflection!=nullptr));}
    state.proofLightSlot=info.lightSlot;
    ComPtr<ID3D11Buffer> nativeView;ctx->PSGetConstantBuffers(4,1,&nativeView);
    // The material adds cb4[35].xyz to its position varying for world-space lookups (probe
    // volume, reflections). Compare it between the world pass and the first-person pass.
    if(MvmDebugLog_Active()&&nativeView){
        static ULONGLONG last[3]{};const unsigned pass=skybox?2:vp.MaxDepth<=.1f+1e-6f?1:0;const auto now=GetTickCount64();
        if(now-last[pass]>=3000){last[pass]=now;
            D3D11_BUFFER_DESC d{};nativeView->GetDesc(&d);
            if(d.ByteWidth>=36*16){d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Buffer> read;D3D11_MAPPED_SUBRESOURCE m{};
                if(SUCCEEDED(device->CreateBuffer(&d,nullptr,&read))){ctx->CopyResource(read.Get(),nativeView.Get());
                    if(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m))){const auto f=static_cast<const float*>(m.pData);
                        MvmDebugLog_LinefAlways("relight.view.offset","pass=%s hash=%016llx cb4[35]=(%.2f %.2f %.2f) camera=(%.2f %.2f %.2f) probe=%u traced=%u vp=%g..%g",
                            pass==2?"skybox":pass==1?"viewmodel":"world",info.programHash,f[140],f[141],f[142],state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,
                            info.probeSlot,unsigned(state.probeTraced),vp.MinDepth,vp.MaxDepth);
                        if(pass==1&&d.ByteWidth>=41*16)MvmDebugLog_LinefAlways("relight.view.offset","viewmodel cb4[36]=(%.3f %.3f %.3f %.3f) shadowBypass=0x%08x",f[144],f[145],f[146],f[147],static_cast<const unsigned*>(m.pData)[8]);
                        ctx->Unmap(read.Get(),0);}}}
            // The first-person path moves its lighting position with per-view cb1 rows 23-28.
            ComPtr<ID3D11Buffer> perView;ctx->PSGetConstantBuffers(1,1,&perView);
            if(pass==1&&perView){D3D11_BUFFER_DESC v{};perView->GetDesc(&v);
                if(v.ByteWidth>=29*16){v.Usage=D3D11_USAGE_STAGING;v.BindFlags=v.MiscFlags=0;v.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Buffer> read;D3D11_MAPPED_SUBRESOURCE m{};
                    if(SUCCEEDED(device->CreateBuffer(&v,nullptr,&read))){ctx->CopyResource(read.Get(),perView.Get());
                        if(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m))){const auto g=static_cast<const float*>(m.pData);
                            MvmDebugLog_LinefAlways("relight.view.offset","viewmodel cb1[20]=(%g %g %g %g) [23]=(%g %g %g %g) [24]=(%g %g %g %g) [26]=(%g %g %g %g) [28]=(%g %g %g %g)",
                                g[80],g[81],g[82],g[83],g[92],g[93],g[94],g[95],g[96],g[97],g[98],g[99],g[104],g[105],g[106],g[107],g[112],g[113],g[114],g[115]);
                            ctx->Unmap(read.Get(),0);}}}}}
    }
    Matrix skyMatrices[SkyVisibilityDirections]{};SkyVisibilityInput sky{};
    if(set.skyOcclusion>0&&!state.probeTraced&&skyVisibilitySource&&skyVisibilitySource(&sky.maps,skyMatrices)){sky.matrices=skyMatrices;sky.strength=set.skyOcclusion;}
    Matrix clipToWorld{};const bool unprojects=state.frame.hasWorldToClip&&Inverse(state.frame.worldToClip,clipToWorld);
    LiveLap(0);
    auto ao=AmbientOcclusion(ctx,state.frame.camera,vp,nativeView.Get(),clipToWorld,unprojects?set.ao:0.f,unprojects?&sky:nullptr,unprojects&&state.probeTraced&&!skybox);
    LiveLap(2);
    if(state.probeTraced&&!skybox&&!set.probeCheck){
        UINT width=0,height=0;auto positions=AmbientOcclusionPositions(ctx,width,height);
        Relight::MarkVisibleProbeGi(ctx,positions,width,height,state.probeFrame);
    }
    UINT slots[48]{13,state.proofLightSlot,6,7};
    ID3D11ShaderResourceView* views[48]{state.proofMaskSrv.Get(),ownedSun?state.proofLocalLightSrv.Get():state.proofLightSrv.Get(),skyboxShadow?skyboxShadow:state.proofAtlas.Get(),ao};
    unsigned bindings=4;
    if(sunVisibility){slots[bindings]=info.screenShadowSlot;views[bindings++]=sunVisibility;}
    for(unsigned slot=0;slot<32;++slot){
        if(info.cubeSlotMask&(1u<<slot)){slots[bindings]=slot;views[bindings++]=state.proofBlackCube.Get();}
        if(info.cubeArraySlotMask&(1u<<slot)){slots[bindings]=slot;views[bindings++]=reflections?live.reflection:state.proofBlackCubeArray.Get();}
    }
    // Stained glass beams select their window from this cookie volume by radiance.w.
    if(state.glassCookies&&info.hasCookie&&!skybox){slots[bindings]=info.cookieSlot;views[bindings++]=state.glassCookies;}
    const auto& probe=skybox?state.skyboxBinding:state.probeBinding;
    if(probe.constants&&info.probeSlot){
        const UINT table[4]{state.clusterGrid.probes.slices,state.clusterGrid.probes.tiles,1,0};
        patchRange(21*16,16,table);patchRange(1177*16,6*16,probe.packing.probe.data());
        patchRange(1947*16,16,zeros);
        if(set.probeCheck!=1&&set.probeCheck!=2)patchRange(18*16,16,zeros);
        ctx->PSGetConstantBuffers(2,1,state.previousProbeConstants.ReleaseAndGetAddressOf());
        auto cb2=probe.constants;ctx->PSSetConstantBuffers(2,1,&cb2);state.probeActive=true;
        slots[bindings]=info.probeSlot;views[bindings++]=probe.irradiance;
        if(info.probeOcclusionSlot){slots[bindings]=info.probeOcclusionSlot;views[bindings++]=probe.sunOcclusion;}
    }
    for(unsigned array=0;array<3;++array)if(info.lightmapArrayMask&(1u<<array)){
        slots[bindings]=array+1;
        if(array==1&&!state.engineLightmapWidth){
            ComPtr<ID3D11ShaderResourceView> engine;ctx->PSGetShaderResources(2,1,&engine);
            if(engine){
                ComPtr<ID3D11Resource> resource;engine->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
                if(resource&&SUCCEEDED(resource.As(&texture))){
                    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                    state.engineLightmapWidth=desc.Width;state.engineLightmapHeight=desc.Height;state.engineLightmapArray=desc.ArraySize;
                    if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.lightmap.engine","width=%u height=%u array=%u format=%u",desc.Width,desc.Height,desc.ArraySize,unsigned(desc.Format));
                }
            }
        }
        // The replacement shader queries world-space irradiance. All three baked
        // arrays are unreachable; black is also the safe value for an unknown query.
        views[bindings]=state.proofBlackArray.Get();
        ++bindings;
    }
    state.proofResources.Bind(ctx,bindings,slots,views);
    state.proofSamplerSlot=info.comparisonSampler;
    ctx->PSGetSamplers(state.proofSamplerSlot,1,state.proofPreviousSampler.ReleaseAndGetAddressOf());
    auto sampler=state.proofComparison.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);
    state.proofActive=true;
    return true;
}
bool FinalizeOwnedConstants(ID3D11DeviceContext* ctx,ID3D11Device* device,ID3D11Buffer* native){
    static unsigned sharedDraws=0,sharedUploads=0,fallbackDraws=0;
    ComPtr<ID3D11PixelShader> current,shared;ctx->PSGetShader(&current,nullptr,nullptr);
    UINT bytes=sizeof(ID3D11PixelShader*);NativeSharedLightingInfo layout{};UINT layoutBytes=sizeof(layout);
    const bool sparseView=state.clusterSummaries&&state.clusterMode==1&&state.proofViewport.MinDepth==0&&
        std::abs(state.proofViewport.MaxDepth-.95f)<1e-6f;
    if(current&&sparseView){ComPtr<ID3D11PixelShader> sparseShader;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(current.Get(),SparseClusterShaderTag,&bytes,(void**)sparseShader.GetAddressOf()))&&sparseShader)shared=std::move(sparseShader);
    }
    if(!shared&&current){bytes=sizeof(ID3D11PixelShader*);::Filmmaker::GetOwnedInterface(current.Get(),SharedLightingShaderTag,&bytes,(void**)shared.GetAddressOf());}

    auto& covered=state.lightingCovered;covered.clear();
    state.constantPatches.Flush([&](UINT offset,UINT size,const void*){covered.push_back({offset,offset+size});});
    for(const auto& copy:state.lightingCopies)covered.push_back({copy.offset,copy.offset+copy.bytes});
    const bool eligible=current&&shared&&
        SUCCEEDED(shared->GetPrivateData(SharedLightingInfoTag,&layoutBytes,&layout))&&layoutBytes==sizeof(layout)&&
        NativeSharedLightingCovered(layout,covered);
    ID3D11Buffer* selected=nullptr;
    if(eligible){
        UINT sparse=0,sparseBytes=sizeof(sparse);
        const UINT summaryOffset=state.clusterSummaries&&state.clusterMode==1&&state.proofViewport.MinDepth==0&&
            SUCCEEDED(shared->GetPrivateData(SparseClusterInfoTag,&sparseBytes,&sparse))&&sparse?state.clusterGrid.size:0u;
        // Exact plan equality within one frame/view. Engine buffer identities are
        // deliberately absent: native rows stay on b3 and may change every draw.
        auto& key=state.lightingKey;key.clear();
        auto append=[&](const void* data,size_t size){const auto* p=static_cast<const uint8_t*>(data);key.insert(key.end(),p,p+size);};
        append(&state.proofUploads,sizeof(state.proofUploads));
        append(&summaryOffset,sizeof(summaryOffset));
        const UINT cpuRanges=UINT(covered.size()-state.lightingCopies.size()),gpuRanges=UINT(state.lightingCopies.size());
        append(&cpuRanges,sizeof(cpuRanges));append(&gpuRanges,sizeof(gpuRanges));
        state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){append(&offset,sizeof(offset));append(&size,sizeof(size));append(value,size);});
        for(const auto& copy:state.lightingCopies){append(&copy.offset,sizeof(copy.offset));append(&copy.bytes,sizeof(copy.bytes));append(&copy.source,sizeof(copy.source));}
        SharedLightingBuffer* entry=nullptr;
        for(unsigned i=0;i<state.sharedLightingUsed;++i)if(state.sharedLightingPool[i].key==key){entry=&state.sharedLightingPool[i];break;}
        if(!entry&&state.sharedLightingUsed<64){
            if(state.sharedLightingUsed==state.sharedLightingPool.size())state.sharedLightingPool.emplace_back();
            entry=&state.sharedLightingPool[state.sharedLightingUsed];
            if(!entry->raw||!entry->constants){
                RenderPrivateTextureScope privateTextures;D3D11_BUFFER_DESC d{};d.ByteWidth=NativeLightingRows*16;
                if(!entry->raw)device->CreateBuffer(&d,nullptr,&entry->raw);
                d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                if(!entry->constants)device->CreateBuffer(&d,nullptr,&entry->constants);
            }
            if(entry->raw&&entry->constants){
                std::array<uint8_t,NativeLightingRows*16> data{};
                state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){std::memcpy(data.data()+offset,value,size);});
                ctx->UpdateSubresource(entry->raw.Get(),0,nullptr,data.data(),0,0);
                for(const auto& copy:state.lightingCopies)
                    state.constantPatches.ForEachUnwritten(copy.offset,copy.bytes,[&](UINT offset,UINT size){
                        const UINT sourceOffset=offset-copy.offset;
                        const D3D11_BOX range{sourceOffset,0,0,sourceOffset+size,1,1};
                        ctx->CopySubresourceRegion(entry->raw.Get(),0,offset,0,0,copy.source,0,&range);
                    });
                // GPU cluster rows overwrite the CPU row during the copies above.
                // Enable occupancy only for the selected, validated shared variant.
                const D3D11_BOX summaryFlag{19*16+12,0,0,19*16+16,1,1};
                if(summaryOffset)ctx->UpdateSubresource(entry->raw.Get(),0,&summaryFlag,&summaryOffset,0,0);
                ctx->CopyResource(entry->constants.Get(),entry->raw.Get());entry->key=key;++state.sharedLightingUsed;++sharedUploads;
            }else entry=nullptr;
        }
        if(entry){
            selected=entry->constants.Get();ctx->PSGetConstantBuffers(NativeLightingConstantSlot,1,state.previousSharedLighting.ReleaseAndGetAddressOf());
            ctx->PSSetConstantBuffers(NativeLightingConstantSlot,1,&selected);state.sharedLightingActive=true;
            ctx->PSSetShader(shared.Get(),nullptr,0);++sharedDraws;
            if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(summaryOffset&&now-last>=3000){last=now;
                MvmDebugLog_LinefAlways("relight.cluster.sparse","offset=%u words=%u rasterOnly=1",summaryOffset,state.proofMaskWords);}}
        }
    }
    if(!selected){
        D3D11_BUFFER_DESC desc{};native->GetDesc(&desc);const D3D11_BOX nativeRange{0,0,0,desc.ByteWidth,1,1};
        ctx->CopySubresourceRegion(state.raw.Get(),0,0,0,0,native,0,&nativeRange);
        for(const auto& copy:state.lightingCopies){const D3D11_BOX range{0,0,0,copy.bytes,1,1};
            ctx->CopySubresourceRegion(state.raw.Get(),0,copy.offset,0,0,copy.source,0,&range);}
        state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){const D3D11_BOX range{offset,0,0,offset+size,1,1};
            ctx->UpdateSubresource(state.raw.Get(),0,&range,value,0,0);});
        ctx->CopyResource(state.clone.Get(),state.raw.Get());auto clone=state.clone.Get();ctx->PSSetConstantBuffers(3,1,&clone);++fallbackDraws;
    }
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.constants.shared","draws=%u uploads=%u fallback=%u variants=%u",sharedDraws,sharedUploads,fallbackDraws,state.sharedLightingUsed);
        sharedDraws=sharedUploads=fallbackDraws=0;}}
    return selected!=nullptr;
}
}
NativeMaterialSkyScope::NativeMaterialSkyScope(ID3D11DeviceContext* ctx,bool enabled){
    if(!enabled||!ctx||!state.frame.active||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    const auto climate=EffectiveClimate(state.frame.scene.climate);
    if(climate[CL_sky]<=0)return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    char name[128]{};UINT bytes=sizeof(name)-1;
    if(!ps||FAILED(ps->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,name))||bytes>=sizeof(name))return;
    if(std::strcmp(name,"sky.vfx_ps")&&std::strcmp(name,"sky_pc_50_ps"))return;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    if(count!=1||vp.Width<256||vp.Height<256)return;
    Matrix inverse{};if(!Inverse(state.frame.worldToClip,inverse))return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!state.skyShader){
        ComPtr<ID3DBlob> code,error;
        if(FAILED(CompileEnvironmentShader(NativeSkyShader,sizeof(NativeSkyShader)-1,"EnvironmentNativeSky",nullptr,
            "NativeSkyPS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||
           FAILED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&state.skyShader))){
            Evidence("native-sky:compile-failed");return;
        }
    }
    struct Row {float x,y,z,w;};
    struct Constants {Matrix inverse;Row camera,viewport,sun,radiance,sky;};
    if(!state.skyConstants){
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(Constants);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.skyConstants)))return;
    }
    const auto direction=ClimateSunDirection(climate),radiance=ClimateSunRadiance(climate);
    Constants constants{inverse,{state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,0},
        {vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height},
        {direction.x,direction.y,direction.z,climate[CL_sunElevation]},
        {radiance.x,radiance.y,radiance.z,climate[CL_haze]},
        {climate[CL_stars],nativePostCompressedHdr.load(std::memory_order_relaxed)?1.f:0.f,0,0}};
    ctx->UpdateSubresource(state.skyConstants.Get(),0,nullptr,&constants,0,0);
    ctx->PSGetConstantBuffers(13,1,&previousConstants);
    auto cb=state.skyConstants.Get();ctx->PSSetConstantBuffers(13,1,&cb);
    original=ps.Detach();ctx->PSSetShader(state.skyShader.Get(),nullptr,0);context=ctx;
    state.skyApplied=true;state.skyGeneration=state.frame.generation;
    Evidence("native-sky:geometry-coverage");
}
NativeMaterialSkyScope::~NativeMaterialSkyScope(){
    if(!context)return;
    context->PSSetShader(original,nullptr,0);original->Release();
    context->PSSetConstantBuffers(13,1,&previousConstants);
    if(previousConstants)previousConstants->Release();
}
bool PrepareSmokeInputs(ID3D11Device* device,ID3D11DeviceContext* ctx){
    if(!state.proofResourcesReady||!state.probeBinding.irradiance||state.frame.smokes.empty())return false;
    if(state.smokeFrame==state.probeFrame&&state.smokeLightSrv)return true;
    const auto& set=state.frame.relight;
    std::vector<std::pair<float,unsigned>> ranked;
    for(unsigned i=0;i<set.lights.size();++i){const auto& light=set.lights[i];
        if(!set.localLights||!light.enabled||Relight::Power(light,set.intensity)<=0)continue;
        float nearest=INFINITY;
        for(const auto& smoke:state.frame.smokes){const auto d=smoke.center-light.position;nearest=(std::min)(nearest,std::sqrt(Dot(d,d))-smoke.radius);}
        if(nearest>light.range)continue;
        ranked.push_back({nearest-(light.transient||light.sceneObject?100000.f:0.f),i});
    }
    std::stable_sort(ranked.begin(),ranked.end());if(ranked.size()>8)ranked.resize(8);
    state.smokeRecords.clear();
    for(const auto& entry:ranked){const unsigned index=entry.second;const auto& light=set.lights[index];
        const unsigned first=index<state.proofFirstTiles.size()?state.proofFirstTiles[index]:~0u;
        if(first==~0u){if(!set.shadows||!light.transient)state.smokeRecords.push_back(Relight::ToRecord(light,set.intensity));continue;}
        for(unsigned face=0;face<Relight::FixtureFaces(light);++face)
            state.smokeRecords.push_back(Relight::ToShadowedRecord(light,set.intensity,state.proofMatrices[first+face].data(),first+face));
    }
    state.smokeRecords.push_back(Relight::SunRecord(Relight::SunDirection(set.sunAngles),Relight::SunRadiance(set),
        state.proofMatrices[Relight::SunTile].data(),state.proofSunShadowed&&set.shadows&&set.sunShadows));
    if(!state.smokeLights||!state.smokeLightSrv||!state.smokeConstants||!state.smokeProbes){
        state.smokeLights.Reset();state.smokeLightSrv.Reset();state.smokeConstants.Reset();state.smokeProbes.Reset();
        D3D11_BUFFER_DESC d{};d.ByteWidth=64*sizeof(Relight::LightRecord);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(Relight::LightRecord);
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeLights)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=64;
        if(FAILED(device->CreateShaderResourceView(state.smokeLights.Get(),&view,&state.smokeLightSrv)))return false;
        d={};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeConstants)))return false;
        d.ByteWidth=8*16;if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeProbes)))return false;
    }
    const D3D11_BOX range{0,0,0,UINT(state.smokeRecords.size()*sizeof(Relight::LightRecord)),1,1};
    ctx->UpdateSubresource(state.smokeLights.Get(),0,&range,state.smokeRecords.data(),0,0);
    struct Constants{UINT count;float bias,padding[2];} constants{UINT(state.smokeRecords.size()),set.shadowBias,{}};
    ctx->UpdateSubresource(state.smokeConstants.Get(),0,nullptr,&constants,0,0);
    const auto& packing=state.probeBinding.packing;std::array<Relight::ProbeRow,8> rows{};
    for(unsigned axis=0;axis<3;++axis){rows[0][axis]=packing.probe[axis][axis];rows[1][axis]=packing.probe[axis][3];
        rows[2][axis]=packing.volume[3][axis]*(axis==2?6.f:1.f);rows[3][axis]=packing.volume[4][axis]*(axis==2?6.f:1.f);}
    rows[4]=packing.volume[5];rows[6]=packing.volume[6];rows[7]=packing.volume[7];
    ctx->UpdateSubresource(state.smokeProbes.Get(),0,nullptr,rows.data(),0,0);
    state.smokeFrame=state.probeFrame;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=1000){last=now;
        MvmDebugLog_LinefAlways("environment.smoke.lighting","records=%zu lights=%zu clouds=%zu liveGi=1 atlas=%d",state.smokeRecords.size(),ranked.size(),state.frame.smokes.size(),bool(state.proofAtlas));}}
    return true;
}
NativeMaterialLiveScope::NativeMaterialLiveScope(ID3D11DeviceContext* ctx,bool enabled){
    // One bool decides the common case: with live lighting off this costs a single test.
    if(!enabled&&ctx&&state.live&&MvmDebugLog_Active()){
        D3D11_VIEWPORT diagnostic{};UINT n=1;ctx->RSGetViewports(&n,&diagnostic);
        if(n==1)Census(ctx,"live-disabled-hook",diagnostic);
    }
    if(!enabled||!ctx||!state.live||state.proofActive||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    LiveLapStart();
    struct LapTail{~LapTail(){LiveLap(0);}} lapTail;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    // Distant 3D skybox meshes are real surfaces too. Their separate .95..1
    // depth interval must not let baked lightmaps and probes escape replacement.
    if(count!=1||!NativeLightingViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth))return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!ps)return;
    // The clustered-light interface is derived from each program's bytecode, so any material
    // family that carries it is lit, whatever its vertex shader is called.
    NativeSunAbiInfo info{};UINT size=sizeof(info);
    if(FAILED(ps->GetPrivateData(SunAbiInfoTag,&size,&info))||size!=sizeof(info))return;
    // Depth/stencil passes still need their native shader for alpha rejection,
    // but have no colour output to relight. Leave every native binding intact.
    // A bound target whose blend state masks every channel is a depth pass too.
    ID3D11RenderTargetView* colorTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,colorTargets,nullptr);
    ComPtr<ID3D11BlendState> colorBlend;ctx->OMGetBlendState(&colorBlend,nullptr,nullptr);
    D3D11_BLEND_DESC colorWrites{};if(colorBlend)colorBlend->GetDesc(&colorWrites);
    bool hasColor=false;
    for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i)if(colorTargets[i]){
        if(!colorBlend||colorWrites.RenderTarget[colorWrites.IndependentBlendEnable?i:0].RenderTargetWriteMask)hasColor=true;
        colorTargets[i]->Release();
    }
    if(!hasColor){Census(ctx,"live-depth-only",vp);return;}
    if(!info.lightSlot&&info.cb3Count==1990){
        ComPtr<ID3D11PixelShader> smoke;UINT bytes=sizeof(ID3D11PixelShader*);
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),NativeSmokeTag,&bytes,(void**)smoke.GetAddressOf()))&&smoke){
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);D3D11_BUFFER_DESC desc{};if(native)native->GetDesc(&desc);
            if(device&&native&&desc.ByteWidth>=NativeSunBufferBytes&&EnsureConstantClones(device.Get(),desc.ByteWidth)&&PrepareSmokeInputs(device.Get(),ctx)){
                ctx->CopyResource(state.raw.Get(),native.Get());
                const float zero[3]{};const D3D11_BOX radiance{NativeSunRadianceOffset,0,0,NativeSunRadianceOffset+12,1,1};
                ctx->UpdateSubresource(state.raw.Get(),0,&radiance,zero,0,0);ctx->CopyResource(state.clone.Get(),state.raw.Get());
                ctx->PSGetShaderResources(120,3,previousSmokeViews);ctx->PSGetSamplers(14,2,previousSmokeSamplers);ctx->PSGetConstantBuffers(12,2,previousSmokeConstants);
                ID3D11ShaderResourceView* views[]{state.smokeLightSrv.Get(),state.proofAtlas.Get(),state.probeBinding.irradiance};ctx->PSSetShaderResources(120,3,views);
                ID3D11SamplerState* samplers[]{state.probeBinding.sampler,state.proofComparison.Get()};ctx->PSSetSamplers(14,2,samplers);
                ID3D11Buffer* constants[]{state.smokeConstants.Get(),state.smokeProbes.Get()};ctx->PSSetConstantBuffers(12,2,constants);
                auto clone=state.clone.Get();ctx->PSSetConstantBuffers(3,1,&clone);
                original=native.Detach();originalShader=ps.Detach();ctx->PSSetShader(smoke.Get(),nullptr,0);
                smokeActive=true;context=ctx;return;
            }
        }
    }
    if(!info.lightSlot||(info.flags&(2u|32u))!=(2u|32u)){
        Census(ctx,"live-no-abi",vp);
        // Additive shafts and unlit effect cards have no light slot, so the
        // material path never sees them. Scale the add by the same night factor
        // as the skybox. Skybox-depth draws stay with the composite, which
        // already darkened the city.
        if(vp.MinDepth<.5f&&state.frame.relight.enabled){
            ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
            char name[96]{};UINT nameBytes=sizeof(name)-1;
            const bool sprite=vs&&SUCCEEDED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&nameBytes,name))
                &&(!strcmp(name,"spritecard.vfx_vs")||!strcmp(name,"csgo_effects.vfx_vs"));
            if(sprite){
                ComPtr<ID3D11BlendState> blend;float factor[4]{};UINT mask=0xffffffff;
                ctx->OMGetBlendState(&blend,factor,&mask);
                D3D11_BLEND_DESC blendDesc{};if(blend)blend->GetDesc(&blendDesc);
                const auto& rt=blendDesc.RenderTarget[0];
                const bool additive=rt.BlendEnable&&rt.DestBlend==D3D11_BLEND_ONE
                    &&(rt.SrcBlend==D3D11_BLEND_ONE||rt.SrcBlend==D3D11_BLEND_SRC_ALPHA);
                const float scale=UnlitOverlayScale(state.frame.relight.sunAngles.x,state.frame.relight.indirect);
                if(additive&&scale<.98f){
                    if(!state.unlitOverlayBlend){
                        auto scaled=blendDesc;scaled.RenderTarget[0].SrcBlend=D3D11_BLEND_BLEND_FACTOR;
                        scaled.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_BLEND_FACTOR;
                        scaled.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;
                        scaled.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE;
                        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
                        if(device)device->CreateBlendState(&scaled,&state.unlitOverlayBlend);
                    }
                    if(state.unlitOverlayBlend){
                        const float dim[4]={scale,scale,scale,scale};
                        previousOverlayBlend=blend.Detach();
                        std::copy(factor,factor+4,previousOverlayFactor);
                        previousOverlayMask=mask;
                        ctx->OMSetBlendState(state.unlitOverlayBlend.Get(),dim,mask);
                        overlayScaled=true;context=ctx;
                        if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();
                            if(now-last>1000){last=now;MvmDebugLog_LinefAlways("environment.unlit.overlay","shader=%s scale=%.3f elevation=%.2f",name,scale,state.frame.relight.sunAngles.x);}}
                    }
                }
            }
        }
        return;
    }
    if(MvmDebugLog_Active()){
        static std::map<uint64_t,bool> reported;
        if(reported.size()<512&&!reported[info.programHash]){
            reported[info.programHash]=true;
            MvmDebugLog_LinefAlways("relight.live.program","hash=%016llx flags=%u light=%u lightmap=%u probe=%u cascades=%u shadows=%u indirect=%g",
                info.programHash,info.flags,info.lightSlot,info.lightmapArrayMask,info.probeSlot,unsigned(info.ownedCascades),
                unsigned(state.frame.relight.shadows),state.frame.relight.indirect);
        }
    }
    if(state.frame.relight.programFilter==1&&!info.lightmapArrayMask)return;
    if(state.frame.relight.programFilter==2&&info.lightmapArrayMask)return;
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
    ComPtr<ID3D11HullShader> hs;ctx->HSGetShader(&hs,nullptr,nullptr);
    ComPtr<ID3D11DomainShader> ds;ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds){Census(ctx,"live-extra-stages",vp);return;}
    ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);
    D3D11_BUFFER_DESC desc{};if(native)native->GetDesc(&desc);
    if(!native||desc.ByteWidth<NativeSunBufferBytes||desc.ByteWidth>65536||desc.ByteWidth%16){Census(ctx,"live-cb3",vp);return;}
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!device||!EnsureConstantClones(device.Get(),info.ownedCascades?(std::max)(desc.ByteWidth,OwnedSunBufferBytes):desc.ByteWidth)){Census(ctx,"live-alloc",vp);return;}
    if(vp.MaxDepth>.1f+1e-6f)RestoreWorldSunTile(ctx);
    if(!BindOwnedInputs(ctx,device.Get(),vp,native.Get(),info)){RestoreOwnedInputs(ctx);Census(ctx,"live-bind-failed",vp);return;}
    // First-person draws read a close sun cascade redrawn in their own pose (once per frame). Only
    // after BindOwnedInputs, which prepares this frame's shadow maps: when the first-person block was
    // the frame's first lit draw, the pass backed up the previous frame's tile and the world was then
    // restored to it, so every body near the camera lost its sun shadow in first person (2026-10-13).
    if(viewmodelShadowPass&&vp.MaxDepth<=.1f+1e-6f&&state.frame.relight.shadows&&state.frame.relight.sunShadows)viewmodelShadowPass(ctx);
    if(state.frame.relight.bindingCheck){
        ComPtr<ID3D11PixelShader> coverage;UINT bytes=sizeof(ID3D11PixelShader*);
        const auto& tag=state.frame.relight.bindingCheck==2?BlackCoverageShaderTag:CoverageShaderTag;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),tag,&bytes,(void**)coverage.GetAddressOf()))&&coverage){
            originalShader=ps.Detach();ctx->PSSetShader(coverage.Get(),nullptr,0);
        }
    }else if((info.lightmapArrayMask&2)||info.probeSlot){
        ComPtr<ID3D11PixelShader> dynamic;UINT bytes=sizeof(ID3D11PixelShader*);
        const auto& probe=vp.MinDepth>0?state.skyboxBinding:state.probeBinding;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),DynamicIrradianceTag,&bytes,(void**)dynamic.GetAddressOf()))&&dynamic&&probe.irradiance){
            const auto& packing=probe.packing;
            std::array<float,40> rows{};
            for(unsigned axis=0;axis<3;++axis){
                rows[axis]=packing.probe[axis][axis];rows[4+axis]=packing.probe[axis][3];
                rows[8+axis]=packing.volume[3][axis]*(axis==2?6.f:1.f);
                rows[12+axis]=packing.volume[4][axis]*(axis==2?6.f:1.f);
            }
            rows[15]=probe.distance?1.f:0.f;
            ID3D11ShaderResourceView* screen=nullptr;
            if(AmbientViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth)&&state.frame.hasWorldToClip&&state.frame.relight.screenGi){
                UINT width=0,height=0;auto positions=AmbientOcclusionPositions(ctx,width,height);
                screen=Relight::PrepareScreenGi(device.Get(),ctx,probe,positions,width,height,
                    state.frame.camera,state.frame.worldToClip,state.probeFrame);
                if(screen){rows[15]=2;std::copy(state.frame.worldToClip.begin(),state.frame.worldToClip.end(),rows.begin()+16);
                    rows[36]=float(width);rows[37]=float(height);rows[38]=vp.Width/width;rows[39]=vp.Height/height;}
            }
            if(!state.irradianceConstants||rows!=state.irradiancePacking){
                D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(rows);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                d.Usage=D3D11_USAGE_DEFAULT;
                if(!state.irradianceConstants)device->CreateBuffer(&d,nullptr,&state.irradianceConstants);
                if(state.irradianceConstants){
                    ctx->UpdateSubresource(state.irradianceConstants.Get(),0,nullptr,rows.data(),0,0);
                    state.irradiancePacking=rows;
                }
            }
            if(state.irradianceConstants){
                ctx->PSGetConstantBuffers(13,1,&previousIrradianceConstants);
                ctx->PSGetShaderResources(126,1,&previousIrradiance);
                ctx->PSGetShaderResources(125,1,&previousDistance);
                ctx->PSGetShaderResources(124,1,&previousWorldIrradiance);
                ctx->PSGetSamplers(15,1,&previousIrradianceSampler);
                auto constants=state.irradianceConstants.Get();auto volume=screen?screen:probe.irradiance;auto sampler=probe.sampler;
                ctx->PSSetConstantBuffers(13,1,&constants);ctx->PSSetShaderResources(126,1,&volume);ctx->PSSetSamplers(15,1,&sampler);
                auto distance=probe.distance;ctx->PSSetShaderResources(125,1,&distance);
                auto world=probe.irradiance;ctx->PSSetShaderResources(124,1,&world);
                originalShader=ps.Detach();ctx->PSSetShader(dynamic.Get(),nullptr,0);
                irradianceActive=true;
            }
        }else Census(ctx,"live-irradiance-unsupported",vp);
    }
    if(!originalShader){
        ComPtr<ID3D11PixelShader> atmosphere;UINT bytes=sizeof(ID3D11PixelShader*);
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),NativeAtmosphereTag,&bytes,(void**)atmosphere.GetAddressOf()))&&atmosphere){
            originalShader=ps.Detach();ctx->PSSetShader(atmosphere.Get(),nullptr,0);
        }
    }
    BindAoView(ctx,device.Get(),vp);
    FinalizeOwnedConstants(ctx,device.Get(),native.Get());
    InspectLiveInputs(ctx,device.Get(),info,irradianceActive);
    context=ctx;original=native.Detach();
    // Live lighting already ran inside the native material draw, before MSAA
    // resolves. Report that ownership just as the HDR replay does: the late
    // display pass must not reconstruct and relight an already resolved edge.
    state.composited=true;state.renderedGeneration=state.frame.generation;
    Census(ctx,"live-lit",vp);
    BeginTiming(ctx,false,true);
}
void NativeMaterialLiveCost(float& prepareMs,float& ambientOcclusionMs,float& totalMs){
    prepareMs=liveCostMs[0].load(std::memory_order_relaxed);ambientOcclusionMs=liveCostMs[1].load(std::memory_order_relaxed);
    totalMs=liveCostMs[2].load(std::memory_order_relaxed);
}
NativeMaterialLiveScope::~NativeMaterialLiveScope(){
    if(!context)return;
    if(overlayScaled){
        context->OMSetBlendState(previousOverlayBlend,previousOverlayFactor,previousOverlayMask);
        if(previousOverlayBlend)previousOverlayBlend->Release();
        return;
    }
    if(!smokeActive)EndTiming(context);
    if(originalShader){
        context->PSSetShader(originalShader,nullptr,0);originalShader->Release();
    }
    if(smokeActive){
        context->PSSetShaderResources(120,3,previousSmokeViews);context->PSSetSamplers(14,2,previousSmokeSamplers);context->PSSetConstantBuffers(12,2,previousSmokeConstants);
        for(auto resource:previousSmokeViews)if(resource)resource->Release();
        for(auto sampler:previousSmokeSamplers)if(sampler)sampler->Release();
        for(auto constant:previousSmokeConstants)if(constant)constant->Release();
        context->PSSetConstantBuffers(3,1,&original);original->Release();return;
    }
    if(irradianceActive){
        context->PSSetConstantBuffers(13,1,&previousIrradianceConstants);
        context->PSSetShaderResources(126,1,&previousIrradiance);
        context->PSSetShaderResources(125,1,&previousDistance);
        context->PSSetShaderResources(124,1,&previousWorldIrradiance);
        context->PSSetSamplers(15,1,&previousIrradianceSampler);
        if(previousIrradianceConstants)previousIrradianceConstants->Release();
        if(previousIrradiance)previousIrradiance->Release();
        if(previousDistance)previousDistance->Release();
        if(previousWorldIrradiance)previousWorldIrradiance->Release();
        if(previousIrradianceSampler)previousIrradianceSampler->Release();
    }
    context->PSSetConstantBuffers(3,1,&original);original->Release();
    RestoreOwnedInputs(context);
}
bool BeginNativeMaterialDraw(ID3D11DeviceContext* ctx){
    if(ctx&&MvmDebugLog_Active()&&(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!state.wanted||state.active)){
        D3D11_VIEWPORT v{};UINT c=1;ctx->RSGetViewports(&c,&v);
        Census(ctx,ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE?"deferred":!state.wanted?"not-wanted":"nested",v);}
    if(!ctx||!state.wanted||state.active||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    if(count==1&&BeginTonemap(ctx,vp))return true;
    const bool viewmodel=std::abs(vp.MaxDepth-.1f)<1e-6f;
    if(count!=1||vp.Width<256||vp.Height<256||vp.MinDepth!=0||(!viewmodel&&std::abs(vp.MaxDepth-.95f)>1e-6f)){Census(ctx,"viewport",vp);return false;}
    Census(ctx,"viewport-ok",vp);
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);char name[128]{};UINT size=sizeof(name)-1;
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name)))return false;
    const bool supported=viewmodel?(!strcmp(name,"csgo_weapon.vfx_vs")||!strcmp(name,"csgo_character.vfx_vs")):
        (!strcmp(name,"csgo_lightmappedgeneric.vfx_vs")||!strcmp(name,"csgo_vertexlitgeneric.vfx_vs")||
         !strcmp(name,"csgo_complex.vfx_vs")||!strcmp(name,"csgo_simple.vfx_vs")||!strcmp(name,"csgo_simple_2way_blend.vfx_vs")||!strcmp(name,"csgo_static_overlay.vfx_vs"));
    if(!supported){Evidence((std::string("unsupported-family:")+name).c_str());return false;}
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);UINT tag=0;size=sizeof(tag);
    if(MvmDebugLog_Active()&&ps){
        NativeSunAbiInfo info{};UINT infoSize=sizeof(info);ps->GetPrivateData(SunAbiInfoTag,&infoSize,&info);
        static std::set<uint64_t> seen;
        if(seen.size()<64&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("environment.native.material.abi","vs=%s flags=%u cb3Count=%u hash=%016llx",name,info.flags,info.cb3Count,info.programHash);
    }
    if(!ps||FAILED(ps->GetPrivateData(SunAbiTag,&size,&tag))||tag!=1){Evidence("unsupported-sun-abi");return false;}
    NativeSunAbiInfo fixtureInfo{};UINT fixtureInfoSize=sizeof(fixtureInfo);
    state.fixtureAbi=SUCCEEDED(ps->GetPrivateData(SunAbiInfoTag,&fixtureInfoSize,&fixtureInfo))&&(fixtureInfo.flags&32);
    if(state.fixturePass&&!state.fixtureAbi)Evidence("unsupported-fixture-abi:retained-native-baseline");
    const bool owned=state.frame.proof.enabled||state.frame.relight.enabled;
    if(owned&&!fixtureInfo.lightSlot){Evidence("owned:no-derived-fixture-abi-native-retained");return false;}
    state.coveragePs.Reset();size=sizeof(ID3D11PixelShader*);
    if(FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),CoverageShaderTag,&size,(void**)state.coveragePs.GetAddressOf()))||!state.coveragePs){Evidence("unsupported-native-coverage");return false;}
    ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);D3D11_BUFFER_DESC bufferDesc{};
    if(native)native->GetDesc(&bufferDesc);
    if(bufferDesc.ByteWidth<NativeSunBufferBytes||bufferDesc.ByteWidth>65536||bufferDesc.ByteWidth%16){Evidence("lighting-buffer-size",bufferDesc.ByteWidth);return false;}
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
    ComPtr<ID3D11HullShader> hs;ctx->HSGetShader(&hs,nullptr,nullptr);
    ComPtr<ID3D11DomainShader> ds;ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds){Evidence("extra-shader-stages");return false;}
    ComPtr<ID3D11BlendState> blend;float factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC nativeBlend{};
    if(blend)blend->GetDesc(&nativeBlend);else nativeBlend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    // Preserve native blend equations in both HDR passes. Coverage runs the same
    // PS and alpha-to-coverage, but writes a validity bit without color blending.
    if(nativeBlend.IndependentBlendEnable){Evidence("independent-blend-pass");return false;}
    // Alpha-to-coverage retains native per-sample coverage. Blended overlays still
    // require a validated ordering contract before replacing their background.
    if(owned&&nativeBlend.RenderTarget[0].BlendEnable){Evidence("owned:blended-material-not-yet-validated");return false;}
    ComPtr<ID3D11RenderTargetView> targets[8];ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* rawTargets[8]{};ctx->OMGetRenderTargets(8,rawTargets,depth.GetAddressOf());
    for(unsigned i=0;i<8;++i)targets[i].Attach(rawTargets[i]);
    if(!targets[0]||!depth){Evidence("no-color-or-depth");return false;}
    for(unsigned i=1;i<8;++i)if(targets[i]){Evidence("multiple-render-targets");return false;}
    ComPtr<ID3D11Resource> resource,depthResource;targets[0]->GetResource(&resource);depth->GetResource(&depthResource);
    ComPtr<ID3D11Texture2D> texture,depthTexture;
    if(FAILED(resource.As(&texture))||FAILED(depthResource.As(&depthTexture)))return false;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC rt{};targets[0]->GetDesc(&rt);
    if(td.ArraySize!=1||td.MipLevels!=1||td.Width!=UINT(vp.Width)||td.Height!=UINT(vp.Height)){Evidence("target-dimensions");return false;}
    if(state.captured&&(state.source.Get()!=texture.Get()||state.depth.Get()!=depthTexture.Get()))return false;
    ComPtr<ID3D11DepthStencilState> previousDepth;UINT stencil=0;ctx->OMGetDepthStencilState(&previousDepth,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};
    if(previousDepth)previousDepth->GetDesc(&dd);else {dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;}
    // Native forward materials can use a read-only EQUAL pass after the depth prepass.
    if(!dd.DepthEnable){Evidence("depth-disabled");return false;}
    if(dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO)dd.DepthFunc=D3D11_COMPARISON_EQUAL;
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilWriteMask=0;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!Resources(device.Get(),td,rt,bufferDesc.ByteWidth)){Evidence("resource-allocation");return false;}
    auto coverageBlend=nativeBlend;coverageBlend.RenderTarget[0].BlendEnable=FALSE;coverageBlend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    if(!state.maskBlend||std::memcmp(&coverageBlend,&state.maskBlendDesc,sizeof(coverageBlend))){
        state.maskBlend.Reset();if(FAILED(device->CreateBlendState(&coverageBlend,&state.maskBlend)))return false;state.maskBlendDesc=coverageBlend;
    }
    if(!state.depthState||std::memcmp(&dd,&state.depthDesc,sizeof(dd))){
        state.depthState.Reset();if(FAILED(device->CreateDepthStencilState(&dd,&state.depthState)))return false;state.depthDesc=dd;}
    // Clone on the GPU: no readback stall and no mutation of engine-owned values.
    BeginTiming(ctx,false);
    ctx->CopyResource(state.raw.Get(),native.Get());
    if(owned&&MvmDebugLog_Active()) {
        static uint64_t sampled=~0ull;
        if(sampled!=state.frame.relight.generation) {
            sampled=state.frame.relight.generation;
            D3D11_BUFFER_DESC d{};native->GetDesc(&d);
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
            ComPtr<ID3D11Buffer> readback;
            if(SUCCEEDED(device->CreateBuffer(&d,nullptr,&readback))) {
                ctx->CopyResource(readback.Get(),native.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                if(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped))) {
                    const float* values=static_cast<const float*>(mapped.pData);
                    MvmDebugLog_LinefAlways("relight.native.reference","generation=%llu sunDirection=(%g %g %g) sunRadiance=(%g %g %g) ambientFloor=(%g %g %g)",
                        sampled,values[1945*4],values[1945*4+1],values[1945*4+2],values[1946*4],values[1946*4+1],values[1946*4+2],values[18*4],values[18*4+1],values[18*4+2]);
                    ctx->Unmap(readback.Get(),0);
                }
            }
        }
    }
    auto patchRange=[&](UINT offset,UINT bytes,const void* value){
        const D3D11_BOX range{offset,0,0,offset+bytes,1,1};
        ctx->UpdateSubresource(state.raw.Get(),0,&range,value,0,0);
    };
    if(!state.frame.relight.reference) {
    patchRange(NativeSunDirectionOffset,12,state.next.direction);
    patchRange(NativeSunRadianceOffset,12,state.next.radiance);
    const UINT noNativeCascades=0;
    patchRange(NativeSunCascadesOffset,sizeof(noNativeCascades),&noNativeCascades);
    if(owned){
        if(!ProofResources(device.Get(),ctx,vp)){Evidence("owned:resource-allocation-failed");return false;}
        const UINT zeros[4]{};
        patchRange(NativeSunRadianceOffset,12,zeros);
        // cb3[0..3] carry the shadow filter kernel. Zeroing them, as the single-fixture
        // proof did, collapses all nine taps onto one texel and every shadow becomes a
        // hard binary edge.
        float filterRows[16];
        Relight::ShadowFilterRows(state.frame.relight.enabled?Relight::NativeFilterTexels:0.f,filterRows);
        patchRange(0,sizeof(filterRows),filterRows);
        const float bias=state.frame.relight.enabled?-state.frame.relight.shadowBias:-.0002f;patchRange(1953*16+8,4,&bias);
        // cb3[19].z is how many 32-bit mask words the clustered loop reads,
        // so it is what raises capacity past a single light.
        const UINT words=state.frame.relight.enabled&&state.frame.relight.cost==1?1:state.proofMaskWords;
        auto cull=state.clusterGrid.ListRow(state.clusterGrid.lights,state.clusterMode);cull[2]=words;
        const auto tiles=state.clusterGrid.TileConstants(state.clusterMode);
        const auto slices=state.clusterGrid.SliceConstants(state.clusterMode);
        patchRange(19*16,16,cull.data());patchRange(20*16,16,zeros);patchRange(21*16,16,zeros);
        patchRange(22*16,16,tiles.data());patchRange(23*16,16,slices.data());
        // GPU validates cb1[20] before publishing the real grid. An unsupported
        // viewport receives the identical full-mask constants without a readback stall.
        if(state.clusterMode) {
            const D3D11_BOX rows{0,0,0,80,1,1};
            ctx->CopySubresourceRegion(state.raw.Get(),0,19*16,0,0,state.clusterRows.Get(),0,&rows);
        }
        float ambient[12]{};
        if(state.frame.relight.enabled){
            // A sky-over-ground hemisphere rather than a flat constant, so an upward
            // face sees the sky and a downward face sees the bounce. The native shader
            // multiplies this by the surface albedo on our behalf.
            const auto& set=state.frame.relight;
            Relight::AmbientRows(Relight::SkyAmbient(set,set.indirect),{},ambient);
        }else{
            const float fill=state.frame.proof.indirect;
            const float flat[12]{0,0,0,fill,0,0,0,fill,0,0,0,fill};
            std::copy(std::begin(flat),std::end(flat),std::begin(ambient));
        }
        if(state.frame.relight.enabled&&state.frame.relight.probeCheck)
            Relight::AmbientRows(Relight::ProbeNeutral,Relight::ProbeNeutral,ambient);
        patchRange(7*16,sizeof(ambient),ambient);
        // cb3[18] is a flat ambient floor the shader adds regardless of sun visibility
        // or surface orientation, and it is the channel that actually reaches an unlit
        // face -- the hemisphere rows above only fill the share the probe loop leaves
        // behind. Because it is flat, whatever goes here is added to every surface
        // equally, so putting the hemisphere AVERAGE in it lights the shadowed side of a
        // wall exactly as brightly as the lit side and the whole frame reads as
        // fullbright. Each row holds (sky+ground)/2 in .w and (sky-ground)/2 in .z, so
        // .w-.z is the ground term: the least any face can receive. The sky share is left
        // to the orientation-dependent rows, which is where it belongs.
        const float floorTerm[4]{ambient[3]-ambient[2],ambient[7]-ambient[6],
                                 ambient[11]-ambient[10],0};
        if(state.frame.relight.enabled&&!(fixtureInfo.lightmapArrayMask&2))patchRange(18*16,16,floorTerm);
        else patchRange(18*16,16,zeros);
        ctx->CopyResource(state.clone.Get(),state.raw.Get());
        state.proofLightSlot=fixtureInfo.lightSlot;
        // t7 is the engine's screen-space ambient occlusion, a multiplier on the ambient
        // term. It is left alone deliberately. Replacing it with black deletes all ambient
        // light, and replacing it with white -- which this did -- deletes every contact
        // shadow and crevice in the frame, which is most of what makes a surface read as
        // lit at all. It is a screen-space term computed from the depth buffer, so it
        // describes the geometry in front of the camera and not the lighting we replaced;
        // it stays valid when the light set changes and is the only occlusion the small
        // detail in the frame gets.
        UINT slots[10]{12,13,state.proofLightSlot,6};
        ID3D11ShaderResourceView* views[10]{state.proofBlackCube.Get(),state.proofMaskSrv.Get(),state.proofLightSrv.Get(),state.proofAtlas.Get()};
        // Neutralise only the baked arrays this program actually declares. A slot it
        // types differently has to keep whatever the engine bound there.
        unsigned bindings=4;
        const auto& probe=state.probeBinding;
        if(probe.constants&&fixtureInfo.probeSlot) {
            // All table entries name our one map-wide probe. The shared tile/slice
            // grid can change independently; there is no probe-grid capacity limit.
            const UINT table[4]{state.clusterGrid.probes.slices,state.clusterGrid.probes.tiles,1,0};
            patchRange(21*16,16,table);patchRange(1177*16,6*16,probe.packing.probe.data());
            patchRange(1947*16,16,zeros);
            // The traced sky already includes occlusion. A flat floor would add that
            // energy back in shadow; retain it only for the neutral ABI comparison.
            if(!state.frame.relight.probeCheck)patchRange(18*16,16,zeros);
            ctx->PSGetConstantBuffers(2,1,state.previousProbeConstants.ReleaseAndGetAddressOf());
            auto cb2=probe.constants;ctx->PSSetConstantBuffers(2,1,&cb2);state.probeActive=true;
            slots[bindings]=fixtureInfo.probeSlot;views[bindings++]=probe.irradiance;
            if(fixtureInfo.probeOcclusionSlot){slots[bindings]=fixtureInfo.probeOcclusionSlot;views[bindings++]=probe.sunOcclusion;}
            if(MvmDebugLog_Active()) {
                static uint64_t logged=~0ull;
                if(logged!=state.frame.relight.generation){logged=state.frame.relight.generation;
                    MvmDebugLog_LinefAlways("relight.probe","generation=%llu hash=%016llx cb2=%p irradianceSlot=%u occlusionSlot=%u neutral=%d",
                        logged,fixtureInfo.programHash,cb2,fixtureInfo.probeSlot,fixtureInfo.probeOcclusionSlot,state.frame.relight.probeCheck==2);}
            }
        }
        for(unsigned array=0;array<3;++array)if(fixtureInfo.lightmapArrayMask&(1u<<array)){
            // t3 is geometric bent-normal data, not radiance. Keep its normal response.
            // Send owned fill through t2 so native diffuse energy and ambient occlusion
            // apply; cb3[18] bypasses those terms and made recesses uniformly bright.
            if(state.frame.relight.enabled&&array==2)continue;
            slots[bindings]=array+1;
            // Once per session, record the engine's lightmap dimensions. Calculated world
            // irradiance must eventually match this atlas layout, not our diagnostic size.
            if(array==1&&!state.engineLightmapWidth){
                ComPtr<ID3D11ShaderResourceView> engine;
                ctx->PSGetShaderResources(2,1,&engine);
                if(engine){
                    ComPtr<ID3D11Resource> resource;engine->GetResource(&resource);
                    ComPtr<ID3D11Texture2D> texture;if(resource&&SUCCEEDED(resource.As(&texture))){
                        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                        state.engineLightmapWidth=desc.Width;state.engineLightmapHeight=desc.Height;
                        state.engineLightmapArray=desc.ArraySize;
                        if(MvmDebugLog_Active())
                            MvmDebugLog_LinefAlways("relight.lightmap.engine","width=%u height=%u array=%u format=%u",
                                desc.Width,desc.Height,desc.ArraySize,unsigned(desc.Format));
                    }
                }
            }
            views[bindings]=state.frame.relight.enabled&&array==1?state.relightAmbientSrv.Get():state.proofBlackArray.Get();
            ++bindings;
        }
        state.proofResources.Bind(ctx,bindings,slots,views);
        state.proofSamplerSlot=fixtureInfo.comparisonSampler;ctx->PSGetSamplers(state.proofSamplerSlot,1,state.proofPreviousSampler.ReleaseAndGetAddressOf());auto sampler=state.proofComparison.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);
        state.proofActive=true;
        const uint64_t generation=state.frame.relight.enabled?state.frame.relight.generation:state.frame.proof.generation;
        static uint64_t logged=~0ull;if(logged!=generation){logged=generation;MvmDebugLog_LinefAlways("environment.owned","generation=%llu bound hash=%016llx slot=%u sampler=%u records=%u words=%u",generation,fixtureInfo.programHash,fixtureInfo.lightSlot,fixtureInfo.comparisonSampler,state.proofRecordCount,state.proofMaskWords);}
    }
    ctx->CopyResource(state.clone.Get(),state.raw.Get());
    }
    if(state.frame.relight.reference)ctx->CopyResource(state.clone.Get(),state.raw.Get());
    if(!state.captured){const float clear[4]{};ctx->ClearRenderTargetView(state.rtv.Get(),clear);ctx->ClearRenderTargetView(state.coverageRtv.Get(),clear);ctx->ClearRenderTargetView(state.baselineRtv.Get(),clear);
        if(state.fixturePass)ctx->ClearRenderTargetView(state.withoutFixturesRtv.Get(),clear);
        state.rendered=state.next;state.renderedGeneration=state.generation;}
    state.previousConstants=native;state.previousRtv=targets[0];state.previousDsv=depth;
    state.previousDepthState=previousDepth;state.previousStencil=stencil;
    state.previousBlend=blend;std::memcpy(state.previousFactors,factors,sizeof(factors));state.previousMask=mask;
    auto cb=state.clone.Get();auto target=state.rtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,depth.Get());ctx->OMSetDepthStencilState(state.depthState.Get(),stencil);
    ctx->OMSetBlendState(blend.Get(),factors,mask);
    if(MvmDebugLog_Active()){static bool once=false;if(!once){once=true;char textureName[256]{};UINT length=255;texture->GetPrivateData(WKPDID_D3DDebugObjectName,&length,textureName);
        MvmDebugLog_LinefAlways("environment.native.material.target","name=%s resource=%p textureFormat=%u viewFormat=%u samples=%u",textureName,texture.Get(),td.Format,rt.Format,td.SampleDesc.Count);}}
    state.source=texture;state.depth=depthTexture;state.sourceDsv=depth;state.worldActive=!viewmodel;state.active=true;return true;
}
bool BeginNativeMaterialBaseline(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive)return false;
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){state.proofResources.Restore(ctx);auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;}
    auto cb=state.previousConstants.Get();auto target=state.baselineRtv.Get();
    ctx->PSSetConstantBuffers(3,1,&cb);ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());return true;
}
bool BeginNativeMaterialWithoutFixtures(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive||state.proofActive||!state.fixturePass)return false;
    auto cb=state.previousConstants.Get();
    if(state.fixtureAbi){
        ++state.fixtureDraws;
        // Clone the ORIGINAL baseline, not the custom sun pass. Only clustered
        // direct evaluation changes; native textures, emission, shadow/cookie data,
        // coverage and blending remain identical in this differential pair.
        ctx->CopyResource(state.raw.Get(),state.previousConstants.Get());
        const UINT zero=0;const D3D11_BOX range{NativeFixtureWordsOffset,0,0,NativeFixtureWordsOffset+4,1,1};
        ctx->UpdateSubresource(state.raw.Get(),0,&range,&zero,0,0);
        ctx->CopyResource(state.clone.Get(),state.raw.Get());cb=state.clone.Get();
    }
    auto target=state.withoutFixturesRtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());return true;
}
bool BeginNativeMaterialCoverage(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive||state.proofActive||state.coverageActive)return false;
    auto cb=state.previousConstants.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->PSGetShader(&state.previousPs,nullptr,nullptr);
    auto target=state.coverageRtv.Get();ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());
    ctx->PSSetShader(state.coveragePs.Get(),nullptr,0);ctx->OMSetBlendState(state.maskBlend.Get(),state.previousFactors,state.previousMask);state.coverageActive=true;return true;
}
void EndNativeMaterialDraw(ID3D11DeviceContext* ctx){
    if(!state.active||!ctx)return;
    EndTiming(ctx);
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){
        state.proofResources.Restore(ctx);
        auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;
    }
    if(state.coverageActive){ctx->PSSetShader(state.previousPs.Get(),nullptr,0);state.previousPs.Reset();state.coverageActive=false;}
    if(state.postActive){
        auto input=state.previousInput.Get();auto target=state.previousRtv.Get();ctx->PSSetShaderResources(12,1,&input);ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());
        state.previousInput.Reset();state.previousRtv.Reset();state.previousDsv.Reset();state.postActive=state.active=false;state.toned=true;Evidence("native-tonemap-replay");return;
    }
    auto cb=state.previousConstants.Get();auto target=state.previousRtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());ctx->OMSetDepthStencilState(state.previousDepthState.Get(),state.previousStencil);
    ctx->OMSetBlendState(state.previousBlend.Get(),state.previousFactors,state.previousMask);state.previousBlend.Reset();
    state.previousConstants.Reset();state.previousRtv.Reset();state.previousDsv.Reset();state.previousDepthState.Reset();
    state.active=false;state.captured=true;++state.draws;if(state.worldActive)++state.worldDraws;Evidence("native-sun-replay",NativeSunBufferBytes);
}
namespace {
Sun FrameSun(const Frame& frame){
    auto climate=EffectiveClimate(frame.scene.climate);

    const auto direction=ClimateSunDirection(climate);Sun next{};next.direction[0]=direction.x;next.direction[1]=direction.y;next.direction[2]=direction.z;
    const float mix=std::clamp((climate[CL_sunElevation]+4)/30,0.f,1.f);
    const float power=climate[CL_sunPower]*climate[CL_sunRelight]*ClimateSunVisibility(climate);
    next.radiance[0]=power;next.radiance[1]=(.36f+.58f*mix)*power;next.radiance[2]=(.12f+.71f*mix)*power;
    return next;
}
}
void PrepareNativeMaterialFrame(const Frame& frame){
    state.skyApplied=false;
    NextAmbientOcclusionFrame();
    NextSunVisibilityFrame();
    ++state.probeFrame;
    state.proofPrepared=false;state.proofResourcesReady=false;state.proofPreparedView.Reset();state.proofPreparedViewport.Reset();state.proofUploads=0;
    state.proofAuthoredUploads=0;state.proofAuthoredBytes=0;state.proofViewGate.Invalidate();
    const auto climate=EffectiveClimate(frame.scene.climate);
    // Owned lighting substitutes the lightmap itself, with its own fill; the plain unbake is for
    // every other frame that asked for the baked lighting to go. It runs as its own per-draw
    // scope on the engine's draw, so it deliberately does NOT request the replay pass: asking
    // for one would cost a full private-target render to deliver a substitution the scope has
    // already made in place.
    state.unbake=frame.active&&!frame.proof.enabled&&!frame.relight.enabled&&climate[CL_bakedSuppressed]>.5f;
    state.unbakeFillValue=climate[CL_bakedFill];
    // Whole-map relighting no longer asks for the replay: it lights the engine's draw in place.
    state.live=frame.active&&frame.relight.enabled&&!frame.relight.reference&&!frame.proof.enabled;
    state.requested=frame.active&&(frame.proof.enabled||climate[CL_relighting]>.5f);
    state.wanted=state.requested;
    state.fixturePass=state.requested&&!frame.proof.enabled&&!frame.relight.enabled&&std::abs(climate[CL_ambient]-1.f)>1e-6f;
    state.next=FrameSun(frame);state.generation=frame.generation;state.frame=frame;
}
bool BindNativeMaterial(ID3D11DeviceContext* ctx,ID3D11Texture2D* target,ID3D11Texture2D* depth,const Frame& frame,ID3D11Texture2D* nativeHdrSource){
    const auto climate=EffectiveClimate(frame.scene.climate);
    // Owned lighting zeroes CL_relighting in its own frame, so asking the climate
    // whether it wants a replacement would answer no for the very pass that needs one.
    const bool requested=frame.active&&(frame.proof.enabled||frame.relight.enabled||climate[CL_relighting]>.5f);
    const Sun next=FrameSun(frame);
    bool targetMatches=state.toned?state.tonedSource.Get()==target:state.source.Get()==target;
    if(frame.hdr){
        D3D11_TEXTURE2D_DESC copy{},original{};
        if(target)target->GetDesc(&copy);
        if(state.color)state.color->GetDesc(&original);
        // A matching size is insufficient: only the capture callback can attest that
        // this private HDR copy came from the exact source of this frame's replay.
        targetMatches=nativeHdrSource&&nativeHdrSource==state.source.Get()&&
            copy.Width==original.Width&&copy.Height==original.Height&&copy.ArraySize==1&&
            copy.MipLevels==1&&copy.SampleDesc.Count==1&&copy.Format==original.Format&&
            (copy.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||copy.Format==DXGI_FORMAT_R32G32B32A32_FLOAT);
    }
    const bool valid=requested&&state.captured&&frame.generation==state.renderedGeneration&&targetMatches&&state.depth.Get()==depth&&(frame.proof.enabled||frame.relight.enabled||std::memcmp(&next,&state.rendered,sizeof(next))==0)&&
        (state.toned||ResolveMaterial(ctx));
    if(MvmDebugLog_Active()){
        static ULONGLONG last[2]{};const auto now=GetTickCount64();auto& logged=last[frame.hdr?1:0];if(now-logged>3000){logged=now;
            MvmDebugLog_LinefAlways("environment.native.material.bind","generation=%llu renderedGeneration=%llu hdr=%u valid=%u captured=%u targetMatch=%u depthMatch=%u sunMatch=%u draws=%u worldDraws=%u fixtureDraws=%u fixturePass=%u source=%p target=%p",
                frame.generation,state.renderedGeneration,UINT(frame.hdr),UINT(valid),UINT(state.captured),UINT(targetMatches),UINT(state.depth.Get()==depth),UINT(std::memcmp(&next,&state.rendered,sizeof(next))==0),state.draws,state.worldDraws,state.fixtureDraws,UINT(state.fixturePass),state.source.Get(),target);}}

    ID3D11ShaderResourceView* views[]={valid?(frame.hdr?state.resolvedSrv.Get():(state.toned?state.tonedSrv.Get():state.srv.Get())):nullptr,valid?(frame.hdr?state.coverageResolvedSrv.Get():state.coverageSrv.Get()):nullptr};ctx->PSSetShaderResources(22,2,views);
    auto baseline=valid?(frame.hdr?state.baselineResolvedSrv.Get():state.baselineSrv.Get()):nullptr;ctx->PSSetShaderResources(24,1,&baseline);
    // No extra replay/allocation at neutral ambient. Unsupported variants replay
    // unchanged, yielding zero fixture delta rather than inventing light energy.
    auto withoutFixtures=valid&&state.fixturePass?(frame.hdr?state.withoutFixturesResolvedSrv.Get():state.withoutFixturesSrv.Get()):baseline;
    ctx->PSSetShaderResources(25,1,&withoutFixtures);return valid;
}
}
