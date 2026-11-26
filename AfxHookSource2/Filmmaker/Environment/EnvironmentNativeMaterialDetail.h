#pragma once
// The native material driver's own state, shared by its translation units
// (EnvironmentNativeMaterial.cpp: frame preparation, light lists, tonemap;
// EnvironmentNativeMaterialDerive.cpp: deriving the owned shader variants;
// EnvironmentNativeMaterialDraw.cpp: binding the owned inputs around each lit draw).
// The state is inline, so the units see one copy. Nothing outside those files includes this.
#include "EnvironmentNativeMaterial.h"
#include "../Render/RenderPixelStateLayer.h"
#include "../Debug/GpuPassProfiler.h"
#include "OwnedInterface.h"
#include "EnvironmentAnimatedCasters.h"
#include "EnvironmentNativeMaterialMath.h"
#include "EnvironmentClimateMath.h"
#include "EnvironmentConstantPatchMath.h"
#include "EnvironmentNativePostMath.h"
#include "../Relight/RelightMath.h"
#include "../Relight/RelightClusterMath.h"
#include "../Relight/RelightLitBoundsMath.h"
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
#include "EnvironmentNativeScaledColorMath.h"
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
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
// RenderSystemDX11Hooks: our own shader creations on this thread are not counted as CS2's.
void RenderShaderCost_OwnWork(bool begin);
namespace Filmmaker::Environment {
namespace NativeDetail {
using Microsoft::WRL::ComPtr;
constexpr GUID SunAbiInfoTag={0xc93635ba,0x7dca,0x49ab,{0xb5,0xee,0xab,0x60,0x17,0x0d,0x7c,0x94}};
constexpr GUID CoverageShaderTag={0x6c1edb13,0xeca5,0x4e44,{0x89,0x37,0x62,0x2e,0xae,0x1a,0xf5,0xf7}};
constexpr GUID BlackCoverageShaderTag={0x39e29345,0x4b16,0x4534,{0xac,0x89,0x7a,0x24,0xc0,0xea,0x5b,0x17}};
constexpr GUID SunAbiTag={0x3f61bdd1,0x622a,0x49f0,{0xae,0x7f,0x71,0xbc,0xda,0x42,0x66,0x21}};
constexpr GUID PostCompressionTag={0x61859533,0x6b4a,0x45b0,{0x90,0x5f,0x2e,0x5b,0x12,0xa1,0xc3,0xe7}};
constexpr GUID SharedAoShaderTag={0x72546290,0xe394,0x4a17,{0x9c,0xf0,0x17,0xb9,0x8a,0xfa,0x3e,0x11}};
constexpr GUID UnchangedAoViewTag={0x417c643b,0x9a52,0x4823,{0xb1,0x71,0xa6,0x40,0x68,0x54,0x3c,0x22}};
constexpr GUID DynamicIrradianceTag={0x1fd049d5,0x52d1,0x4337,{0xa9,0x65,0x71,0x21,0x44,0xa4,0x48,0x94}};
// Set on a queued program once a draw has moved it to the front of the derivation queue.
constexpr GUID DerivationWantedTag={0xccfd28be,0xdbf9,0x46e4,{0x82,0x69,0x58,0x6f,0x2e,0x4f,0x7f,0xfc}};
constexpr GUID NativeAtmosphereTag={0x5c3f07b1,0xc2b7,0x4a6f,{0x91,0x23,0x60,0x48,0x45,0xe1,0x9a,0xb2}};
constexpr GUID NativeSmokeTag={0x07ca4708,0x7687,0x41c4,{0x90,0x4e,0xc7,0xb3,0x58,0x6d,0x0b,0x92}};
constexpr GUID SharedLightingShaderTag={0xd706814b,0xb394,0x405b,{0x95,0x66,0x35,0xd2,0x1c,0xa7,0x97,0xac}};
constexpr GUID SharedLightingInfoTag={0x80f77a69,0x4e85,0x437b,{0x9e,0x14,0xc4,0x62,0x2f,0x80,0x64,0x19}};
constexpr GUID SparseClusterInfoTag={0xf5c0c203,0x47a8,0x49e0,{0xb0,0x90,0xa4,0x47,0x41,0xd3,0x53,0x17}};
constexpr GUID SparseClusterShaderTag={0x4ac1e69b,0x03cf,0x4eb0,{0x95,0x6d,0x81,0xfa,0x6c,0x45,0x7c,0x42}};
// The 3D skybox's alpha-blended unlit cards (EnvironmentNativeScaledColorMath.h): the native
// program's own bytecode, kept on effect-family programs so the variant can be made when one is
// first drawn in the skybox; the variant; and the mark that it has been asked for.
constexpr GUID ScaledColorCodeTag={0x2b8f6e1d,0x91c4,0x4d27,{0xa6,0x3e,0x5f,0x0b,0x72,0xd9,0x14,0xc8}};
constexpr GUID ScaledColorShaderTag={0x8d71a2c5,0x3f0e,0x4b9a,{0x8e,0x52,0x16,0xc7,0xa4,0x3b,0x90,0x6f}};
constexpr GUID ScaledColorWantedTag={0x5e9c0a47,0xd2b3,0x4f18,{0xb4,0x0d,0x7a,0x61,0xe8,0x25,0xc3,0x9e}};
struct Sun {float direction[4]{},radiance[4]{};};
inline std::map<uint64_t,std::vector<uint8_t>>& programRecord(){static std::map<uint64_t,std::vector<uint8_t>> record;return record;}
struct MaterialTiming {
    // Every lit pass of a frame (about 900 at a busy view), so the report is the frame's whole
    // material cost rather than its first passes. Sampled once every three seconds.
    static constexpr unsigned Capacity=1536;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> disjoint,start[Capacity],end[Capacity];
    bool measuring=false,pending=false,post[Capacity]{},live[Capacity]{};
    // What each live pass was: its program, which owned variant ran, and its viewport
    // (the main view, or a secondary render such as a capture), for the grouped report.
    uint64_t program[Capacity]{};uint8_t variant[Capacity]{};uint16_t viewW[Capacity]{},viewH[Capacity]{};float viewMin[Capacity]{};
    unsigned count=0,eligible=0;int active=-1;ULONGLONG last=0;
};
inline NativeMaterialHdrPass hdrPass=nullptr;
// Shader creation can precede renderer resource preparation. Keep this ABI
// observation across resource resets; the already tagged shaders remain alive.
inline std::atomic<bool> nativePostCompressedHdr{false};
inline LightingProofShadowPass proofShadowPass=nullptr;
inline SkyVisibilitySource skyVisibilitySource=nullptr;
inline ViewmodelShadowPass viewmodelShadowPass=nullptr;
inline GlassBeamShadowPass glassBeamShadowPass=nullptr;
inline MapLightShadowPass mapShadowPass=nullptr;
// Set once a derived program cannot read cube shadow records (SmoothNativeLampKernels cubeReady).
inline std::atomic<bool> g_cubeShadowUnsupported{false};
// Live lighting's draw-setup CPU time, split into laps: 0 everything else, 1 light preparation,
// 2 ambient occlusion. Render thread only; NextNativeMaterialFrame folds it into averages the
// main-thread heartbeat reads ("live ... prepare ... ao").
inline std::array<long long,3> liveLaps{};inline LARGE_INTEGER liveLapClock{};
inline std::array<std::atomic<float>,3> liveCostMs{}; // prepare, ambient occlusion, total; ~20-frame averages
inline void LiveLapStart(){QueryPerformanceCounter(&liveLapClock);}
inline void LiveLap(unsigned i){LARGE_INTEGER n;QueryPerformanceCounter(&n);liveLaps[i]+=n.QuadPart-liveLapClock.QuadPart;liveLapClock=n;}
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
    // The texture the main view's lit draws render to this frame (compared, never dereferenced): an
    // effect card drawn elsewhere is in CS2's transparency colour pass (UnlitCardDim). Cleared with
    // each new frame (PrepareNativeMaterialFrame), recorded by its first main-view lit draw.
    const void* sceneTarget=nullptr;
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
    // What the lit draw in progress bound, by slot, for a substitution that outlives the draw
    // (PixelStateLayer). litDraws counts the frame's lit draws, set up or reused.
    std::array<ID3D11ShaderResourceView*,RenderPixelResourceBindings::Capacity> proofOurViews{};
    ID3D11Buffer* proofOurProbeConstants=nullptr;ID3D11Buffer* proofOurLighting=nullptr;
    // The draw being set up got a "not yet this frame" answer for ambient occlusion or sun visibility.
    bool inputsProvisional=false;
    // A draw is in progress on inputs the previous lit draw left bound: nested draws are left alone,
    // as they are while proofActive marks a draw set up for itself.
    bool reuseActive=false;
    unsigned litDraws=0;
    ComPtr<ID3D11Buffer> proofLights,proofMasks,proofAuthored,proofRebaseParams;
    ComPtr<ID3D11Buffer> proofLocalLights,sunCascadeParams,sunCascadeColumns,characterSunCascadeColumns;
    ComPtr<ID3D11UnorderedAccessView> characterSunCascadeUav;
    // Copy of the native view constants (b4) with the AO addressing row repaired; see BindOwnedInputs.
    // The traced bounce volume carries its own sky visibility; the screen-space sky term must
    // then stay out of the AO, which also scales the bounce it would wrongly darken indoors.
    bool probeTraced=false;
    bool liteReleasedProbes=false; // Lite lighting already dropped the traced bounce volume
    bool liteGrid=false; // Lite lighting is reading its bounce grid, not the map-wide fill
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
    // The frame's light records, built once for the world passes and once for the first-person pass
    // (which scales effect lights). Every view of a frame reads the same lights, tiles and matrices.
    // litBounds: one row per record, or empty when no record has bounds.
    struct RecordCache {bool valid=false;std::vector<Relight::LightRecord> records;ID3D11ShaderResourceView* glassCookies=nullptr;
        std::vector<Relight::LightRecord> bounceRecords;
        std::vector<Relight::LitBoundsRow> litBounds;};
    // mirv_relight dumpview: staging copies of the engine's b1 and b4 for one world lit draw.
    ComPtr<ID3D11Buffer> viewDump[2];UINT viewDumpBytes[2]{};bool viewDumpPending=false;
    // One row of the engine's per-view constants, read back without waiting (NativeExposureScale).
    ComPtr<ID3D11Buffer> exposureReadback;bool exposurePending=false;uint64_t exposureFrame=~0ull;
    ComPtr<ID3D11Buffer> proofLitBounds;ComPtr<ID3D11ShaderResourceView> proofLitBoundsSrv;
    std::vector<Relight::LitBoundsRow> proofUploadedLitBounds;bool proofLitBoundsUploaded=false,proofLitBoundsActive=false;
    RecordCache proofRecordCache[2];unsigned proofRecordBuilds=0,proofRecordReuses=0;
    LightingViewGate proofViewGate;
    ComPtr<ID3D11Buffer> proofViewGateReadback;bool proofViewGateReadbackPending=false;
    Relight::ClusterGrid clusterGrid{};
    Relight::NativeClusterWork nativeClusters;
    unsigned clusterMode=0;bool clusterSummaries=false;
    // The world pass's cluster mode, kept past the first-person pass (which sets up without clusters and
    // clears clusterMode): the composite reads the world's light lists for its full-screen pixels.
    unsigned worldClusterMode=0;
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
};
inline State state;
// Local-light receiver bias for player bodies (cb3[1953].z is shared by every light record).
inline float CharacterLocalBias(const Relight::Scene& set){return (std::max)(set.shadowBias,.002f);}
inline void BeginTiming(ID3D11DeviceContext* ctx,bool post,bool live=false,uint64_t program=0,uint8_t variant=0,const D3D11_VIEWPORT* vp=nullptr){
    auto& t=state.timing;t.active=-1;
    // One frame every three seconds for the log, every second while the GPU pass profile is on.
    const bool profiling=GpuProfile::Enabled();
    if(!(MvmDebugLog_Active()||profiling)||t.pending||(!t.measuring&&GetTickCount64()-t.last<(profiling?1000u:3000u)))return;
    // A live sample covers a whole frame: it starts at the frame's first lit draw or not at all.
    if(live&&!t.measuring&&state.litDraws)return;
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
    t.active=int(t.count++);t.post[t.active]=post;t.live[t.active]=live;
    t.program[t.active]=program;t.variant[t.active]=variant;
    t.viewW[t.active]=vp?uint16_t((std::min)(vp->Width,65535.f)):0;t.viewH[t.active]=vp?uint16_t((std::min)(vp->Height,65535.f)):0;
    t.viewMin[t.active]=vp?vp->MinDepth:0;
    ctx->End(t.start[t.active].Get());
}
inline void EndTiming(ID3D11DeviceContext* ctx){auto& t=state.timing;if(t.active>=0){ctx->End(t.end[t.active].Get());t.active=-1;}}
inline void NextTimingFrame(){
    auto& t=state.timing;if(!t.context)return;
    if(t.measuring){t.context->End(t.disjoint.Get());t.measuring=false;t.pending=true;}
    if(!t.pending)return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    if(t.context->GetData(t.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
    double material=0,post=0,live=0;unsigned liveCount=0;
    struct Group{double ms=0;unsigned passes=0;};
    std::map<uint64_t,Group> byView,byProgram;double byVariant[3]{};
    for(unsigned i=0;i<t.count;++i){
        UINT64 start=0,end=0;
        if(t.context->GetData(t.start[i].Get(),&start,sizeof(start),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||
           t.context->GetData(t.end[i].Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
        const double ms=!d.Disjoint&&d.Frequency&&end>=start?double(end-start)*1000/d.Frequency:0;
        (t.post[i]?post:t.live[i]?live:material)+=ms;
        if(t.live[i]){++liveCount;
            const uint64_t view=(uint64_t(t.viewW[i])<<48)|(uint64_t(t.viewH[i])<<32)|uint64_t(t.viewMin[i]*1000);
            auto& v=byView[view];v.ms+=ms;++v.passes;auto& p=byProgram[t.program[i]];p.ms+=ms;++p.passes;
            byVariant[(std::min)(t.variant[i],uint8_t(2))]+=ms;}
    }
    if(!d.Disjoint&&GpuProfile::Enabled()){
        GpuProfile::NoteSampled("lit draws: all patched",live,liveCount);
        // BeginTiming's variant: 0 the native program with owned inputs, 1 dynamic irradiance, 2 another
        // derived program (atmosphere, smoke, coverage).
        static const char* const variants[3]{"lit draws: native program","lit draws: dynamic irradiance","lit draws: atmosphere program"};
        unsigned passes[3]{};for(unsigned i=0;i<t.count;++i)if(t.live[i])++passes[(std::min)(t.variant[i],uint8_t(2))];
        for(unsigned v=0;v<3;++v)if(passes[v])GpuProfile::NoteSampled(variants[v],byVariant[v],passes[v]);
        if(material>0)GpuProfile::NoteSampled("lit draws: replay",material,t.count-liveCount);
        if(post>0)GpuProfile::NoteSampled("lit draws: tonemap replay",post,0);
        for(const auto& [program,group]:byProgram)GpuProfile::NoteSampledProgram(program,group.ms,group.passes);
    }
    if(!d.Disjoint&&MvmDebugLog_Active()){
        MvmDebugLog_LinefAlways("environment.native.material.gpu","sampledReplayMs=%.3f sampledLiveMs=%.3f sampledTonemapMs=%.3f sampledLivePasses=%u sampledPasses=%u eligiblePasses=%u maxSampledPasses=%u nativeMs=%.3f irradianceMs=%.3f atmosphereMs=%.3f",material,live,post,liveCount,t.count,t.eligible,MaterialTiming::Capacity,byVariant[0],byVariant[1],byVariant[2]);
        for(const auto& v:byView)MvmDebugLog_LinefAlways("environment.native.material.gpu.view","viewport=%llux%llu minDepth=%.3f passes=%u ms=%.3f",
            (unsigned long long)(v.first>>48),(unsigned long long)((v.first>>32)&0xffff),double(v.first&0xffffffffu)/1000,v.second.passes,v.second.ms);
        std::vector<std::pair<uint64_t,Group>> programs(byProgram.begin(),byProgram.end());
        std::sort(programs.begin(),programs.end(),[](const auto& a,const auto& b){return a.second.ms>b.second.ms;});
        for(size_t i=0;i<programs.size()&&i<12;++i)MvmDebugLog_LinefAlways("environment.native.material.gpu.program","rank=%zu hash=%016llx passes=%u ms=%.3f",
            i+1,(unsigned long long)programs[i].first,programs[i].second.passes,programs[i].second.ms);
    }
    t.pending=false;
}
inline void Evidence(const char* reason,UINT bytes=0){
    if(!MvmDebugLog_Active())return;
    static std::set<std::string> reasons; if(reasons.size()>64||!reasons.insert(reason).second)return;
    MvmDebugLog_LinefAlways("environment.native.material","reason=%s bufferBytes=%u draws=%u sun=(%.3f %.3f %.3f) radiance=(%.3f %.3f %.3f)",
        reason,bytes,state.draws,state.next.direction[0],state.next.direction[1],state.next.direction[2],state.next.radiance[0],state.next.radiance[1],state.next.radiance[2]);
}

// Frame preparation, defined in EnvironmentNativeMaterial.cpp and called around each draw.
bool Resources(ID3D11Device* device,const D3D11_TEXTURE2D_DESC& source,const D3D11_RENDER_TARGET_VIEW_DESC& view,UINT bufferBytes);
bool EnsureUnbakeResources(ID3D11Device* device,ID3D11DeviceContext* ctx);
bool ProofResources(ID3D11Device* device,ID3D11DeviceContext* ctx,const D3D11_VIEWPORT& viewport);
bool BeginTonemap(ID3D11DeviceContext* ctx,const D3D11_VIEWPORT& vp);
} // namespace NativeDetail
// Moves a program to the front of the derivation queue (EnvironmentNativeMaterialDerive.cpp).
void PrioritizeNativeMaterial(ID3D11PixelShader* shader);
// Asks the derivation workers for a program's scaled-colour variant (ScaledColorShaderTag), once;
// false when the program kept no bytecode for it (ScaledColorCodeTag).
bool QueueScaledColorVariant(ID3D11PixelShader* shader);
} // namespace Filmmaker::Environment
