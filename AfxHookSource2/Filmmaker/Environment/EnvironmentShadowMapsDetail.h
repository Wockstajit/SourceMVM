#pragma once
// The shadow map driver's own state and helpers, shared by its translation units
// (EnvironmentShadowMaps.cpp: sun cascades, cover maps, sky visibility, player mask;
// EnvironmentLampShadows.cpp: the lamp tiers). Everything here is an inline definition, so both
// units see one copy. Nothing outside those two files includes this.
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentShadowMaps.h"
#include "../Debug/GpuPassProfiler.h"
#include "EnvironmentAnimatedCasters.h"
#include "EnvironmentCasterCull.h"
#include "EnvironmentFoliage.h"
#include "EnvironmentFoliageMath.h"
#include "EnvironmentFoliageShaders.h"
#include "EnvironmentCasterMath.h"
#include "EnvironmentCasterAlphaMath.h"
#include "EnvironmentShadowMath.h"
#include "EnvironmentNativeSunCsmMath.h"
#include "EnvironmentSkyVisibilityMath.h"
#include "EnvironmentShadowSpatialMath.h"
#include "EnvironmentShadowPyramid.h"
#include "EnvironmentNativeShadowMath.h"
#include "EnvironmentWeatherMath.h"
#include "EnvironmentRender.h"
#include "../Relight/RelightProbeMath.h"
#include "../Relight/RelightClusterMath.h"
#include "../Relight/RelightLitBoundsMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <vector>
#include <cstring>
#include <cstdio>
#include <array>
#include <string>
namespace Filmmaker::Environment {
namespace ShadowDetail {
using Microsoft::WRL::ComPtr;
// The sun map spans the whole map and the sky map the playable part of it; 4096 keeps a texel near 1.5-2 units.
constexpr UINT LocalResolution = 1024, SunResolution = 4096, MaxShadowLights = 4;
// How many cached static faces the relight tier draws per frame. The whole tier is
// drawn once and then costs nothing, so this only sets how long it takes to converge:
// a full 384-face tier is resident in about half a second of playback.
constexpr unsigned StaticFacesPerFrame = 12;
// Visible cached faces with a moving actor in them, refreshed every frame ahead of the background
// rotation (see ShadowRefreshCandidate::actor). Nearest first; beyond this they rotate like the rest.
// Fewer is faster (at the de_ancient A-site view 16 instead of 48 saved 1.4 ms, 2026-10-18) but
// lets players' lamp shadows lag; only casters inside a lamp's shadow reach compete for them.
inline unsigned g_actorShadowFacesPerFrame = 48;
struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct Constants {
    Matrix local[24], sun;
    Float4 light[16], info;
    Matrix shelter;
    Float4 shelterInfo, shelterDirection;
    Matrix nearSun;
    Float4 nearSunInfo;
    Matrix closeSun;
    Float4 closeSunInfo;
    Matrix detailSun;
    Float4 detailSunInfo;
    Float4 viewmodelShadowInfo; // viewmodel-safe sun/local receiver layers available
    Float4 sunFilter;           // x = sun shadow filter radius, in shadow-map texels
    // The weather cover volume (EnvironmentWeatherCoverMath.h): world corner (w = ready), voxels
    // per world unit, voxel counts; then the wind.
    Float4 coverLow, coverScale, coverSize;
    Float4 coverWind; // xyz toward where the weather comes from, w its lean
};
struct GpuMesh {
    std::shared_ptr<const Mesh> source;
    ComPtr<ID3D11Buffer> vertices;
};
struct GpuCaster {
    ComPtr<ID3D11Buffer> vertices, indices;
    ComPtr<ID3D11ShaderResourceView> alpha;
    ComPtr<ID3D11SamplerState> sampler;
    UINT count=0;
    float cutoff=.5f;
    CasterAnimation animation;
    float padding=0;
    std::vector<ShadowDrawRange> ranges;
};
struct CutoutDraw { Matrix transform; Float4 settings; CasterAnimation animation; };
struct StaticDepth {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> view;
    uint64_t geometry=0, pose=0, receiverPose=0;
    bool valid=false, composed=false;
    bool receiverValid=false, split=false;
};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11VertexShader> cutoutVertex;
    ComPtr<ID3D11PixelShader> cutoutPixel;
    ComPtr<ID3D11InputLayout> cutoutLayout;
    ComPtr<ID3D11Buffer> cutoutDraw;
    ComPtr<ID3D11SamplerState> cutoutSampler;
    std::vector<GpuCaster> casters;
    StaticDepth cached[29]; // Local faces, far sun, sky map, near sun, close sun, detail bridge.
    ComPtr<ID3D11Buffer> frame, draw, worldVertices, worldIndices;
    std::vector<ShadowDrawRange> worldRanges;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster,sunRaster;
    ComPtr<ID3D11Texture2D> local, sun, shelter, nearSun;
    ComPtr<ID3D11Texture2D> viewmodelSun;
    ComPtr<ID3D11ShaderResourceView> viewmodelSunSrv;
    ComPtr<ID3D11DepthStencilView> viewmodelSunDsv;
    ComPtr<ID3D11Texture2D> closeSun;
    ComPtr<ID3D11ShaderResourceView> closeSunSrv;
    ComPtr<ID3D11DepthStencilView> closeSunDsv;
    ComPtr<ID3D11Texture2D> detailSun;
    ComPtr<ID3D11ShaderResourceView> detailSunSrv;
    ComPtr<ID3D11DepthStencilView> detailSunDsv;
    ComPtr<ID3D11ShaderResourceView> localSrv, sunSrv, shelterSrv, nearSunSrv;
    ComPtr<ID3D11DepthStencilView> localDsv[24], sunDsv, shelterDsv, nearSunDsv;
    ComPtr<ID3D11ShaderResourceView> coverSrv;
    // The nav mesh's footprint with room around it (x, y only), worked out once per map.
    const OcclusionMesh* skyWorld=nullptr;Vec3 skyLow{},skyHigh{};
    ComPtr<ID3D11Texture2D> receiverSun,receiverLocal;
    ComPtr<ID3D11ShaderResourceView> receiverSunSrv,receiverLocalSrv;
    ComPtr<ID3D11DepthStencilView> receiverSunDsv[4],receiverLocalDsv[24];
    bool receiverWanted=false;
    uint64_t receiverPose=0;
    std::shared_ptr<const OcclusionMesh> world;
    std::map<const Mesh *, GpuMesh> props;
    uint64_t localKeys[4]{}, sunKey = 0, shelterKey = 0, nearSunKey = 0;
    // Static geometry and props only: what a cached sun tier depends on, independent of lights.
    uint64_t worldKey = 0;
    Constants constants{};
    bool ready = false;
    bool animate = false;
    Vec3 wind{};float windStrength=0;bool windValid=false;
    bool posedLast = false, sceneCastersLast = false;
};
inline State state;
inline State proofState;
inline State skyboxState;
inline ComPtr<ID3D11Texture2D> skyboxSunTexture;
inline ComPtr<ID3D11DepthStencilView> skyboxSunDepth;
inline ComPtr<ID3D11ShaderResourceView> skyboxSunView;
inline Matrix skyboxSunMatrix{};inline bool skyboxSunReady=false;
inline ComPtr<ID3D11DeviceContext> proofContext;
inline ComPtr<ID3D11Texture2D> proofAtlas,proofSun,proofTransfer;
inline ComPtr<ID3D11Texture2D> mapAtlas,mapSunTransfer;inline ComPtr<ID3D11ShaderResourceView> mapAtlasView;
// The world's close sun cascade while the first-person version occupies its atlas tile (see
// PrepareViewmodelSunShadow / RestoreWorldSunTile).
inline ComPtr<ID3D11Texture2D> mapSunWorldBackup;inline bool mapSunViewmodelActive=false;
inline ComPtr<ID3D11Texture2D> mapSunTexture;inline ComPtr<ID3D11DepthStencilView> mapSunDepth;
// The lamp tiles' depth summary behind the lamp filter's exact early-out (EnvironmentShadowPyramid.h).
// Rebuilt for the tiles each atlas update writes, inside that update's command list.
struct LampPyramid {
    ShadowPyramidBuilder builder;bool ready=false,failed=false;ULONGLONG logged=0;
};
inline LampPyramid lampPyramid;
inline bool g_lampSummary=true;
inline void MarkLampTile(unsigned x,unsigned y,unsigned size){lampPyramid.builder.Mark(x,y,size,size);}
// Records the summary update for every lamp tile marked since the last call, behind the copies
// that wrote them. Any failure unbinds the summary for good: the lamp filter then always runs its
// full disk, as it did before the summary existed.
inline void RecordLampPyramid(ID3D11DeviceContext* ctx){
    auto& p=lampPyramid;if(p.failed||!mapAtlasView||!state.device)return;
    std::string error;
    if(!p.builder.Created()){
        if(!p.builder.Create(state.device.Get(),Relight::AtlasWidth,Relight::AtlasHeight,NativeLampAtlasPyramid.originX,NativeLampPyramidLevels,error)){
            p.failed=true;MvmDebugLog_LinefAlways("environment.shadow.pyramid","disabled: %s",error.c_str());return;}
        MvmDebugLog_LinefAlways("environment.shadow.pyramid","created levels=%u origin=%u",NativeLampPyramidLevels,NativeLampAtlasPyramid.originX);
    }
    unsigned cells=0;
    if(!p.builder.Record(ctx,mapAtlasView.Get(),error,&cells)){
        p.failed=true;p.ready=false;MvmDebugLog_LinefAlways("environment.shadow.pyramid","disabled: %s",error.c_str());return;}
    p.ready=true;
    if(cells&&MvmDebugLog_Active()){const auto now=GetTickCount64();if(now-p.logged>=3000){p.logged=now;
        MvmDebugLog_LinefAlways("environment.shadow.pyramid","updated cells=%u",cells);}}
}
// The same summary for the four sun cascade tiles (the atlas's first 4096 columns), behind the
// sun penumbra pass's exact shortcuts (EnvironmentSunVisibilityShaders.h). Rebuilt for the cascades
// each map pass redraws. Not offered while the first-person pass has its own close cascade in the
// atlas: the summary describes the world's.
inline LampPyramid sunPyramid;
inline bool g_sunSummary=true;
inline void MarkSunTile(unsigned x,unsigned y,unsigned size){if(g_sunSummary)sunPyramid.builder.Mark(x,y,size,size);}
inline void RecordSunPyramid(ID3D11DeviceContext* ctx){
    auto& p=sunPyramid;if(p.failed||!mapAtlasView||!state.device)return;
    std::string error;
    if(!p.builder.Created()){
        if(!p.builder.Create(state.device.Get(),Relight::LocalOriginX,Relight::AtlasHeight,0,SunShadowPyramidLevels,error)){
            p.failed=true;MvmDebugLog_LinefAlways("environment.shadow.sunpyramid","disabled: %s",error.c_str());return;}
        MvmDebugLog_LinefAlways("environment.shadow.sunpyramid","created levels=%u width=%u",SunShadowPyramidLevels,Relight::LocalOriginX);
    }
    unsigned cells=0;
    if(!p.builder.Record(ctx,mapAtlasView.Get(),error,&cells)){
        p.failed=true;p.ready=false;MvmDebugLog_LinefAlways("environment.shadow.sunpyramid","disabled: %s",error.c_str());return;}
    p.ready=true;
    if(cells&&MvmDebugLog_Active()){const auto now=GetTickCount64();if(now-p.logged>=3000){p.logged=now;
        MvmDebugLog_LinefAlways("environment.shadow.sunpyramid","updated cells=%u",cells);}}
}
inline Matrix mapCloseMatrix{};inline Vec3 mapCloseCamera{};inline bool mapCloseReady=false;
// The map pass's static world (no posed casters), kept for the stained glass beam shadows.
inline std::vector<Matrix> mapBeamMatrices;
inline Frame mapBeamFrame;inline bool mapBeamFrameReady=false;inline uint64_t mapBeamKey=0;inline unsigned mapBeamCount=0;
// Draw keeps depth clipping for an orthographic projection while this is set: a beam's near
// plane sits just behind its glass, and the glass must not shadow its own beam.
inline bool g_clipOrthographic=false;
// Diagnostic (mirv_relight dumpsun <file>): the four sun cascade tiles and matrices, written raw.
inline std::string g_sunDumpPath;inline Matrix g_sunDumpMatrices[4]{};inline bool g_sunDumpMatricesReady=false;
inline void DumpSunCascades(ID3D11DeviceContext* immediate){
    if(g_sunDumpPath.empty()||!g_sunDumpMatricesReady||!mapAtlas)return;
    const std::string path=g_sunDumpPath;g_sunDumpPath.clear();
    ComPtr<ID3D11Device> device;immediate->GetDevice(&device);
    D3D11_TEXTURE2D_DESC d{};mapAtlas->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s{};s.Width=s.Height=Relight::SunCascadeSize;s.MipLevels=s.ArraySize=s.SampleDesc.Count=1;
    s.Format=d.Format;s.Usage=D3D11_USAGE_STAGING;s.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&s,nullptr,&staging)))return;
    FILE* file=nullptr;if(fopen_s(&file,path.c_str(),"wb")||!file)return;
    const unsigned tiles[]{Relight::SunTile,Relight::SunMiddleTile,Relight::SunDetailTile,Relight::SunCloseTile};
    for(unsigned c=0;c<4;++c){
        unsigned x=0,y=0,size=0;Relight::TileOrigin(tiles[c],x,y,size);
        const D3D11_BOX box{x,y,0,x+size,y+size,1};
        immediate->CopySubresourceRegion(staging.Get(),0,0,0,0,mapAtlas.Get(),0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))break;
        fwrite(g_sunDumpMatrices[c].data(),sizeof(float),16,file);
        for(unsigned row=0;row<size;++row)fwrite(static_cast<const char*>(mapped.pData)+size_t(row)*mapped.RowPitch,4,size,file);
        immediate->Unmap(staging.Get(),0);
    }
    fclose(file);MvmDebugLog_LinefAlways("environment.shadow.dump","sun cascades written to %s",path.c_str());
    if(!mapBeamMatrices.empty()){
        s.Width=s.Height=Relight::StaticSize;staging.Reset();
        if(SUCCEEDED(device->CreateTexture2D(&s,nullptr,&staging))&&
           !fopen_s(&file,(path+".glass").c_str(),"wb")&&file){
            for(unsigned i=0;i<mapBeamMatrices.size();++i){
                unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::GlassBeamTile+i,x,y,size);
                const D3D11_BOX box{x,y,0,x+size,y+size,1};
                immediate->CopySubresourceRegion(staging.Get(),0,0,0,0,mapAtlas.Get(),0,&box);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if(FAILED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))break;
                fwrite(mapBeamMatrices[i].data(),sizeof(float),16,file);
                for(unsigned row=0;row<size;++row)fwrite(static_cast<const char*>(mapped.pData)+size_t(row)*mapped.RowPitch,4,size,file);
                immediate->Unmap(staging.Get(),0);
            }
            fclose(file);MvmDebugLog_LinefAlways("environment.shadow.dump","glass beams=%zu written to %s.glass",mapBeamMatrices.size(),path.c_str());
        }
        s.Width=s.Height=Relight::SunCascadeSize;staging.Reset();
        if(FAILED(device->CreateTexture2D(&s,nullptr,&staging)))return;
    }
    if(skyboxSunReady&&skyboxSunTexture){
        immediate->CopyResource(staging.Get(),skyboxSunTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))){
            const auto skyPath=path+".skybox";
            if(!fopen_s(&file,skyPath.c_str(),"wb")&&file){
                fwrite(skyboxSunMatrix.data(),sizeof(float),16,file);
                for(unsigned row=0;row<Relight::SunCascadeSize;++row)
                    fwrite(static_cast<const char*>(mapped.pData)+size_t(row)*mapped.RowPitch,4,Relight::SunCascadeSize,file);
                fclose(file);
            }
            immediate->Unmap(staging.Get(),0);
        }
    }
}
// The static shadow tier. One small depth target and one transfer texture serve every
// face in turn, because each face is copied into the atlas before the next is drawn.
inline ComPtr<ID3D11Texture2D> mapStaticDepth,mapStaticTransfer;inline ComPtr<ID3D11DepthStencilView> mapStaticDsv;
inline ComPtr<ID3D11Texture2D> mapStaticBase;
// Refresh batches instead give each face its own pair: a batch's copies, draws and atlas copies
// are then independent of one another, not one dependent chain through the single pair above.
constexpr unsigned StaticRefreshBatch=16;
struct StaticScratch {ComPtr<ID3D11Texture2D> depth,transfer;ComPtr<ID3D11DepthStencilView> dsv;};
inline std::array<StaticScratch,StaticRefreshBatch> mapStaticScratch;
inline std::array<Vec3,Relight::ShadowBudgetLimit> mapSlotPositions{};inline std::array<float,Relight::ShadowBudgetLimit> mapSlotRanges{};inline unsigned mapSlotCount=0;
inline bool EnsureStaticScratch(ID3D11Device* device){
    for(auto& scratch:mapStaticScratch){
        if(scratch.dsv&&scratch.transfer)continue;
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::StaticSize;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(FAILED(device->CreateTexture2D(&d,nullptr,scratch.transfer.ReleaseAndGetAddressOf())))return false;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        if(FAILED(device->CreateTexture2D(&d,nullptr,scratch.depth.ReleaseAndGetAddressOf()))||
           FAILED(device->CreateDepthStencilView(scratch.depth.Get(),&ds,scratch.dsv.ReleaseAndGetAddressOf()))){scratch=StaticScratch{};return false;}
    }
    return true;
}
// Runtime sky visibility: whole-map depth from each sky direction, drawn once per map from
// static render geometry (EnvironmentSkyVisibilityMath.h). Never Valve's baked data.
inline ComPtr<ID3D11Texture2D> skyArray,skyDepth;inline ComPtr<ID3D11DepthStencilView> skyDsv;inline ComPtr<ID3D11ShaderResourceView> skyArrayView;
inline Matrix skyMatrices[SkyVisibilityDirections]{};inline uint64_t skyKey=0;inline unsigned skyDone=0;
// Frames since anything asked for the sky visibility maps (SkyVisibilityMaps). They are 60 MB and
// only the lighting without a bounce volume reads them, so they exist while they are being asked
// for and are released SkyVisibilityIdleFrames after the last request.
inline constexpr unsigned SkyVisibilityIdleFrames=600;
inline unsigned skyIdleFrames=SkyVisibilityIdleFrames;
inline std::vector<bool> mapFaceDynamic;
inline std::vector<uint64_t> mapFaceRefresh;
inline uint64_t mapRefreshFrame=0;
inline std::vector<bool> mapFoliageFaces;
struct FoliageDraw {size_t caster;std::vector<size_t> ranges;};
using FoliageDrawList=std::vector<FoliageDraw>;
inline std::vector<FoliageDrawList> mapFoliageDraws;
// Which set of lights the resident faces belong to, and how many of them are drawn. Faces
// survive in the atlas across frames, so this is the only state saying which are valid.
inline uint64_t mapStaticKey=0;inline unsigned mapStaticDone=0;
inline std::vector<Matrix> mapStaticMatrices;
// Last frame's posed caster bounds, sorted bytewise (the moving-caster test of the lamp face refresh).
inline std::vector<CasterBounds> mapPreviousBounds;
inline ComPtr<ID3D11ShaderResourceView> proofAtlasView;
inline ComPtr<ID3D11DepthStencilView> proofSunDepth;
inline uint64_t Hash(uint64_t h, const void *p, size_t size) {
    auto b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
inline bool Buffer(UINT size, UINT flags, ComPtr<ID3D11Buffer> &out, const void *data = nullptr) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    d.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    return SUCCEEDED(state.device->CreateBuffer(&d, data ? &initial : nullptr, &out));
}
inline bool Textures(int kind) {
    bool sun=kind!=0;
    auto &texture = kind==6?state.viewmodelSun:kind==5?state.detailSun:kind==4?state.closeSun:kind==3?state.nearSun:kind==2?state.shelter:sun ? state.sun : state.local;
    if (texture)
        return true;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = sun ? SunResolution : LocalResolution;
    d.MipLevels = 1;
    d.ArraySize = sun ? 1 : 24;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> result;
    if (FAILED(state.device->CreateTexture2D(&d, nullptr, &result)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
    sr.Format = DXGI_FORMAT_R32_FLOAT;
    sr.ViewDimension = sun ? D3D11_SRV_DIMENSION_TEXTURE2D : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    if (sun)
        sr.Texture2D.MipLevels = 1;
    else {
        sr.Texture2DArray.MipLevels = 1;
        sr.Texture2DArray.ArraySize = 24;
    }
    auto &srv = kind==6?state.viewmodelSunSrv:kind==5?state.detailSunSrv:kind==4?state.closeSunSrv:kind==3?state.nearSunSrv:kind==2?state.shelterSrv:sun ? state.sunSrv : state.localSrv;
    if (FAILED(state.device->CreateShaderResourceView(result.Get(), &sr, &srv)))
        return false;
    for (UINT i = 0; i < d.ArraySize; ++i) {
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};
        ds.Format = DXGI_FORMAT_D32_FLOAT;
        ds.ViewDimension = sun ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        if (!sun) {
            ds.Texture2DArray.FirstArraySlice = i;
            ds.Texture2DArray.ArraySize = 1;
        }
        auto &dsv = kind==6?state.viewmodelSunDsv:kind==5?state.detailSunDsv:kind==4?state.closeSunDsv:kind==3?state.nearSunDsv:kind==2?state.shelterDsv:sun ? state.sunDsv : state.localDsv[i];
        if (FAILED(state.device->CreateDepthStencilView(result.Get(), &ds, &dsv)))
            return false;
    }
    texture = result;
    return true;
}
inline bool ReceiverTexture(bool sun,UINT sunLayers=4) {
    auto &texture=sun?state.receiverSun:state.receiverLocal;
    auto &srv=sun?state.receiverSunSrv:state.receiverLocalSrv;
    const UINT layers=sun?sunLayers:24;
    D3D11_TEXTURE2D_DESC previous{};if(texture)texture->GetDesc(&previous);
    if(texture&&srv&&previous.ArraySize==layers)return true;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=sun?SunResolution:LocalResolution;
    td.MipLevels=td.SampleDesc.Count=1;td.ArraySize=layers;
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> result;ComPtr<ID3D11ShaderResourceView> view;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels=1;sd.Texture2DArray.ArraySize=td.ArraySize;
    if(FAILED(state.device->CreateTexture2D(&td,nullptr,&result))||
       FAILED(state.device->CreateShaderResourceView(result.Get(),&sd,&view)))return false;
    ComPtr<ID3D11DepthStencilView> depthViews[24];
    for(UINT i=0;i<td.ArraySize;++i){
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;
        ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        ds.Texture2DArray.FirstArraySlice=i;ds.Texture2DArray.ArraySize=1;
        if(FAILED(state.device->CreateDepthStencilView(result.Get(),&ds,&depthViews[i])))return false;
    }
    for(UINT i=0;i<(sun?4u:24u);++i)(sun?state.receiverSunDsv[i]:state.receiverLocalDsv[i])=depthViews[i];
    // The new texture has no cached receiver content, including when shrinking
    // back to the far-only layer after native relighting takes over cascades.
    for(UINT i=0;i<(sun?4u:24u);++i){
        const UINT index=sun?(i?25+i:24):i;
        state.cached[index].receiverValid=false;
    }
    MvmDebugLog_LinefAlways("environment.shadow.receiver.allocate","sun=%u layers=%u bytes=%llu",unsigned(sun),layers,
        static_cast<unsigned long long>(td.Width)*td.Height*layers*4);
    texture=result;srv=view;return true;
}
// A face drawn into one tile of a larger depth target (the tiled lamp face refresh): its top left
// corner, and the scissored rasterizer state that keeps its triangles inside the tile.
struct DrawTile {UINT x=0,y=0;ID3D11RasterizerState* raster=nullptr;};
inline void Draw(ID3D11DeviceContext *ctx, const Frame &f, const Matrix &vp, ID3D11DepthStencilView *dsv,
          UINT size, bool animatedOnly=false,
          AnimatedCasterSubset actors=AnimatedCasterSubset::All, bool foliage=true,
          const FoliageDrawList* foliageDraws=nullptr, const DrawTile* tile=nullptr) {
    const ShadowFrustum frustum(vp);
    if(!animatedOnly)ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->OMSetDepthStencilState(state.depth.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT viewport{tile?float(tile->x):0.f, tile?float(tile->y):0.f, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    if(tile){const D3D11_RECT scissor{LONG(tile->x),LONG(tile->y),LONG(tile->x+size),LONG(tile->y+size)};ctx->RSSetScissorRects(1,&scissor);}
    // Orthographic (sun) projections pancake casters nearer than their depth interval onto the
    // near plane instead of clipping them; perspective faces keep their authored near plane.
    ctx->RSSetState(tile?tile->raster:!g_clipOrthographic&&vp[12]==0&&vp[13]==0&&vp[14]==0&&vp[15]==1?state.sunRaster.Get():state.raster.Get());
    ctx->IASetInputLayout(state.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(state.vertex.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    auto cb = state.draw.Get();
    ctx->VSSetConstantBuffers(3, 1, &cb);
    ctx->UpdateSubresource(cb, 0, nullptr, vp.data(), 0, 0);
    UINT stride = sizeof(Vec3), offset = 0;
    auto vb = state.worldVertices.Get();
    if(!animatedOnly){
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(state.worldIndices.Get(), DXGI_FORMAT_R32_UINT, 0);
    if(ShadowDrawRunMerging.load(std::memory_order_relaxed))
        DrawShadowRuns(state.worldRanges,[&](size_t i){return frustum.Intersects(state.worldRanges[i].low,state.worldRanges[i].high);},
            [&](uint32_t first,uint32_t count){ctx->DrawIndexed(count,first,0);});
    else for(const auto& range:state.worldRanges)
        if(frustum.Intersects(range.low,range.high))ctx->DrawIndexed(range.count,range.first,0);
    }
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    stride = sizeof(Vertex);
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight() && o.mesh) {
            auto transform = Multiply(vp, ModelMatrix(o.position, o.angles, o.scale));
            ctx->UpdateSubresource(cb, 0, nullptr, transform.data(), 0, 0);
            vb = state.props.at(o.mesh.get()).vertices.Get();
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->Draw(UINT(o.mesh->vertices.size()), 0);
        }
    if(animatedOnly&&(actors==AnimatedCasterSubset::All||actors==AnimatedCasterSubset::Viewmodel||actors==AnimatedCasterSubset::Scene)){
        ctx->UpdateSubresource(cb,0,nullptr,vp.data(),0,0);
        if(actors==AnimatedCasterSubset::Viewmodel){auto anchored=Multiply(vp,ModelMatrix(f.camera,{0,0,0},{1,1,1}));ctx->UpdateSubresource(cb,0,nullptr,anchored.data(),0,0);}
        DrawAnimatedCasters(ctx,actors,actors==AnimatedCasterSubset::Viewmodel?nullptr:&vp);
        if(actors==AnimatedCasterSubset::All||actors==AnimatedCasterSubset::Scene)DrawAnimatedCoverageCasters(ctx,vp);
    }
    // Use real render UVs and alpha, not the opaque collision hull or solid leaf cards.
    if(foliage&&!state.casters.empty()){
        ctx->IASetInputLayout(state.cutoutLayout.Get());
        ctx->VSSetShader(state.cutoutVertex.Get(),nullptr,0);
        ctx->PSSetShader(state.cutoutPixel.Get(),nullptr,0);
        cb=state.cutoutDraw.Get();ctx->VSSetConstantBuffers(3,1,&cb);
        auto sampler=state.cutoutSampler.Get();ctx->PSSetSamplers(0,1,&sampler);ctx->VSSetSamplers(0,1,&sampler);
        stride=sizeof(CasterVertex);
        auto drawCaster=[&](const GpuCaster& caster,const std::vector<size_t>* selected){
            if(caster.animation.Active()!=animatedOnly)return;
            const float padding=caster.padding;
            if(!selected&&std::none_of(caster.ranges.begin(),caster.ranges.end(),[&](const ShadowDrawRange& r){
                return !std::isfinite(padding)||frustum.Intersects(r.low,r.high,padding);}))return;
            CutoutDraw constants{vp,{caster.cutoff,state.animate&&caster.animation.Active()?1.f:0.f,0,0},caster.animation};
            ctx->UpdateSubresource(cb,0,nullptr,&constants,0,0);
            vb=caster.vertices.Get();ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
            ctx->IASetIndexBuffer(caster.indices.Get(),DXGI_FORMAT_R32_UINT,0);
            auto alpha=caster.alpha.Get();ctx->PSSetShaderResources(0,1,&alpha);
            auto alphaSampler=caster.sampler.Get();ctx->PSSetSamplers(0,1,&alphaSampler);
            const auto drawRun=[&](uint32_t first,uint32_t count){ctx->DrawIndexed(count,first,0);};
            if(ShadowDrawRunMerging.load(std::memory_order_relaxed)){
                if(selected)DrawSelectedShadowRuns(caster.ranges,*selected,drawRun);
                else DrawShadowRuns(caster.ranges,[&](size_t i){
                    return !std::isfinite(padding)||frustum.Intersects(caster.ranges[i].low,caster.ranges[i].high,padding);},drawRun);
            }
            else if(selected){for(size_t index:*selected){const auto& range=caster.ranges[index];ctx->DrawIndexed(range.count,range.first,0);}}
            else for(const auto& range:caster.ranges)
                if(!std::isfinite(padding)||frustum.Intersects(range.low,range.high,padding))ctx->DrawIndexed(range.count,range.first,0);
        };
        if(foliageDraws){for(const auto& selected:*foliageDraws)drawCaster(state.casters[selected.caster],&selected.ranges);}
        else for(const auto& caster:state.casters)drawCaster(caster,nullptr);
        ID3D11ShaderResourceView *empty=nullptr;ctx->PSSetShaderResources(0,1,&empty);
    }
    // A tile's target stays bound for the next tile of its batch.
    if(!tile)ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
inline void PrepareFoliageDraws(const Matrix& vp,FoliageDrawList& draws) {
    draws.clear();
    const ShadowFrustum frustum(vp);
    for(size_t c=0;c<state.casters.size();++c){const auto& caster=state.casters[c];if(!caster.animation.Active())continue;
        FoliageDraw draw{c,{}};
        for(size_t i=0;i<caster.ranges.size();++i){const auto& range=caster.ranges[i];
            if(!std::isfinite(caster.padding)||frustum.Intersects(range.low,range.high,caster.padding))draw.ranges.push_back(i);}
        if(!draw.ranges.empty())draws.push_back(std::move(draw));
    }
}
// Cache the rigid depth independently of the native animation clock. A million
// placed-map triangles must not be redrawn every time a leaf moves. Copying the
// cached depth also removes the previous pose (no accumulated ghost shadows).
inline bool DrawCached(ID3D11DeviceContext *ctx,const Frame &f,const Matrix &vp,
                ID3D11DepthStencilView *dsv,UINT size,int index,uint64_t geometry,
                uint64_t pose,bool animated){
    auto &cache=state.cached[index];
    const bool receiver=state.receiverWanted&&index!=25&&
        (index<24?state.constants.viewmodelShadowInfo.y>0:state.constants.viewmodelShadowInfo.x>0);
    if(cache.valid&&cache.geometry==geometry&&cache.composed&&cache.pose==pose&&cache.split==receiver&&
       (!receiver||cache.receiverValid))return false;
    const UINT receiverSlice=index<24?index:index==24?0:index==26?1:index==27?2:3;
    auto environmentDsv=receiver?(index<24?state.receiverLocalDsv[receiverSlice].Get():state.receiverSunDsv[receiverSlice].Get()):dsv;
    // Keep environment and actors separate when first-person receivers need
    // isolation. World lookups take the minimum depth of both layers; the
    // viewmodel reads only environment depth. No full-cascade receiver copy.
    if(animated&&!cache.texture){
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_D32_FLOAT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(FAILED(state.device->CreateTexture2D(&td,nullptr,&cache.texture))||
           FAILED(state.device->CreateDepthStencilView(cache.texture.Get(),&vd,&cache.view))){
            cache.texture.Reset();cache.view.Reset();
            cache.valid=false;
        }
        // The output may already hold rigid depth from before the first player
        // appeared, but a newly allocated cache has never been rendered into.
        cache.valid=false;
    }
    const bool reuseEnvironment=receiver&&cache.valid&&cache.geometry==geometry&&
        cache.receiverValid&&cache.receiverPose==state.receiverPose;
    if(!reuseEnvironment){
        if(!cache.valid||cache.geometry!=geometry||!cache.texture){
            // Refresh retained rigid depth even on static-only frames, and
            // initialize newly allocated caches before any pose reuses them.
            std::optional<GpuProfile::Scope> redrawScope;
            {static const unsigned sunRedraw=GpuProfile::PassId("shadows: static redraw (sun)"),lampRedraw=GpuProfile::PassId("shadows: static redraw (lamp)");
             redrawScope.emplace(ctx,index>=24?sunRedraw:lampRedraw);}
            if(MvmDebugLog_Active()){static unsigned counts[64]{};static ULONGLONG last=0;++counts[unsigned(index)&63];
                const auto now=GetTickCount64();if(now-last>=3000){last=now;char line[512]{};size_t at=0;
                    for(unsigned k=0;k<64&&at<sizeof(line)-16;++k)if(counts[k])at+=sprintf_s(line+at,sizeof(line)-at,"%u:%u ",k,counts[k]);
                    MvmDebugLog_LinefAlways("environment.shadow.redraw","per-slot static redraws in 3 s (slot:count; <24 lamp faces, 24-28 sun) %s invalid=%u",line,unsigned(!cache.valid));
                    std::fill(std::begin(counts),std::end(counts),0u);}}
            Draw(ctx,f,vp,cache.view?cache.view.Get():environmentDsv,size);
            cache.geometry=geometry;cache.valid=true;
        }
        if(cache.texture){
            ComPtr<ID3D11Resource> destination;environmentDsv->GetResource(&destination);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};environmentDsv->GetDesc(&vd);
            const UINT slice=vd.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DARRAY?vd.Texture2DArray.FirstArraySlice:0;
            ctx->CopySubresourceRegion(destination.Get(),slice,0,0,0,cache.texture.Get(),0,nullptr);
        }
        if(receiver)Draw(ctx,f,vp,environmentDsv,size,true,AnimatedCasterSubset::Scene,animated);
        else if(animated)Draw(ctx,f,vp,environmentDsv,size,true,AnimatedCasterSubset::All);
        cache.receiverValid=receiver;cache.receiverPose=state.receiverPose;
    }
    if(receiver){
        ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1,0);
        if(animated)Draw(ctx,f,vp,dsv,size,true,AnimatedCasterSubset::All,false);
    }
    cache.pose=pose;cache.composed=true;cache.split=receiver;
    return true;
}

// Defined with the lamp tiers; EndShadowMaps drops their state.
void ResetLitBounds();
void ResetTileRefresh();
} // namespace ShadowDetail
// Whether the lighting pass draws its shadows from its own state. Defined with the lamp tiers.
bool RelightPrivateShadowState(const std::shared_ptr<const OcclusionMesh>& world);
} // namespace Filmmaker::Environment
