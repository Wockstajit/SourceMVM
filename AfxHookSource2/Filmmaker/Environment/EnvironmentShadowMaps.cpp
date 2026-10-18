#define NOMINMAX
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
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
// The sun and rain-cover maps each span the whole map; 4096 keeps a texel near 1.5-2 units.
constexpr UINT LocalResolution = 1024, SunResolution = 4096, MaxShadowLights = 4;
// Gust shelter maps (PrecipitationGustDirections): half the main shelter resolution is plenty for
// deciding how far rain reaches into an opening, and keeps the four of them at 64 MiB.
constexpr UINT GustResolution = 2048;
constexpr int GustCacheBase = 29; // cached[] slots 29..32
// How many cached static faces the relight tier draws per frame. The whole tier is
// drawn once and then costs nothing, so this only sets how long it takes to converge:
// a full 384-face tier is resident in about half a second of playback.
constexpr unsigned StaticFacesPerFrame = 12;
// Visible cached faces with a moving actor in them, refreshed every frame ahead of the background
// rotation (see ShadowRefreshCandidate::actor). Nearest first; beyond this they rotate like the rest.
// Fewer is faster (at the de_ancient A-site view 16 instead of 48 saved 1.4 ms, 2026-10-18) but
// lets players' lamp shadows lag; only casters inside a lamp's shadow reach compete for them.
unsigned g_actorShadowFacesPerFrame = 48;
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
    Matrix gust[kPrecipitationGusts];
    Float4 gustDirection[kPrecipitationGusts]; // xyz direction toward the sky, w world bias
    Float4 gustInfo;            // x = ready, y = resolution, z = mean weight, w = gust weight
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
    StaticDepth cached[33]; // Local faces, far sun, shelter, near sun, close sun, detail bridge, gusts.
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
    ComPtr<ID3D11Texture2D> gust;
    ComPtr<ID3D11ShaderResourceView> gustSrv;
    ComPtr<ID3D11DepthStencilView> gustDsv[kPrecipitationGusts];
    ComPtr<ID3D11Texture2D> receiverSun,receiverLocal;
    ComPtr<ID3D11ShaderResourceView> receiverSunSrv,receiverLocalSrv;
    ComPtr<ID3D11DepthStencilView> receiverSunDsv[4],receiverLocalDsv[24];
    bool receiverWanted=false;
    uint64_t receiverPose=0;
    std::shared_ptr<const OcclusionMesh> world;
    std::map<const Mesh *, GpuMesh> props;
    uint64_t localKeys[4]{}, sunKey = 0, shelterKey = 0, nearSunKey = 0;
    Constants constants{};
    bool ready = false;
    bool animate = false;
    Vec3 wind{};float windStrength=0;bool windValid=false;
    bool posedLast = false, sceneCastersLast = false;
} state;
State proofState;
State skyboxState;
ComPtr<ID3D11Texture2D> skyboxSunTexture;
ComPtr<ID3D11DepthStencilView> skyboxSunDepth;
ComPtr<ID3D11ShaderResourceView> skyboxSunView;
Matrix skyboxSunMatrix{};bool skyboxSunReady=false;
ComPtr<ID3D11DeviceContext> proofContext;
ComPtr<ID3D11Texture2D> proofAtlas,proofSun,proofTransfer;
ComPtr<ID3D11Texture2D> mapAtlas,mapSunTransfer;ComPtr<ID3D11ShaderResourceView> mapAtlasView;
// The world's close sun cascade while the first-person version occupies its atlas tile (see
// PrepareViewmodelSunShadow / RestoreWorldSunTile).
ComPtr<ID3D11Texture2D> mapSunWorldBackup;bool mapSunViewmodelActive=false;
ComPtr<ID3D11Texture2D> mapSunTexture;ComPtr<ID3D11DepthStencilView> mapSunDepth;
// The lamp tiles' depth summary behind the lamp filter's exact early-out (EnvironmentShadowPyramid.h).
// Rebuilt for the tiles each atlas update writes, inside that update's command list.
struct LampPyramid {
    ShadowPyramidBuilder builder;bool ready=false,failed=false;ULONGLONG logged=0;
} lampPyramid;
bool g_lampSummary=true;
void MarkLampTile(unsigned x,unsigned y,unsigned size){lampPyramid.builder.Mark(x,y,size,size);}
// Records the summary update for every lamp tile marked since the last call, behind the copies
// that wrote them. Any failure unbinds the summary for good: the lamp filter then always runs its
// full disk, as it did before the summary existed.
void RecordLampPyramid(ID3D11DeviceContext* ctx){
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
Matrix mapCloseMatrix{};Vec3 mapCloseCamera{};bool mapCloseReady=false;
// The map pass's static world (no posed casters), kept for the stained glass beam shadows.
std::vector<Matrix> mapBeamMatrices;
Frame mapBeamFrame;bool mapBeamFrameReady=false;uint64_t mapBeamKey=0;unsigned mapBeamCount=0;
// Draw keeps depth clipping for an orthographic projection while this is set: a beam's near
// plane sits just behind its glass, and the glass must not shadow its own beam.
bool g_clipOrthographic=false;
// Diagnostic (mirv_relight dumpsun <file>): the four sun cascade tiles and matrices, written raw.
std::string g_sunDumpPath;Matrix g_sunDumpMatrices[4]{};bool g_sunDumpMatricesReady=false;
void DumpSunCascades(ID3D11DeviceContext* immediate){
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
ComPtr<ID3D11Texture2D> mapStaticDepth,mapStaticTransfer;ComPtr<ID3D11DepthStencilView> mapStaticDsv;
ComPtr<ID3D11Texture2D> mapStaticBase;
// Refresh batches instead give each face its own pair: a batch's copies, draws and atlas copies
// are then independent of one another, not one dependent chain through the single pair above.
constexpr unsigned StaticRefreshBatch=16;
struct StaticScratch {ComPtr<ID3D11Texture2D> depth,transfer;ComPtr<ID3D11DepthStencilView> dsv;};
std::array<StaticScratch,StaticRefreshBatch> mapStaticScratch;
bool EnsureStaticScratch(ID3D11Device* device){
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
ComPtr<ID3D11Texture2D> skyArray,skyDepth;ComPtr<ID3D11DepthStencilView> skyDsv;ComPtr<ID3D11ShaderResourceView> skyArrayView;
Matrix skyMatrices[SkyVisibilityDirections]{};uint64_t skyKey=0;unsigned skyDone=0;
std::vector<bool> mapFaceDynamic;
std::vector<uint64_t> mapFaceRefresh;
uint64_t mapRefreshFrame=0;
std::vector<bool> mapFoliageFaces;
struct FoliageDraw {size_t caster;std::vector<size_t> ranges;};
using FoliageDrawList=std::vector<FoliageDraw>;
std::vector<FoliageDrawList> mapFoliageDraws;
// Which set of lights the resident faces belong to, and how many of them are drawn. Faces
// survive in the atlas across frames, so this is the only state saying which are valid.
uint64_t mapStaticKey=0;unsigned mapStaticDone=0;
std::vector<Matrix> mapStaticMatrices;
ComPtr<ID3D11ShaderResourceView> proofAtlasView;
ComPtr<ID3D11DepthStencilView> proofSunDepth;
uint64_t Hash(uint64_t h, const void *p, size_t size) {
    auto b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
bool Buffer(UINT size, UINT flags, ComPtr<ID3D11Buffer> &out, const void *data = nullptr) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    d.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    return SUCCEEDED(state.device->CreateBuffer(&d, data ? &initial : nullptr, &out));
}
bool Textures(int kind) {
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
bool ReceiverTexture(bool sun,UINT sunLayers=4) {
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
bool GustTextures() {
    if (state.gust && state.gustSrv) return true;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=GustResolution;
    td.MipLevels=td.SampleDesc.Count=1;td.ArraySize=kPrecipitationGusts;
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> result;ComPtr<ID3D11ShaderResourceView> view;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels=1;sd.Texture2DArray.ArraySize=td.ArraySize;
    if(FAILED(state.device->CreateTexture2D(&td,nullptr,&result))||
       FAILED(state.device->CreateShaderResourceView(result.Get(),&sd,&view)))return false;
    ComPtr<ID3D11DepthStencilView> depthViews[kPrecipitationGusts];
    for(UINT i=0;i<td.ArraySize;++i){
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;
        ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        ds.Texture2DArray.FirstArraySlice=i;ds.Texture2DArray.ArraySize=1;
        if(FAILED(state.device->CreateDepthStencilView(result.Get(),&ds,&depthViews[i])))return false;
    }
    for(int i=0;i<kPrecipitationGusts;++i)state.gustDsv[i]=depthViews[i];
    state.gust=result;state.gustSrv=view;return true;
}
void Draw(ID3D11DeviceContext *ctx, const Frame &f, const Matrix &vp, ID3D11DepthStencilView *dsv,
          UINT size, bool animatedOnly=false,
          AnimatedCasterSubset actors=AnimatedCasterSubset::All, bool foliage=true,
          const FoliageDrawList* foliageDraws=nullptr) {
    const ShadowFrustum frustum(vp);
    if(!animatedOnly)ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->OMSetDepthStencilState(state.depth.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    // Orthographic (sun) projections pancake casters nearer than their depth interval onto the
    // near plane instead of clipping them; perspective faces keep their authored near plane.
    ctx->RSSetState(!g_clipOrthographic&&vp[12]==0&&vp[13]==0&&vp[14]==0&&vp[15]==1?state.sunRaster.Get():state.raster.Get());
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
    for(const auto& range:state.worldRanges)
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
            if(selected){for(size_t index:*selected){const auto& range=caster.ranges[index];ctx->DrawIndexed(range.count,range.first,0);}}
            else for(const auto& range:caster.ranges)
                if(!std::isfinite(padding)||frustum.Intersects(range.low,range.high,padding))ctx->DrawIndexed(range.count,range.first,0);
        };
        if(foliageDraws){for(const auto& selected:*foliageDraws)drawCaster(state.casters[selected.caster],&selected.ranges);}
        else for(const auto& caster:state.casters)drawCaster(caster,nullptr);
        ID3D11ShaderResourceView *empty=nullptr;ctx->PSSetShaderResources(0,1,&empty);
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
void PrepareFoliageDraws(const Matrix& vp,FoliageDrawList& draws) {
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
bool DrawCached(ID3D11DeviceContext *ctx,const Frame &f,const Matrix &vp,
                ID3D11DepthStencilView *dsv,UINT size,int index,uint64_t geometry,
                uint64_t pose,bool animated){
    auto &cache=state.cached[index];
    const bool receiver=state.receiverWanted&&index!=25&&index<GustCacheBase&&
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
} // namespace
namespace {
struct PlayerMaskTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilState> forward, reversed;
    UINT width = 0, height = 0;
    bool valid = false;
} playerMask;
} // namespace
void EndShadowMaps() { playerMask = PlayerMaskTarget{};mapBeamMatrices.clear();mapBeamFrame=Frame{};mapBeamFrameReady=false;mapBeamKey=0;mapBeamCount=0;ResetFoliageInputs();EndAnimatedCasters();state = State{};proofState=State{};proofContext.Reset();proofAtlas.Reset();proofAtlasView.Reset();proofSun.Reset();proofSunDepth.Reset();proofTransfer.Reset();mapAtlas.Reset();mapAtlasView.Reset();lampPyramid=LampPyramid{};mapSunTransfer.Reset();mapSunWorldBackup.Reset();mapSunViewmodelActive=false;
    mapSunTexture.Reset();mapSunDepth.Reset();mapStaticDepth.Reset();mapStaticTransfer.Reset();mapStaticDsv.Reset();mapStaticBase.Reset();mapStaticScratch={};
    mapStaticKey=0;mapStaticDone=0;mapStaticMatrices.clear();mapFaceDynamic.clear();mapFaceRefresh.clear();mapRefreshFrame=0;mapFoliageFaces.clear();mapFoliageDraws.clear();
    skyArray.Reset();skyDepth.Reset();skyDsv.Reset();skyArrayView.Reset();skyKey=0;skyDone=0;
    skyboxState=State{};skyboxSunTexture.Reset();skyboxSunDepth.Reset();skyboxSunView.Reset();skyboxSunReady=false; }
void BeginShadowMaps(ID3D11Device *device) {
    EndShadowMaps();
    if (!device)
        return;
    state.device = device;
    static HMODULE compiler = EnvironmentShaderCompiler();
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    const char *shader = "cbuffer Draw:register(b3){row_major float4x4 transform;} float4 Main(float3 "
                         "p:POSITION):SV_Position{return mul(transform,float4(p,1));}";
    ComPtr<ID3DBlob> code, errors;
    if (!compile || FAILED(CompileEnvironmentShader(shader, strlen(shader), "EnvironmentShadowDepth", nullptr, "Main",
                                   "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &code, &errors))) {
        MvmDebugLog_LinefAlways("environment.shadow", "depth shader failed: %s",
                                errors ? static_cast<const char *>(errors->GetBufferPointer())
                                       : "compiler unavailable");
        return;
    }
    D3D11_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                          &state.vertex)) ||
        FAILED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(),
                                         &state.layout)) ||
        !Buffer(sizeof(Constants), D3D11_BIND_CONSTANT_BUFFER, state.frame) ||
        !Buffer(sizeof(Matrix), D3D11_BIND_CONSTANT_BUFFER, state.draw))
        return;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.SlopeScaledDepthBias = 1;
    if (FAILED(device->CreateDepthStencilState(&depth, &state.depth)) ||
        FAILED(device->CreateRasterizerState(&raster, &state.raster)))
        return;
    raster.DepthClipEnable = FALSE;
    // Sun receiver-plane filtering handles slopes without detaching contact shadows.
    raster.SlopeScaledDepthBias = 0;
    if (FAILED(device->CreateRasterizerState(&raster, &state.sunRaster)))
        return;
    const std::string cutout=std::string(FoliageDeformationShader)+R"(
        cbuffer Draw:register(b3){row_major float4x4 transform;float4 settings;float4 sway;float4 flutter;}
        cbuffer NativeClock:register(b5){float4 clockData[19];}
        cbuffer NativeWind:register(b6){float4 windData[19];}
        Texture2D<float> alphaMap:register(t0);SamplerState alphaSampler:register(s0);
        struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;nointerpolation float cutoff:TEXCOORD1;};
        V VS(float3 p:POSITION,float2 uv:TEXCOORD0,float3 color:COLOR0){
            if(settings.y>0)p=FoliagePosition(p,color,sway,flutter,clockData[18].w,windData[17].xyz,windData[18].x);
            V o;o.p=mul(transform,float4(p,1));o.uv=uv;o.cutoff=settings.x;return o;}
        void PS(V i){clip(alphaMap.SampleLevel(alphaSampler,i.uv,0)-i.cutoff);}
    )";
    ComPtr<ID3DBlob> cutoutVS,cutoutPS;
    if(FAILED(CompileEnvironmentShader(cutout.c_str(),cutout.size(),"EnvironmentCutoutShadow",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutVS,&errors))||
       FAILED(CompileEnvironmentShader(cutout.c_str(),cutout.size(),"EnvironmentCutoutShadow",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutPS,&errors))){
        MvmDebugLog_LinefAlways("environment.shadow","cutout shader failed: %s",errors?static_cast<const char*>(errors->GetBufferPointer()):"unknown");
        return;
    }
    D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(device->CreateVertexShader(cutoutVS->GetBufferPointer(),cutoutVS->GetBufferSize(),nullptr,&state.cutoutVertex))||
       FAILED(device->CreatePixelShader(cutoutPS->GetBufferPointer(),cutoutPS->GetBufferSize(),nullptr,&state.cutoutPixel))||
       FAILED(device->CreateInputLayout(elements,3,cutoutVS->GetBufferPointer(),cutoutVS->GetBufferSize(),&state.cutoutLayout))||
       FAILED(device->CreateSamplerState(&sampler,&state.cutoutSampler))||
       !Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw))return;
    state.ready = true;
    BeginAnimatedCasters(device);
    MvmDebugLog_LinefAlways("environment.shadow",
                            "ready=1 localResolution=%u sunResolution=%u maxShadowLights=%u", LocalResolution,
                            SunResolution, MaxShadowLights);
}
void PrepareShadowMaps(ID3D11DeviceContext *ctx, const Frame &f, bool viewmodel, bool requireWorld) {
    auto &c = state.constants;
    c = Constants{};
    state.receiverWanted=viewmodel;
    for (auto &l : c.light)
        l.x = -1;
    c.info = {0, f.scene.climate[CL_shadowBias], float(LocalResolution), float(SunResolution)};
    bool shelter=f.scene.weather.shelter&&f.scene.weather.type>=1&&f.scene.weather.type<=3&&f.scene.weather.count>0;
    c.shelterInfo={shelter?-1.f:0.f,float(SunResolution),1.f,0};
    if (!state.ready)
        return;
    auto upload = [&]() { ctx->UpdateSubresource(state.frame.Get(), 0, nullptr, &c, 0, 0); };
    auto climate = EffectiveClimate(f.scene.climate);
    bool props = false;
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight())
            props = true;
    const bool lighting = f.relight.enabled || f.proof.enabled;
    bool sun = ClimateSamplesSunShadow(climate, lighting, props || ShadowCascadeView());
    // The borrowed map-light and proof passes switch cast shadows off and never ask for the sun.
    if (MvmDebugLog_Active() && climate[CL_contactShadows] > 0) {
        static int lastSun = -1;
        if (lastSun != int(sun)) {
            lastSun = int(sun);
            MvmDebugLog_LinefAlways("environment.shadow",
                "sun=%d enabled=%.0f fog=%.2f shafts=%.0f sky=%.0f clouds=%.0f lighting=%d props=%d",
                int(sun), climate[CL_enabled], climate[CL_fogDensity], climate[CL_lightShafts], climate[CL_sky],
                climate[CL_clouds], int(lighting), int(props));
        }
    }
    int requested = 0;
    for (const auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && o.shadows)
            ++requested;
    if (!f.occlusion || (!requireWorld && !sun && !requested && !shelter)) {
        RequestFoliageInputs(false);
        RequestAnimatedCasters(f.proof.enabled);
        upload();
        return;
    }
    // Retain allocations between camera-mode switches, but never expose stale
    // contents. Every receiver layer is keyed to the same geometry/pose cache.
    // Native relighting owns the near cascades. This pass then samples only its
    // far sun layer for weather/coatings; three unused 4096-square layers cost
    // 192 MiB. Keep all four when this pass actually prepares the near cascades.
    if(viewmodel&&sun&&ReceiverTexture(true,climate[CL_relighting]>0?4u:1u))c.viewmodelShadowInfo.x=1;
    if(viewmodel&&requested&&ReceiverTexture(false))c.viewmodelShadowInfo.y=1;
    if (state.world != f.occlusion) {
        state.world.reset();
        state.worldVertices.Reset();
        state.worldIndices.Reset();
        state.worldRanges.clear();
        state.casters.clear();
        for(auto &cache:state.cached){cache.valid=false;cache.composed=false;}
        std::fill(std::begin(state.localKeys), std::end(state.localKeys), 0);
        state.sunKey = 0;state.shelterKey=0;state.nearSunKey=0;
        RigidShadowGeometry scratch;ShadowSpatialIndices worldIndices;
        const auto rigid=[&]()->std::optional<RigidShadowView>{
            // A whole map's geometry is large. Near the commit limit (8x MSAA at 1440p under
            // RenderDoc, 2026-10-13) an uncaught bad_alloc here closed CS2 outright.
            try{
                const auto source=RigidShadowSource(*f.occlusion,scratch);
                worldIndices=PartitionShadowIndices(source.indices,[&](uint32_t i){return source.positions[i];});
                return source;
            }catch(const std::bad_alloc&){return std::nullopt;}
        }();
        if(!rigid){
            MvmDebugLog_LinefAlways("environment.shadow","map geometry allocation failed: vertices=%zu indices=%zu",
                f.occlusion->positions.size(),f.occlusion->indices.size());
            upload();
            return;
        }
        if (!Buffer(UINT(rigid->positions.size() * sizeof(Vec3)), D3D11_BIND_VERTEX_BUFFER,
                    state.worldVertices, rigid->positions.data()) ||
            !Buffer(UINT(worldIndices.indices.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER,
                    state.worldIndices, worldIndices.indices.data())) {
            MvmDebugLog_LinefAlways("environment.shadow", "map upload failed");
            upload();
            return;
        }
        state.worldRanges=std::move(worldIndices.ranges);
        if(f.occlusion->visual){
            size_t triangles=0;
            CasterAlphaImageIndex alphaIndex;
            std::vector<ComPtr<ID3D11ShaderResourceView>> alphaViews;
            uint64_t alphaBytes=0,uniqueAlphaBytes=0;
            for(const auto &source:f.occlusion->visual->batches){
                if(f.occlusion->visual->renderGeometry&&source.cutoff<=0&&!source.animation.Active())continue;
                GpuCaster caster;caster.count=UINT(source.indices.size());caster.cutoff=source.cutoff;caster.animation=source.animation;
                // Moving foliage needs tighter cells than rigid walls: one broad leaf
                // bounds block can otherwise mark many distant fixture faces dirty.
                auto indices=PartitionShadowIndices(source.indices,[&](uint32_t i){return source.vertices[i].position;},
                                                    source.animation.Active()?128.f:512.f);
                caster.ranges=std::move(indices.ranges);
                D3D11_TEXTURE2D_DESC td{};td.Width=source.width;td.Height=source.height;
                td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8_UNORM;
                td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{source.alpha.data(),source.width,0};
                ComPtr<ID3D11Texture2D> texture;
                D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                sd.AddressU=D3D11_TEXTURE_ADDRESS_MODE(source.addressU+1);
                sd.AddressV=D3D11_TEXTURE_ADDRESS_MODE(source.addressV+1);
                sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
                if(!Buffer(UINT(source.vertices.size()*sizeof(CasterVertex)),D3D11_BIND_VERTEX_BUFFER,caster.vertices,source.vertices.data())||
                   !Buffer(UINT(indices.indices.size()*4),D3D11_BIND_INDEX_BUFFER,caster.indices,indices.indices.data())||
                   FAILED(state.device->CreateSamplerState(&sd,&caster.sampler))){
                    state.casters.clear();upload();return;
                }
                const auto fingerprint=CasterAlphaFingerprint(source.width,source.height,source.alpha);
                const unsigned image=alphaIndex.Intern(source.width,source.height,source.alpha,fingerprint);
                alphaBytes+=source.alpha.size();
                if(image==alphaViews.size()){
                    if(FAILED(state.device->CreateTexture2D(&td,&initial,&texture))||
                       FAILED(state.device->CreateShaderResourceView(texture.Get(),nullptr,&caster.alpha))){
                        state.casters.clear();upload();return;
                    }
                    char name[96];sprintf_s(name,"SourceMVM caster alpha %ux%u %016llx",source.width,source.height,fingerprint);
                    texture->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(std::strlen(name)),name);
                    alphaViews.push_back(caster.alpha);uniqueAlphaBytes+=source.alpha.size();
                }else caster.alpha=alphaViews[image];
                triangles+=source.indices.size()/3;state.casters.push_back(std::move(caster));
            }
            MvmDebugLog_LinefAlways("environment.shadow.alpha.shared","batches=%zu textures=%u bytes=%llu savedBytes=%llu exactTexels=1",
                state.casters.size(),alphaIndex.Size(),uniqueAlphaBytes,alphaBytes-uniqueAlphaBytes);
            size_t chunks=0;for(const auto& caster:state.casters)chunks+=caster.ranges.size();
            MvmDebugLog_LinefAlways("environment.shadow","visualCasters batches=%zu triangles=%zu chunks=%zu collisionChunks=%zu pose=native-inputs alphaTest=1",state.casters.size(),triangles,chunks,state.worldRanges.size());
            MvmDebugLog_LinefAlways("environment.shadow.geometry","renderGeometry=%u rigidTriangles=%zu rigidChunks=%zu physicsHulls=%u",
                unsigned(f.occlusion->visual->renderGeometry),rigid->indices.size()/3,state.worldRanges.size(),unsigned(!f.occlusion->visual->renderGeometry));
        }
        state.world = f.occlusion;
    }
    uint64_t key = 1469598103934665603ull;
    auto ptr = state.world.get();
    key = Hash(key, &ptr, sizeof(ptr));
    bool animated=false;for(const auto &caster:state.casters)animated|=caster.animation.Active();
    if(animated&&ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)
        state.windValid=ReadFoliageWind(ctx,state.wind,state.windStrength);
    RequestFoliageInputs(animated);unsigned long long serial=0;
    state.animate=animated&&BindFoliageInputs(ctx,serial);
    float maximumPadding=0;
    for(auto& caster:state.casters){
        caster.padding=!caster.animation.Active()||!state.animate?0:state.windValid?
            FoliageDisplacementRadius(caster.animation,state.wind,state.windStrength):std::numeric_limits<float>::infinity();
        // Round outwards so slowly drifting native wind reuses conservative face
        // membership instead of re-running the entire map's culling every frame.
        if(std::isfinite(caster.padding)&&caster.padding>0)caster.padding=std::ceil(caster.padding/8)*8;
        maximumPadding=(std::max)(maximumPadding,caster.padding);
    }
    uint64_t pose=Hash(state.animate?serial:0,&state.animate,sizeof(state.animate));
    if(animated){
        static ULONGLONG last=0;auto now=GetTickCount64();
        if(now-last>=1000&&MvmDebugLog_Active()){
            last=now;MvmDebugLog_LinefAlways("environment.foliage","animated=%d nativeGeneration=%llu cameraIndependent=1",int(state.animate),serial);
        }
    }
    Vec3 low = state.world->low, high = state.world->high;
    if(state.world->visual){
        auto a=state.world->visual->low,b=state.world->visual->high;
        low={(std::min)(low.x,a.x),(std::min)(low.y,a.y),(std::min)(low.z,a.z)};
        high={(std::max)(high.x,b.x),(std::max)(high.y,b.y),(std::max)(high.z,b.z)};
        // The native material can move vertices beyond the rest-pose bounds.
        if(animated){const float extent=std::isfinite(maximumPadding)?maximumPadding:256.f;
            low=low-Vec3{extent,extent,extent};high=high+Vec3{extent,extent,extent};}
    }
    RequestAnimatedCasters(true);
    const bool posed=HasAnimatedCasterGeometry();
    // First-person receivers use environment geometry only. Native actor
    // animation must not introduce third-person self shadows on the viewmodel.
    state.receiverPose=pose;
    // Scene assets are environment casters too. Refresh the receiver layer while
    // they are captured, and once more after the last one goes (no frozen prop
    // shadow); a frame with players only keeps reusing the environment layer.
    const bool sceneCasters=HasAnimatedSceneCasters();
    if(sceneCasters||state.sceneCastersLast){const auto generation=AnimatedCasterGeneration();
        state.receiverPose=Hash(state.receiverPose,&generation,sizeof(generation));}
    state.sceneCastersLast=sceneCasters;
    // Also clear the previous pose on the first empty frame: no frozen shadows
    // when an actor disappears or an engine visibility decision stops its draws.
    if(posed||state.posedLast){animated=true;auto generation=AnimatedCasterGeneration();pose=Hash(pose,&generation,sizeof(generation));}
    state.posedLast=posed;
    std::set<const Mesh *> used;
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight() && o.mesh) {
            auto mesh = o.mesh.get();
            used.insert(mesh);
            key = Hash(key, &mesh, sizeof(mesh));
            auto model = ModelMatrix(o.position, o.angles, o.scale);
            key = Hash(key, model.data(), sizeof(model));
            if (!state.props.count(mesh)) {
                GpuMesh gpu;
                gpu.source = o.mesh;
                if (!Buffer(UINT(o.mesh->vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER,
                            gpu.vertices, o.mesh->vertices.data())) {
                    upload();
                    return;
                }
                state.props.emplace(mesh, std::move(gpu));
            }
            for (const auto &v : o.mesh->vertices) {
                auto p = Transform(model, v.position);
                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
            }
        }
    for (auto it = state.props.begin(); it != state.props.end();)
        if (!used.count(it->first))
            it = state.props.erase(it);
        else
            ++it;
    unsigned draws = 0;
    int light = 0, slot = 0;
    for (const auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && light < 16) {
            if (o.shadows && slot < int(MaxShadowLights) && Textures(false)) {
                bool point = o.kind == "point";
                int first = slot * 6, count = point ? 6 : o.kind == "area" ? 4 : 1;
                Vec3 direction, r, u;
                CameraBasis(o.angles, direction, r, u);
                uint64_t hash = Hash(key, &o.position, sizeof(o.position));
                hash = Hash(hash, &direction, sizeof(direction));
                hash = Hash(hash, &o.range, sizeof(o.range));
                hash = Hash(hash, &o.cone, sizeof(o.cone));
                hash = Hash(hash, &o.shadowNearClip, sizeof(o.shadowNearClip));
                hash = Hash(hash, &point, sizeof(point));
                hash=Hash(hash,&o.studio.width,sizeof(float)); hash=Hash(hash,&o.studio.height,sizeof(float));
                hash=Hash(hash,&count,sizeof(count)); hash=Hash(hash,&r,sizeof(r)); hash=Hash(hash,&u,sizeof(u));
                c.light[light] = {float(first), point ? 1.f : o.kind=="area"?2.f:0.f, o.studio.sourceRadius, o.range};
                std::array<Matrix,6> faceMatrices{};std::array<uint64_t,6> faceHashes{};
                for (int face = 0; face < count; ++face) {
                    auto matrix =
                        LocalShadowMatrix(o.kind=="area"?AreaLightSample(o.position,r,u,o.studio.width,o.studio.height,face):o.position, point ? PointShadowDirection(face) : direction,
                                          point ? 90 : o.cone, o.range,o.shadowNearClip);
                    if(requireWorld&&!point){
                        for(const auto& source:f.relight.lights)
                            if(Relight::HasFixtureProjection(source)&&source.position.x==o.position.x&&source.position.y==o.position.y&&source.position.z==o.position.z){
                                matrix=Relight::FixtureProjection(source);break;}
                        hash=Hash(hash,matrix.data(),sizeof(Matrix));
                    }
                    // The map atlas samples these faces with a filter that reaches across the face
                    // edge: render the budgeted cube faces with the same guard band as the static
                    // tier (the exact matrix is restored when the atlas is packed).
                    if (requireWorld) Relight::GuardFace(matrix.data(), Relight::LocalFaceGuard);
                    faceMatrices[face]=matrix;faceHashes[face]=hash;
                }
                // Moving actors are drawn into every face; cull their triangle blocks per face first.
                if(animated&&ShadowCasterCullingWide.load(std::memory_order_relaxed))
                    PrepareAnimatedCasterCulling(ctx,faceMatrices.data(),unsigned(count));
                for (int face = 0; face < count; ++face) {
                    const auto& matrix=faceMatrices[face];const auto faceHash=faceHashes[face];
                    c.local[first + face] = matrix;
                    if (DrawCached(ctx,f,matrix,state.localDsv[first+face].Get(),LocalResolution,first+face,faceHash,pose,animated)) {
                        ++draws;
                    }
                }
                state.localKeys[slot] = hash;
                ++slot;
            }
            ++light;
        }
    // One ladder for every sun cascade, so the tiers stay geometrically spaced against whatever
    // range the user asked for instead of against four compile-time constants.
    const Vec3 mapSpan = high - low;
    const auto cascades = SunCascadeHalfExtents(climate[CL_shadowRange],
                                                std::sqrt(Dot(mapSpan, mapSpan)) * .5f);
    if (sun && Textures(true)) {
        // The outer map is the camera-independent fallback used by fog/GI and
        // receivers beyond the detail cascades. Cropping it around the camera
        // removes their shadows entirely when the camera leaves that region.
        c.sun = SunShadowMatrix(low, high, ClimateSunDirection(climate));
        auto hash = Hash(key, c.sun.data(), sizeof(Matrix));
        if (DrawCached(ctx,f,c.sun,state.sunDsv.Get(),SunResolution,24,hash,pose,animated)) {
            state.sunKey = hash;
            ++draws;
        }
        c.info.x = 1;
        c.sunFilter.x = Relight::NativeFilterTexels;
        if(MvmDebugLog_Active()){
            // The full-map cascade is the one that has to cover everything the finer rings do not,
            // so its texel footprint is the floor on shadow quality at distance. Report it with the
            // bounds it was fitted to, because a visual/skybox union is what makes it coarse.
            static ULONGLONG lastFar=0;const auto now=GetTickCount64();
            if(now-lastFar>3000){lastFar=now;
                const float rowLength=std::sqrt(c.sun[0]*c.sun[0]+c.sun[1]*c.sun[1]+c.sun[2]*c.sun[2]);
                const float texelWorld=rowLength>0?2/(rowLength*float(SunResolution)):0;
                MvmDebugLog_LinefAlways("environment.shadow.far",
                    "span=%.0f resolution=%u texelWorld=%.3f world=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) fitted=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)",
                    rowLength>0?2/rowLength:0,SunResolution,texelWorld,
                    state.world->low.x,state.world->low.y,state.world->low.z,
                    state.world->high.x,state.world->high.y,state.world->high.z,
                    low.x,low.y,low.z,high.x,high.y,high.z);}
        }
        if (climate[CL_relighting] > 0 && Textures(3)) {
            c.nearSun = NearSunShadowMatrix(c.sun, f.camera, cascades.middle, float(SunResolution));
            const auto nearHash = Hash(key, c.nearSun.data(), sizeof(Matrix));
            if (DrawCached(ctx,f,c.nearSun,state.nearSunDsv.Get(),SunResolution,26,nearHash,pose,animated)) {
                state.nearSunKey = nearHash;
                ++draws;
                if(!state.animate&&!posed)MvmDebugLog_Linef("environment.shadow", "nearUpdated=1 resolution=%u span=2048 texelWorld=0.5 camera=(%.2f %.2f %.2f) fullDepthCasters=1",
                                 SunResolution, f.camera.x, f.camera.y, f.camera.z);
            }
            c.nearSunInfo = {1, float(SunResolution), 0, 0};
            if(Textures(5)){
                c.detailSun=NearSunShadowMatrix(c.sun,f.camera,cascades.detail,float(SunResolution));
                const auto detailHash=Hash(key,c.detailSun.data(),sizeof(Matrix));
                if(DrawCached(ctx,f,c.detailSun,state.detailSunDsv.Get(),SunResolution,28,detailHash,pose,animated))++draws;
                c.detailSunInfo={1,float(SunResolution),0,0};
            }
            if(Textures(4)){
                c.closeSun=NearSunShadowMatrix(c.sun,f.camera,cascades.close,float(SunResolution));
                auto closeHash=Hash(key,c.closeSun.data(),sizeof(Matrix));
                if(DrawCached(ctx,f,c.closeSun,state.closeSunDsv.Get(),SunResolution,27,closeHash,pose,animated))++draws;
                c.closeSunInfo={1,float(SunResolution),0,0};
                if(MvmDebugLog_Active()){
                    static ULONGLONG last=0;const auto now=GetTickCount64();
                    if(now-last>3000){last=now;MvmDebugLog_LinefAlways("environment.shadow.detail",
                        "closeSpan=%.0f resolution=%u texelWorld=%.5f bridgeReady=%d bridgeSpan=%.0f bridgeTexel=%.3f middleSpan=%.0f middleTexel=%.3f outerSpan=%.0f outerTexel=%.3f fullDepthCasters=1",
                        2*cascades.close,SunResolution,2*cascades.close/SunResolution,
                        int(c.detailSunInfo.x),2*cascades.detail,2*cascades.detail/SunResolution,
                        2*cascades.middle,2*cascades.middle/SunResolution,
                        2*cascades.outer,cascades.OuterTexelWorld(float(SunResolution)));}
                }
            }
        }
    }
    if(shelter&&Textures(2)){
        const auto &weather=f.scene.weather;auto direction=PrecipitationShelterDirection(weather.type,weather.windX,weather.windY,weather.speed);
        c.shelter=PrecipitationShadowMatrix(low,high,direction);c.shelterDirection={direction.x,direction.y,direction.z,0};
        auto hash=Hash(key,c.shelter.data(),sizeof(Matrix));
        if(DrawCached(ctx,f,c.shelter,state.shelterDsv.Get(),SunResolution,25,hash,pose,animated)){state.shelterKey=hash;
            if(!state.animate&&!posed)MvmDebugLog_LinefAlways("environment.shelter","updated=1 resolution=%u direction=(%.3f %.3f %.3f) mapTriangles=%zu props=%zu cameraIndependent=1",SunResolution,direction.x,direction.y,direction.z,state.world->indices.size()/3,used.size());}
        // Size the depth offset and the soft radius from the real texel footprint: a flat roof seen
        // along a slanted rain direction changes depth within one texel, which drew thin lines.
        float rowLength=std::sqrt(c.shelter[0]*c.shelter[0]+c.shelter[1]*c.shelter[1]+c.shelter[2]*c.shelter[2]);
        float texelWorld=rowLength>0?2/(rowLength*float(SunResolution)):2;
        float slope=std::sqrt((std::max)(0.f,1-direction.z*direction.z))/(std::max)(.2f,std::abs(direction.z));
        c.shelterInfo.z=(std::max)(1.f,texelWorld*(1.5f+slope));
        c.shelterInfo.w=std::clamp(8/texelWorld,1.5f,6.f);
        c.shelterInfo.x=1;
        // Gust directions: the same fixed world bounds seen along each tilted direction. Static
        // like the main map -- redrawn only when geometry, props or the wind change.
        if(!PrecipitationSettles(weather.type)&&GustTextures()){
            const auto gusts=PrecipitationGustDirections(weather.windX,weather.windY,weather.speed);
            for(int g=0;g<kPrecipitationGusts;++g){
                c.gust[g]=PrecipitationShadowMatrix(low,high,gusts[g]);
                const float row=std::sqrt(c.gust[g][0]*c.gust[g][0]+c.gust[g][1]*c.gust[g][1]+c.gust[g][2]*c.gust[g][2]);
                const float texel=row>0?2/(row*float(GustResolution)):2;
                const float gustSlope=std::sqrt((std::max)(0.f,1-gusts[g].z*gusts[g].z))/(std::max)(.2f,std::abs(gusts[g].z));
                c.gustDirection[g]={gusts[g].x,gusts[g].y,gusts[g].z,(std::max)(1.f,texel*(1.5f+gustSlope))};
                DrawCached(ctx,f,c.gust[g],state.gustDsv[g].Get(),GustResolution,GustCacheBase+g,Hash(key,c.gust[g].data(),sizeof(Matrix)),pose,animated);
            }
            c.gustInfo={1,float(GustResolution),kPrecipitationMeanWeight,PrecipitationGustWeight()};
            if(MvmDebugLog_Active()){static ULONGLONG lastGust=0;const auto now=GetTickCount64();
                if(now-lastGust>3000){lastGust=now;MvmDebugLog_Linef("environment.shelter.gusts","ready=1 resolution=%u weights=%.2f/%.2f g0=(%.3f %.3f %.3f) g1=(%.3f %.3f %.3f) g2=(%.3f %.3f %.3f) g3=(%.3f %.3f %.3f)",
                    GustResolution,kPrecipitationMeanWeight,PrecipitationGustWeight(),gusts[0].x,gusts[0].y,gusts[0].z,gusts[1].x,gusts[1].y,gusts[1].z,gusts[2].x,gusts[2].y,gusts[2].z,gusts[3].x,gusts[3].y,gusts[3].z);}}
        }
    }
    if(viewmodel&&c.closeSunInfo.x>0&&AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)&&Textures(6)){
        ctx->ClearDepthStencilView(state.viewmodelSunDsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        Draw(ctx,f,c.closeSun,state.viewmodelSunDsv.Get(),SunResolution,true,AnimatedCasterSubset::Viewmodel,false);
        c.viewmodelShadowInfo.z=1;
    }
    upload();
    if(viewmodel&&MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>3000){last=now;
            MvmDebugLog_LinefAlways("environment.shadow.viewmodel",
                "sun=%u local=%u excludedCharacters=%u excludedWeapons=%u worldCasters=%u viewmodelSelf=%u nativeViewmodelVertices=%u foliagePose=%llu",
                UINT(c.viewmodelShadowInfo.x),UINT(c.viewmodelShadowInfo.y),
                AnimatedCasterVertexCount(AnimatedCasterSubset::Characters),
                AnimatedCasterVertexCount(AnimatedCasterSubset::Weapons),
                AnimatedCasterVertexCount(),UINT(c.viewmodelShadowInfo.z),AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel),serial);}}
    if (draws&&!state.animate&&!posed)
        MvmDebugLog_LinefAlways("environment.shadow",
                                "updatedFaces=%u localLights=%d requested=%d sun=%d mapTriangles=%zu "
                                "props=%zu cameraIndependent=1",
                                draws, slot, requested, int(c.info.x), state.world->indices.size() / 3,
                                used.size());
}
bool PreparePlayerMask(ID3D11DeviceContext *ctx, const Matrix &vp, UINT width, UINT height, bool reversed) {
    playerMask.valid = false;
    if (!ctx || !state.ready || !state.device || !width || !height)
        return false;
    RequestAnimatedCasters(true);
    const UINT actors = AnimatedCasterVertexCount(AnimatedCasterSubset::Actors);
    if (MvmDebugLog_Active()) {
        static ULONGLONG last = 0;
        const auto now = GetTickCount64();
        if (now - last > 2000) {
            last = now;
            MvmDebugLog_LinefAlways("environment.playermask", "characters=%u weapons=%u size=%ux%u reversed=%d",
                                    AnimatedCasterVertexCount(AnimatedCasterSubset::Characters),
                                    AnimatedCasterVertexCount(AnimatedCasterSubset::Weapons), width, height, int(reversed));
        }
    }
    if (!actors)
        return false;
    if (!playerMask.texture || playerMask.width != width || playerMask.height != height) {
        playerMask.texture.Reset(); playerMask.dsv.Reset(); playerMask.srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(state.device->CreateTexture2D(&td, nullptr, &playerMask.texture)) ||
            FAILED(state.device->CreateDepthStencilView(playerMask.texture.Get(), &dv, &playerMask.dsv)) ||
            FAILED(state.device->CreateShaderResourceView(playerMask.texture.Get(), &sv, &playerMask.srv))) {
            playerMask.texture.Reset(); playerMask.dsv.Reset(); playerMask.srv.Reset();
            MvmDebugLog_LinefAlways("environment.playermask", "target allocation failed %ux%u", width, height);
            return false;
        }
        playerMask.width = width; playerMask.height = height;
    }
    if (!playerMask.forward || !playerMask.reversed) {
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS;
        D3D11_DEPTH_STENCIL_DESC dr = ds; dr.DepthFunc = D3D11_COMPARISON_GREATER;
        if (FAILED(state.device->CreateDepthStencilState(&ds, &playerMask.forward)) ||
            FAILED(state.device->CreateDepthStencilState(&dr, &playerMask.reversed)))
            return false;
    }
    ctx->ClearDepthStencilView(playerMask.dsv.Get(), D3D11_CLEAR_DEPTH, reversed ? 0.f : 1.f, 0);
    auto dsv = playerMask.dsv.Get();
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->OMSetDepthStencilState((reversed ? playerMask.reversed : playerMask.forward).Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(state.raster.Get());
    ctx->IASetInputLayout(state.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(state.vertex.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    auto cb = state.draw.Get();
    ctx->VSSetConstantBuffers(3, 1, &cb);
    ctx->UpdateSubresource(cb, 0, nullptr, vp.data(), 0, 0);
    DrawAnimatedCasters(ctx, AnimatedCasterSubset::Actors);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    playerMask.valid = true;
    return true;
}
void BindPlayerMask(ID3D11DeviceContext *ctx) {
    auto srv = playerMask.valid ? playerMask.srv.Get() : nullptr;
    ctx->PSSetShaderResources(31, 1, &srv);
}
// Share immutable shader/mesh resources only; targets, constants and caches have
// independent ownership and never consume a user's light or shadow slot.
bool RelightPrivateShadowState(const std::shared_ptr<const OcclusionMesh>& world){
    // The ordinary environment pass can skip geometry entirely at night or with
    // zero sunlight. Its world pointer is then empty or from a previous map.
    // Residency belongs to the requested map, not that pass's activity.
    if(proofState.ready&&proofState.world==world)return true;
    proofState.device=state.device;proofState.vertex=state.vertex;proofState.layout=state.layout;
    proofState.cutoutVertex=state.cutoutVertex;proofState.cutoutPixel=state.cutoutPixel;
    proofState.cutoutLayout=state.cutoutLayout;proofState.cutoutSampler=state.cutoutSampler;
    // Both rasterizer states: without sunRaster the sun cascades ran on D3D's default state, which
    // culls back faces and clips depth, so single-sided walls and roofs seen from behind by the sun
    // dropped out of the map pass and sunlight came through them.
    proofState.depth=state.depth;proofState.raster=state.raster;proofState.sunRaster=state.sunRaster;
    if(state.world==world){
        proofState.world=state.world;proofState.worldVertices=state.worldVertices;
        proofState.worldIndices=state.worldIndices;proofState.worldRanges=state.worldRanges;proofState.casters=state.casters;
    }else{
        // PrepareShadowMaps uploads this map once into the private state.
        proofState.world.reset();proofState.worldVertices.Reset();proofState.worldIndices.Reset();
        proofState.worldRanges.clear();proofState.casters.clear();
    }
    for(auto& cache:proofState.cached){cache.valid=false;cache.composed=false;}
    mapStaticKey=0;mapStaticDone=0;mapStaticMatrices.clear();mapFaceDynamic.clear();mapFaceRefresh.clear();mapRefreshFrame=0;mapFoliageFaces.clear();mapFoliageDraws.clear();
    skyKey=0;skyDone=0;
    return proofContext||SUCCEEDED(state.device->CreateDeferredContext(0,&proofContext));
}
unsigned PrepareMapLightShadows(ID3D11DeviceContext* immediate,const Frame& input,const Vec3* positions,
                                const float* ranges,unsigned lights,const Vec3* staticPositions,
                                const float* staticRanges,unsigned staticLights,Vec3 sunDirection,
                                Matrix* matrices,unsigned* staticReady,ID3D11ShaderResourceView** atlas) {
    if(staticReady)*staticReady=0;
    if(immediate)DumpSunCascades(immediate);
    skyboxSunReady=false;
    if(!immediate||!matrices||!atlas||!state.ready||!input.occlusion)return 0;
    if(lights&&(!positions||!ranges))return 0;
    if(staticLights&&(!staticPositions||!staticRanges))staticLights=0;
    lights=(std::min)(lights,Relight::ShadowBudgetLimit);
    staticLights=(std::min)(staticLights,Relight::StaticShadowLimit);
    if(!RelightPrivateShadowState(input.occlusion))return 0;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    state.windValid=ReadFoliageWind(immediate,state.wind,state.windStrength);
    if(!state.ready){
        if(!Buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,state.frame)||
           !Buffer(sizeof(Matrix),D3D11_BIND_CONSTANT_BUFFER,state.draw)||
           !Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw))return 0;
        state.ready=true;
    }
    if(!mapAtlasView){
        D3D11_TEXTURE2D_DESC d{};d.Width=Relight::AtlasWidth;
        d.Height=Relight::AtlasHeight;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;
        sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels=1;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&mapAtlas))||
           FAILED(state.device->CreateShaderResourceView(mapAtlas.Get(),&sr,&mapAtlasView)))return 0;
    }
    if(!mapStaticDsv||!mapStaticTransfer){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::StaticSize;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(!mapStaticTransfer&&FAILED(state.device->CreateTexture2D(&d,nullptr,&mapStaticTransfer)))return 0;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(!mapStaticDsv&&(FAILED(state.device->CreateTexture2D(&d,nullptr,&mapStaticDepth))||
           FAILED(state.device->CreateDepthStencilView(mapStaticDepth.Get(),&ds,&mapStaticDsv))))return 0;
    }
    if(!mapStaticBase){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::StaticSize;d.MipLevels=d.SampleDesc.Count=1;
        d.ArraySize=Relight::StaticShadowLimit*Relight::ShadowFaces;d.Format=DXGI_FORMAT_R32_TYPELESS;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&mapStaticBase)))return 0;
    }
    if(!proofTransfer||!mapSunDepth||!mapSunTransfer){
        // D3D11 only allows whole-subresource copies out of a depth resource, so depth
        // lands in a same-size plain texture before being packed into the atlas. The sun
        // and a cube face are different sizes, so each needs its own.
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::LocalSize;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(!proofTransfer&&FAILED(state.device->CreateTexture2D(&d,nullptr,&proofTransfer)))return 0;
        d.Width=d.Height=Relight::SunCascadeSize;
        if(!mapSunTransfer&&FAILED(state.device->CreateTexture2D(&d,nullptr,&mapSunTransfer)))return 0;
        if(!mapSunWorldBackup&&FAILED(state.device->CreateTexture2D(&d,nullptr,&mapSunWorldBackup)))return 0;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(!mapSunDepth&&(FAILED(state.device->CreateTexture2D(&d,nullptr,&mapSunTexture))||
           FAILED(state.device->CreateDepthStencilView(mapSunTexture.Get(),&ds,&mapSunDepth))))return 0;
    }
    // Borrow the ordinary point-light path: it already caches static depth per face and
    // layers animated casters over it, which is what keeps this affordable once the
    // map's lights have each been drawn once.
    Frame frame=input;frame.proof.enabled=false;frame.relight.enabled=false;frame.scene.weather.type=0;
    frame.scene.climate[CL_contactShadows]=0;frame.scene.climate[CL_sunPower]=0;
    frame.scene.objects.erase(std::remove_if(frame.scene.objects.begin(),frame.scene.objects.end(),
        [](const Object& o){return o.IsLight();}),frame.scene.objects.end());
    const auto sourceNearClip=[&](Vec3 position){
        for(const auto& light:input.relight.lights)
            if(light.position.x==position.x&&light.position.y==position.y&&light.position.z==position.z)
                return Relight::AuthoredShadowNearClip(light);
        return 1.f;
    };
    const auto projectedSource=[&](Vec3 position)->const Relight::Light*{
        for(const auto& light:input.relight.lights)
            if(Relight::HasFixtureProjection(light)&&light.position.x==position.x&&light.position.y==position.y&&light.position.z==position.z)return &light;
        return nullptr;
    };
    for(unsigned i=0;i<lights;++i){
        Object lamp;lamp.kind="point";lamp.position=positions[i];
        if(const auto* source=projectedSource(positions[i])){lamp.kind="spot";lamp.angles=source->angles;}
        lamp.shadowNearClip=sourceNearClip(positions[i]);
        lamp.range=(std::max)(1.f,ranges[i]);lamp.shadows=true;frame.scene.objects.push_back(lamp);
    }
    std::vector<CasterBounds> posedBounds;
    const auto boundsStarted=GetTickCount64();
    // Preview cannot stall the CPU until all preceding GPU draws finish merely
    // to exclude a few faces. Unavailable current bounds retain every caster;
    // the refresh budget still bounds shadow work. Exports use exact bounds.
    bool posedBoundsComplete=false;
    {GPU_PASS_SCOPE(immediate,"casters: bounds readback");
     posedBoundsComplete=ReadAnimatedCasterBounds(immediate,posedBounds,input.exporting);}
    const auto boundsMilliseconds=GetTickCount64()-boundsStarted;
    auto ctx=proofContext.Get();ctx->ClearState();
    {GPU_PASS_SCOPE(ctx,"shadows: near lamps");PrepareShadowMaps(ctx,frame,false,true);}
    if(state.world!=input.occlusion||(lights&&!state.local)){
        ctx->ClearState();ComPtr<ID3D11CommandList> discarded;ctx->FinishCommandList(FALSE,&discarded);return 0;}
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    unsigned rendered=0;
    {GPU_PASS_SCOPE(ctx,"shadows: near lamp copies");
    for(unsigned light=0;light<lights;++light){
        // A light that won no shadow slot reports -1 here. Leave it shadowless rather
        // than sampling a tile nothing was ever drawn into.
        if(state.constants.light[light].x<0)break;
        const UINT first=UINT(state.constants.light[light].x);
        const unsigned faces=projectedSource(positions[light])?1u:Relight::ShadowFaces;
        for(UINT face=0;face<faces;++face){
            const unsigned tile=light*6+face;
            if(tile>=Relight::LocalColumns*Relight::LocalRows)break;
            matrices[tile]=state.constants.local[first+face];
            Relight::GuardFace(matrices[tile].data(),1/Relight::LocalFaceGuard);
            unsigned tileX=0,tileY=0,tileSize=0;Relight::TileOrigin(tile,tileX,tileY,tileSize);
            ctx->CopySubresourceRegion(proofTransfer.Get(),0,0,0,0,state.local.Get(),first+face,nullptr);
            ctx->CopySubresourceRegion(mapAtlas.Get(),0,tileX,tileY,0,proofTransfer.Get(),0,nullptr);
            MarkLampTile(tileX,tileY,tileSize);
        }
        ++rendered;
    }}
    // The sun keeps a tile of its own so adding or dropping a local light never
    // moves it. Its frustum covers the whole map, which is what lets sunlight
    // reach through a doorway and stop at the walls around it.
    auto low=input.occlusion->low,high=input.occlusion->high;
    if(input.occlusion->visual){const auto& v=*input.occlusion->visual;
        low={(std::min)(low.x,v.low.x),(std::min)(low.y,v.low.y),(std::min)(low.z,v.low.z)};
        high={(std::max)(high.x,v.high.x),(std::max)(high.y,v.high.y),(std::max)(high.z,v.high.z)};}
    matrices[Relight::SunTile]=SunShadowMatrix(low,high,sunDirection);
    const auto crops=NativeSunCropExtents(input.scene.climate[CL_shadowRange],
        std::sqrt(Dot(high-low,high-low))*.5f);
    const auto forward=ViewForward(input.angles);const float cascadeSize=float(Relight::SunCascadeSize);
    matrices[Relight::SunMiddleTile]=NativeSunCropMatrix(matrices[Relight::SunTile],input.camera,forward,crops.middle,cascadeSize,low.z,high.z);
    matrices[Relight::SunDetailTile]=NativeSunCropMatrix(matrices[Relight::SunTile],input.camera,forward,crops.detail,cascadeSize,low.z,high.z);
    matrices[Relight::SunCloseTile]=NativeSunCropMatrix(matrices[Relight::SunTile],input.camera,forward,crops.close,cascadeSize,low.z,high.z);
    if(input.relight.sunShadows&&Relight::HasRadiance(Relight::SunRadiance(input.relight))){
        GPU_PASS_SCOPE(ctx,"shadows: sun cascades");
        const bool animated=state.animate||HasAnimatedCasterGeometry();
        const unsigned tiles[]{Relight::SunTile,Relight::SunMiddleTile,Relight::SunDetailTile,Relight::SunCloseTile};
        for(unsigned cascade=0;cascade<4;++cascade)g_sunDumpMatrices[cascade]=matrices[tiles[cascade]];g_sunDumpMatricesReady=true;
        if(animated&&ShadowCasterCullingWide.load(std::memory_order_relaxed)){
            std::array<Matrix,4> cascadeMatrices{};for(unsigned cascade=0;cascade<4;++cascade)cascadeMatrices[cascade]=matrices[tiles[cascade]];
            PrepareAnimatedCasterCulling(ctx,cascadeMatrices.data(),4);
        }
        for(unsigned cascade=0;cascade<4;++cascade){const auto tile=tiles[cascade];
            const auto hash=Hash(state.localKeys[0],matrices[tile].data(),sizeof(Matrix));
            DrawCached(ctx,frame,matrices[tile],mapSunDepth.Get(),Relight::SunCascadeSize,24+cascade,hash,
                       AnimatedCasterGeneration(),animated);
            ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(mapSunTransfer.Get(),mapSunTexture.Get());
            unsigned x=0,y=0,size=0;Relight::TileOrigin(tile,x,y,size);
            ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunTransfer.Get(),0,nullptr);
        }
        // The first-person pass redraws the close cascade for itself (PrepareViewmodelSunShadow).
        mapCloseMatrix=matrices[Relight::SunCloseTile];mapCloseCamera=input.camera;mapCloseReady=true;
    }else mapCloseReady=false;
    // The static tier. A map light never moves and map geometry never changes, so a face
    // here is drawn once from static world geometry and then lives in the atlas for the
    // rest of the session; only the set of lights or a different map invalidates it.
    // Drawing several hundred of them in one frame would hitch, so a fixed number are done
    // per frame and a light reports as shadowed only once all six of its faces are in.
    {
        uint64_t key=Hash(0,&input.occlusion,sizeof(const void*));
        key=Hash(key,&staticLights,sizeof(staticLights));
        if(staticLights){
            key=Hash(key,staticPositions,staticLights*sizeof(Vec3));
            key=Hash(key,staticRanges,staticLights*sizeof(float));
            for(unsigned i=0;i<staticLights;++i){const float nearClip=sourceNearClip(staticPositions[i]);key=Hash(key,&nearClip,sizeof(nearClip));}
            for(unsigned i=0;i<staticLights;++i)if(const auto* source=projectedSource(staticPositions[i])){
                const auto projection=Relight::FixtureProjection(*source);key=Hash(key,projection.data(),sizeof(Matrix));}
        }
        if(key!=mapStaticKey){mapStaticKey=key;mapStaticDone=0;
            mapStaticMatrices.assign(size_t(staticLights)*Relight::ShadowFaces,Matrix{});
            mapFaceDynamic.assign(size_t(staticLights)*Relight::ShadowFaces,false);
            mapFaceRefresh.assign(size_t(staticLights)*Relight::ShadowFaces,0);mapRefreshFrame=0;
            mapFoliageFaces.assign(size_t(staticLights)*Relight::ShadowFaces,false);
            mapFoliageDraws.assign(size_t(staticLights)*Relight::ShadowFaces,FoliageDrawList{});}
        // Wind bounds are independent of animation time. Re-evaluate resident
        // face membership only when a material's proven displacement changes.
        uint64_t extentKey=1469598103934665603ull;
        for(const auto& caster:state.casters)extentKey=Hash(extentKey,&caster.padding,sizeof(caster.padding));
        static uint64_t lastExtentKey=0;
        const auto cullStarted=GetTickCount64();
        if(extentKey!=lastExtentKey){lastExtentKey=extentKey;
            for(unsigned i=0;i<mapStaticDone;++i){PrepareFoliageDraws(mapStaticMatrices[i],mapFoliageDraws[i]);mapFoliageFaces[i]=!mapFoliageDraws[i].empty();}}
        const auto cullMs=GetTickCount64()-cullStarted;
        const unsigned wanted=staticLights*Relight::ShadowFaces;
        Frame rigid=frame;rigid.scene.objects.clear();
        mapBeamFrame=rigid;mapBeamFrameReady=true;
        if(mapStaticDone<wanted){GPU_PASS_SCOPE(ctx,"shadows: static faces (first draw)");
        for(unsigned drawn=0;mapStaticDone<wanted&&drawn<StaticFacesPerFrame;++drawn,++mapStaticDone){
            const unsigned light=mapStaticDone/Relight::ShadowFaces,face=mapStaticDone%Relight::ShadowFaces;
            const auto* source=projectedSource(staticPositions[light]);
            if(source&&face)continue;
            const auto matrix=source?Relight::FixtureProjection(*source):LocalShadowMatrix(staticPositions[light],PointShadowDirection(face),90,
                                                (std::max)(1.f,staticRanges[light]),sourceNearClip(staticPositions[light]));
            // Rendered with the guard band; records get the exact face (Relight::TileRect).
            auto guarded=matrix;Relight::GuardStaticFace(guarded.data());
            mapStaticMatrices[mapStaticDone]=guarded;
            PrepareFoliageDraws(guarded,mapFoliageDraws[mapStaticDone]);
            mapFoliageFaces[mapStaticDone]=!mapFoliageDraws[mapStaticDone].empty();
            Draw(ctx,rigid,guarded,mapStaticDsv.Get(),Relight::StaticSize);
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            ctx->CopySubresourceRegion(mapStaticBase.Get(),mapStaticDone,0,0,0,mapStaticDepth.Get(),0,nullptr);
            ctx->CopyResource(mapStaticTransfer.Get(),mapStaticDepth.Get());
            unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::StaticTile+mapStaticDone,x,y,size);
            ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapStaticTransfer.Get(),0,nullptr);
            MarkLampTile(x,y,size);
        }}
        // Every resident fixture receives the current posed geometry and editable
        // objects. Copying rigid depth first removes the previous pose completely.
        // These depths are generated in this session; no map-baked shadows are read.
        unsigned updatedFaces=0;const auto dynamicStarted=GetTickCount64();
        const bool props=std::any_of(frame.scene.objects.begin(),frame.scene.objects.end(),
            [](const Object& o){return o.enabled&&!o.IsLight()&&o.mesh;});
        ++mapRefreshFrame;
        std::vector<ShadowRefreshCandidate> refresh;refresh.reserve(mapStaticDone);
        std::vector<bool> faceDynamic(mapStaticDone);
        std::vector<unsigned> realtimeFaces,actorFaceList;unsigned actorFaces=0;
        const ShadowFrustum receivers(input.worldToClip);
        // Visible reach (RelightClusterMath.h, the same record the light clusters cull with): past
        // it a light cannot change the picture, so a face earns on-screen priority and an
        // every-frame refresh only while that reach, not the authored range, meets the view.
        // With a shadow reach, receivers past it never read the face at all: it bounds instead.
        // staticShadowReach: the half size of the cube a moving caster must reach into (0 = any).
        std::vector<float> staticReach(staticLights),staticShadowReach(staticLights);
        for(unsigned l=0;l<staticLights;++l){staticReach[l]=(std::max)(1.f,staticRanges[l]);
            for(const auto& light:input.relight.lights)
                if(light.position.x==staticPositions[l].x&&light.position.y==staticPositions[l].y&&light.position.z==staticPositions[l].z){
                    const bool reach=input.relight.shadowRadiance>0&&!Relight::HasFixtureProjection(light);
                    staticReach[l]=Relight::LightVisibleReach(Relight::ToRecord(light,input.relight.intensity),staticReach[l],
                        reach?input.relight.shadowRadiance:input.relight.visibleRadiance);
                    if(reach)staticShadowReach[l]=staticReach[l];break;}}
        for(unsigned i=0;i<mapStaticDone;++i){
            if(i%Relight::ShadowFaces&&projectedSource(staticPositions[i/Relight::ShadowFaces]))continue;
            const unsigned owner=i/Relight::ShadowFaces;
            const bool actor=CasterBoundsIntersect(posedBounds,mapStaticMatrices[i],posedBoundsComplete,staticPositions[owner],
                staticShadowReach[owner]);
            const bool dynamic=props||mapFoliageFaces[i]||actor;
            faceDynamic[i]=dynamic;if(!dynamic&&!mapFaceDynamic[i])continue;
            const unsigned light=i/Relight::ShadowFaces;
            const Vec3 p=staticPositions[light];const float range=staticReach[light];
            const Vec3 extent{range,range,range};const float distance=std::sqrt(Dot(p-input.camera,p-input.camera));
            const bool visible=!input.hasWorldToClip||receivers.Intersects(p-extent,p+extent);
            // Preserve per-frame animation across doorways. The extra 128 units
            // covers receivers visible just beyond the fixture's influence.
            const bool realtime=visible&&distance<=range+128;
            if(realtime)realtimeFaces.push_back(i);
            // Unknown bounds intersect everything; only a measured actor earns the priority tier.
            const bool knownActor=actor&&posedBoundsComplete;
            if(knownActor&&visible){++actorFaces;actorFaceList.push_back(i);}
            refresh.push_back({i,mapFaceRefresh[i],1/(1+distance/range),visible,!dynamic,realtime,knownActor});
        }
        // Movie frames must contain current casters in every resident face.
        const unsigned refreshBudget=input.exporting?mapStaticDone:32;
        const auto selected=SelectShadowRefresh(std::move(refresh),mapRefreshFrame,refreshBudget,16,
            input.exporting?0u:g_actorShadowFacesPerFrame);
        if(!selected.empty()){GPU_PASS_SCOPE(ctx,"shadows: lamp face refresh");
        constexpr unsigned batchSize=StaticRefreshBatch;
        const bool batched=ShadowFaceBatching.load(std::memory_order_relaxed)&&EnsureStaticScratch(state.device.Get());
        for(size_t batch=0;batch<selected.size();batch+=batchSize){
        const auto count=unsigned((std::min)(size_t(batchSize),selected.size()-batch));
        std::array<Matrix,batchSize> cullMatrices{};
        for(unsigned j=0;j<count;++j)cullMatrices[j]=mapStaticMatrices[selected[batch+j]];
        PrepareAnimatedCasterCulling(ctx,cullMatrices.data(),count);
        if(batched){
            // The same copies and draws as below, issued phase by phase across the batch.
            for(unsigned j=0;j<count;++j)
                ctx->CopySubresourceRegion(mapStaticScratch[j].depth.Get(),0,0,0,0,mapStaticBase.Get(),selected[batch+j],nullptr);
            for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];
                if(faceDynamic[i])Draw(ctx,frame,mapStaticMatrices[i],mapStaticScratch[j].dsv.Get(),Relight::StaticSize,true,AnimatedCasterSubset::All,true,&mapFoliageDraws[i]);}
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            for(unsigned j=0;j<count;++j)ctx->CopyResource(mapStaticScratch[j].transfer.Get(),mapStaticScratch[j].depth.Get());
            for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];
                unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::StaticTile+i,x,y,size);
                ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapStaticScratch[j].transfer.Get(),0,nullptr);
                MarkLampTile(x,y,size);
                mapFaceDynamic[i]=faceDynamic[i];mapFaceRefresh[i]=mapRefreshFrame;++updatedFaces;
            }
            continue;
        }
        for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];
            const bool dynamic=faceDynamic[i];
            ctx->CopySubresourceRegion(mapStaticDepth.Get(),0,0,0,0,mapStaticBase.Get(),i,nullptr);
            if(dynamic)Draw(ctx,frame,mapStaticMatrices[i],mapStaticDsv.Get(),Relight::StaticSize,true,AnimatedCasterSubset::All,true,&mapFoliageDraws[i]);
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            ctx->CopyResource(mapStaticTransfer.Get(),mapStaticDepth.Get());
            unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::StaticTile+i,x,y,size);
            ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapStaticTransfer.Get(),0,nullptr);
            MarkLampTile(x,y,size);
            mapFaceDynamic[i]=dynamic;mapFaceRefresh[i]=mapRefreshFrame;++updatedFaces;
        }}}
        if(MvmDebugLog_Active()){
            uint64_t realtimeAge=0;for(auto i:realtimeFaces)realtimeAge=(std::max)(realtimeAge,mapRefreshFrame-mapFaceRefresh[i]);
            uint64_t actorAge=0;for(auto i:actorFaceList)actorAge=(std::max)(actorAge,mapRefreshFrame-mapFaceRefresh[i]);
            static ULONGLONG last=0;const auto now=GetTickCount64();
            if(now-last>=3000){last=now;MvmDebugLog_LinefAlways("environment.relight.shadow.spatial",
                "residentFaces=%u updatedFaces=%u backgroundBudget=%u realtimeFaces=%zu actorFaces=%u actorAge=%llu realtimeAge=%llu geometryChunks=%zu posedBounds=%zu boundsComplete=%u boundsMs=%llu cullMs=%llu dynamicMs=%llu",mapStaticDone,updatedFaces,refreshBudget,realtimeFaces.size(),actorFaces,actorAge,realtimeAge,state.worldRanges.size(),posedBounds.size(),unsigned(posedBoundsComplete),boundsMilliseconds,cullMs,GetTickCount64()-dynamicStarted);}
        }
        // Hand back every resident face, not just the ones drawn this frame.
        for(unsigned i=0;i<mapStaticDone;++i){auto exact=mapStaticMatrices[i];Relight::GuardStaticFace(exact.data(),1/Relight::StaticFaceGuard);matrices[Relight::StaticTile+i]=exact;}
        if(staticReady)*staticReady=mapStaticDone/Relight::ShadowFaces;
    }
    // Sky visibility converges over a few frames and then costs nothing: two directions per
    // frame, static geometry only (the same rigid set as the cached light tier).
    {
        uint64_t key=Hash(0,&input.occlusion,sizeof(const void*));key=Hash(key,&low,sizeof(low));key=Hash(key,&high,sizeof(high));
        if(key!=skyKey){skyKey=key;skyDone=0;}
        if(skyDone<SkyVisibilityDirections&&!skyArrayView){
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=SkyVisibilitySize;d.MipLevels=1;d.ArraySize=SkyVisibilityDirections;
            d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sr.Texture2DArray.MipLevels=1;sr.Texture2DArray.ArraySize=SkyVisibilityDirections;
            D3D11_TEXTURE2D_DESC z=d;z.ArraySize=1;z.BindFlags=D3D11_BIND_DEPTH_STENCIL;
            D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            if(FAILED(state.device->CreateTexture2D(&d,nullptr,&skyArray))||FAILED(state.device->CreateShaderResourceView(skyArray.Get(),&sr,&skyArrayView))||
               FAILED(state.device->CreateTexture2D(&z,nullptr,&skyDepth))||FAILED(state.device->CreateDepthStencilView(skyDepth.Get(),&ds,&skyDsv))){
                skyArray.Reset();skyArrayView.Reset();skyDepth.Reset();skyDsv.Reset();}
        }
        if(skyArrayView&&skyDone<SkyVisibilityDirections){GPU_PASS_SCOPE(ctx,"shadows: sky visibility");
            const auto directions=SkyVisibilityDirectionSet();Frame rigid=frame;rigid.scene.objects.clear();
            for(unsigned drawn=0;skyDone<SkyVisibilityDirections&&drawn<2;++drawn,++skyDone){
                skyMatrices[skyDone]=SunShadowMatrix(low,high,directions[skyDone]);
                Draw(ctx,rigid,skyMatrices[skyDone],skyDsv.Get(),SkyVisibilitySize);
                ctx->OMSetRenderTargets(0,nullptr,nullptr);
                ctx->CopySubresourceRegion(skyArray.Get(),skyDone,0,0,0,skyDepth.Get(),0,nullptr);
            }
        }
    }
    if(input.occlusion->skybox&&input.relight.sunShadows&&Relight::HasRadiance(Relight::SunRadiance(input.relight))){
        // Skybox geometry uses the engine's miniature coordinates. It must never enter the
        // foreground bounds, local-light cubes, rain shelter or world-space bounce volume.
        skyboxState.device=state.device;skyboxState.vertex=state.vertex;skyboxState.layout=state.layout;
        skyboxState.cutoutVertex=state.cutoutVertex;skyboxState.cutoutPixel=state.cutoutPixel;
        skyboxState.cutoutLayout=state.cutoutLayout;skyboxState.cutoutSampler=state.cutoutSampler;
        skyboxState.depth=state.depth;skyboxState.raster=state.raster;skyboxState.sunRaster=state.sunRaster;
        skyboxState.wind=state.wind;skyboxState.windStrength=state.windStrength;skyboxState.windValid=state.windValid;
        struct SkyScope {SkyScope(){std::swap(state,skyboxState);}~SkyScope(){std::swap(state,skyboxState);}} skyScope;
        bool ready=state.ready;
        if(!ready){ready=Buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,state.frame)&&
            Buffer(sizeof(Matrix),D3D11_BIND_CONSTANT_BUFFER,state.draw)&&
            Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw);state.ready=ready;}
        if(ready&&!skyboxSunView){
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::SunCascadeSize;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
            D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels=1;
            ready=SUCCEEDED(state.device->CreateTexture2D(&d,nullptr,&skyboxSunTexture))&&
                SUCCEEDED(state.device->CreateDepthStencilView(skyboxSunTexture.Get(),&ds,&skyboxSunDepth))&&
                SUCCEEDED(state.device->CreateShaderResourceView(skyboxSunTexture.Get(),&sr,&skyboxSunView));
        }
        if(ready){GPU_PASS_SCOPE(ctx,"shadows: 3D skybox sun");
            auto skyFrame=frame;skyFrame.occlusion=input.occlusion->skybox;skyFrame.scene.objects.clear();
            PrepareShadowMaps(ctx,skyFrame,false,true);
            if(state.world==skyFrame.occlusion&&skyboxSunView){
                skyboxSunMatrix=NativeSkyboxSunMatrix(SunShadowMatrix(state.world->low,state.world->high,sunDirection),
                    input.camera,state.world->skyboxOrigin,state.world->skyboxScale,
                    input.occlusion->low.z,input.occlusion->high.z,float(Relight::SunCascadeSize));
                Draw(ctx,skyFrame,skyboxSunMatrix,skyboxSunDepth.Get(),Relight::SunCascadeSize,false,AnimatedCasterSubset::FoliageOnly);
                Draw(ctx,skyFrame,skyboxSunMatrix,skyboxSunDepth.Get(),Relight::SunCascadeSize,true,AnimatedCasterSubset::FoliageOnly);
                skyboxSunReady=true;
                if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
                    MvmDebugLog_LinefAlways("environment.skybox.shadow","ready=1 resolution=%u rigidTriangles=%zu foliageBatches=%zu separateCoordinates=1",
                        Relight::SunCascadeSize,state.world->visual->rigidIndices.size()/3,state.casters.size());}}
            }
        }
    }
    {GPU_PASS_SCOPE(ctx,"shadows: lamp summary");RecordLampPyramid(ctx);}
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list))){skyboxSunReady=false;return 0;}
    {GPU_PASS_SCOPE(immediate,"shadows (map pass, total)");immediate->ExecuteCommandList(list.Get(),TRUE);}
    *atlas=mapAtlasView.Get();
    if(MvmDebugLog_Active()){
        static uint64_t reported=~0ull;
        if(reported!=input.relight.generation){
            reported=input.relight.generation;
            MvmDebugLog_LinefAlways("environment.relight.shadow","generation=%llu lights=%u faces=%u static=%u/%u staticFaces=%u/%u sun=%ux%u atlas=%ux%u localTile=%u staticTile=%u casters=%zu animated=%u",
                                    input.relight.generation,rendered,rendered*6,
                                    mapStaticDone/Relight::ShadowFaces,staticLights,
                                    mapStaticDone,staticLights*Relight::ShadowFaces,
                                    Relight::SunSize,Relight::SunSize,
                                    Relight::AtlasWidth,Relight::AtlasHeight,Relight::LocalSize,
                                    Relight::StaticSize,
                                    state.casters.size(),AnimatedCasterVertexCount());
        }
    }
    return rendered;
}
ID3D11ShaderResourceView* LampShadowPyramid(ID3D11ShaderResourceView* atlas){
    return g_lampSummary&&atlas&&atlas==mapAtlasView.Get()&&lampPyramid.ready&&!lampPyramid.failed?lampPyramid.builder.View():nullptr;
}
bool LampShadowSummary(){return g_lampSummary;}
void SetLampShadowSummary(bool enabled){g_lampSummary=enabled;}
ID3D11ShaderResourceView* SkyboxSunShadow(Matrix* matrix){
    if(!skyboxSunReady)return nullptr;
    if(matrix)*matrix=skyboxSunMatrix;
    return skyboxSunView.Get();
}
namespace {
// The first-person weapon and arms are drawn in their own pose, not the third-person body's.
// Sampling the ordinary close cascade put that body's arms and gun (posed differently, in the
// same place) as shadows across the first-person gun, and the first-person gun cast nothing
// on the hands holding it. Before the first-person pass the close cascade is redrawn for it:
// the cached static world, other players (the viewer's own body excluded), and the
// first-person pose itself. The world's own draws are already finished by then; the next
// frame redraws the tile normally, so the viewer's own shadow on the ground is unaffected.
constexpr char kViewerShadowShader[]=R"HLSL(
cbuffer Draw:register(b3){row_major float4x4 transform;}
cbuffer Viewer:register(b4){float4 viewer;float4 extent;} // eye xyz, radius | above, below
struct Varying{float4 position:SV_Position;float3 world:TEXCOORD0;};
Varying VS(float3 p:POSITION){Varying o;o.position=mul(transform,float4(p,1));o.world=p;return o;}
void PS(Varying i){float3 d=i.world-viewer.xyz;
    if(dot(d.xy,d.xy)<viewer.w*viewer.w&&d.z<extent.x&&d.z>-extent.y)discard;}
)HLSL";
ComPtr<ID3D11VertexShader> viewerVertex;ComPtr<ID3D11PixelShader> viewerPixel;ComPtr<ID3D11Buffer> viewerConstants;
uint64_t viewerGeneration=~0ull;bool viewerFailed=false;
bool ViewerShaders(ID3D11Device* device){
    if(viewerVertex&&viewerPixel&&viewerConstants)return true;if(viewerFailed)return false;viewerFailed=true;
    ComPtr<ID3DBlob> vs,ps,errors;
    if(FAILED(CompileEnvironmentShader(kViewerShadowShader,sizeof(kViewerShadowShader)-1,"EnvironmentViewerShadow",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&vs,&errors))||
       FAILED(CompileEnvironmentShader(kViewerShadowShader,sizeof(kViewerShadowShader)-1,"EnvironmentViewerShadow",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&ps,&errors))){
        MvmDebugLog_LinefAlways("environment.shadow.viewmodel","shader failed: %s",errors?static_cast<const char*>(errors->GetBufferPointer()):"compiler unavailable");return false;}
    D3D11_BUFFER_DESC b{};b.ByteWidth=32;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&viewerVertex))||
       FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&viewerPixel))||
       FAILED(device->CreateBuffer(&b,nullptr,&viewerConstants)))return false;
    viewerFailed=false;return true;
}
}
unsigned ActorShadowFacesPerFrame(){return g_actorShadowFacesPerFrame;}
void SetActorShadowFacesPerFrame(unsigned faces){g_actorShadowFacesPerFrame=faces;}
void RequestSunCascadeDump(const char* path){g_sunDumpPath=path?path:"";}
unsigned PrepareGlassBeamShadows(ID3D11DeviceContext* immediate,const Matrix* matrices,unsigned count){
    count=(std::min)(count,Relight::GlassBeamTiles);
    if(!immediate||!matrices||!count||!mapBeamFrameReady||!mapAtlas||!mapStaticDsv||!mapStaticTransfer||!proofContext)return 0;
    uint64_t key=1469598103934665603ull;key=Hash(key,matrices,sizeof(Matrix)*count);key=Hash(key,&count,sizeof(count));
    const auto* world=mapBeamFrame.occlusion.get();key=Hash(key,&world,sizeof(world));
    if(key==mapBeamKey)return mapBeamCount;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready)return 0;
    auto ctx=proofContext.Get();ctx->ClearState();
    g_clipOrthographic=true;
    for(unsigned i=0;i<count;++i){
        Draw(ctx,mapBeamFrame,matrices[i],mapStaticDsv.Get(),Relight::StaticSize);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        ctx->CopyResource(mapStaticTransfer.Get(),mapStaticDepth.Get());
        unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::GlassBeamTile+i,x,y,size);
        ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapStaticTransfer.Get(),0,nullptr);
        MarkLampTile(x,y,size);
    }
    g_clipOrthographic=false;
    RecordLampPyramid(ctx);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return 0;
    {GPU_PASS_SCOPE(immediate,"shadows: glass beams");immediate->ExecuteCommandList(list.Get(),TRUE);}
    mapBeamMatrices.assign(matrices,matrices+count);
    mapBeamKey=key;mapBeamCount=count;
    MvmDebugLog_LinefAlways("environment.shadow.glassbeams","rendered %u stained glass beam shadow maps",count);
    return count;
}
bool PrepareViewmodelSunShadow(ID3D11DeviceContext* immediate){
    if(!immediate||!mapCloseReady||!mapAtlas||!mapSunDepth||!mapSunTexture||!mapSunTransfer||!proofContext)return false;
    const uint64_t generation=AnimatedCasterGeneration();
    if(viewerGeneration==generation)return true;
    viewerGeneration=generation;
    if(!AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel))return false;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready||!state.vertex||!state.layout||!state.device||!ViewerShaders(state.device.Get()))return false;
    auto ctx=proofContext.Get();ctx->ClearState();
    unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::SunCloseTile,x,y,size);
    // Static environment depth: the close cascade's retained cache, or (no posed casters this
    // frame, so no cache) the tile as it is in the atlas.
    const auto& cache=state.cached[27];
    if(cache.texture&&cache.valid)ctx->CopyResource(mapSunTexture.Get(),cache.texture.Get());
    else{const D3D11_BOX box{x,y,0,x+size,y+size,1};ctx->CopySubresourceRegion(mapSunTexture.Get(),0,0,0,0,mapAtlas.Get(),0,&box);}
    ctx->OMSetRenderTargets(0,nullptr,mapSunDepth.Get());
    ctx->OMSetDepthStencilState(state.depth.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,~0u);
    const D3D11_VIEWPORT viewport{0,0,float(Relight::SunCascadeSize),float(Relight::SunCascadeSize),0,1};ctx->RSSetViewports(1,&viewport);
    ctx->RSSetState(state.sunRaster.Get());
    ctx->IASetInputLayout(state.layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto cb=state.draw.Get();ctx->VSSetConstantBuffers(3,1,&cb);
    ctx->UpdateSubresource(cb,0,nullptr,mapCloseMatrix.data(),0,0);
    const float viewer[8]{mapCloseCamera.x,mapCloseCamera.y,mapCloseCamera.z,Relight::ViewerShadowRadius,Relight::ViewerShadowAbove,Relight::ViewerShadowBelow,0,0};
    ctx->UpdateSubresource(viewerConstants.Get(),0,nullptr,viewer,0,0);
    auto vc=viewerConstants.Get();ctx->PSSetConstantBuffers(4,1,&vc);
    ctx->VSSetShader(viewerVertex.Get(),nullptr,0);ctx->PSSetShader(viewerPixel.Get(),nullptr,0);
    DrawAnimatedCasters(ctx,AnimatedCasterSubset::Actors,&mapCloseMatrix);
    const auto anchored=Multiply(mapCloseMatrix,ModelMatrix(mapCloseCamera,{0,0,0},{1,1,1}));
    ctx->UpdateSubresource(cb,0,nullptr,anchored.data(),0,0);
    ctx->VSSetShader(state.vertex.Get(),nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);
    DrawAnimatedCasters(ctx,AnimatedCasterSubset::Viewmodel);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    ctx->CopyResource(mapSunTransfer.Get(),mapSunTexture.Get());
    // Keep the world's tile: every body and surface drawn after the first-person block must still
    // read it. Without this, players drawn after the viewmodel read a cascade with the actors near
    // the camera removed and snapped between sunlit and self-shadowed frame to frame (2026-10-13).
    const bool backup=mapSunWorldBackup!=nullptr;
    if(backup){const D3D11_BOX tile{x,y,0,x+size,y+size,1};ctx->CopySubresourceRegion(mapSunWorldBackup.Get(),0,0,0,0,mapAtlas.Get(),0,&tile);}
    ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunTransfer.Get(),0,nullptr);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return false;
    {GPU_PASS_SCOPE(immediate,"shadows: viewmodel sun");immediate->ExecuteCommandList(list.Get(),TRUE);}
    mapSunViewmodelActive=backup;
    // The tile now holds the first-person version: the map pass must rebuild the world's version
    // next frame instead of keeping the tile as current.
    state.cached[27].composed=false;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("environment.shadow.viewmodel","redrawn close=%ux%u actors=%u viewmodel=%u cache=%u camera=(%.0f %.0f %.0f)",size,size,
            AnimatedCasterVertexCount(AnimatedCasterSubset::Actors),AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel),unsigned(cache.texture&&cache.valid),
            mapCloseCamera.x,mapCloseCamera.y,mapCloseCamera.z);}}
    return true;
}
void RestoreWorldSunTile(ID3D11DeviceContext* immediate){
    if(!mapSunViewmodelActive||!immediate||!mapAtlas||!mapSunWorldBackup)return;
    mapSunViewmodelActive=false;
    unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::SunCloseTile,x,y,size);
    immediate->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunWorldBackup.Get(),0,nullptr);
}
bool PrepareLightingProofShadows(ID3D11DeviceContext* immediate,const Frame& input,Matrix* matrices,ID3D11ShaderResourceView** atlas) {
    if(!immediate||!matrices||!atlas||!state.ready||!input.occlusion)return false;
    RenderPrivateTextureScope privateTextures;
    if(!RelightPrivateShadowState(input.occlusion))return false;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready){
        if(!Buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,state.frame)||!Buffer(sizeof(Matrix),D3D11_BIND_CONSTANT_BUFFER,state.draw)||!Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw))return false;
        state.ready=true;
    }
    if(!proofAtlasView||!proofSunDepth||!proofTransfer){
        D3D11_TEXTURE2D_DESC d{};d.Width=4096;d.Height=2048;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels=1;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofAtlas))||FAILED(state.device->CreateShaderResourceView(proofAtlas.Get(),&sr,&proofAtlasView)))return false;
        // D3D11 depth resources only permit whole-subresource copies with zero
        // destination offsets. Transfer into a same-size non-depth texture first;
        // packing that texture into the atlas then uses ordinary texture copies.
        d.Width=d.Height=1024;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofTransfer)))return false;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofSun))||FAILED(state.device->CreateDepthStencilView(proofSun.Get(),&ds,&proofSunDepth)))return false;
    }
    Frame frame=input;frame.proof.enabled=false;frame.scene.weather.type=0;
    frame.scene.climate[CL_contactShadows]=0;frame.scene.climate[CL_sunPower]=0;
    frame.scene.objects.erase(std::remove_if(frame.scene.objects.begin(),frame.scene.objects.end(),[](const Object& o){return o.IsLight();}),frame.scene.objects.end());
    Object lamp;lamp.kind="point";lamp.position=input.proof.position;lamp.range=input.proof.range;lamp.shadows=true;frame.scene.objects.push_back(lamp);
    auto ctx=proofContext.Get();ctx->ClearState();PrepareShadowMaps(ctx,frame,false);
    if(state.world!=input.occlusion||!state.local||state.constants.light[0].x!=0){ctx->ClearState();ComPtr<ID3D11CommandList> discarded;ctx->FinishCommandList(FALSE,&discarded);return false;}
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    for(UINT face=0;face<6;++face){
        matrices[face]=state.constants.local[face];
        ctx->CopySubresourceRegion(proofTransfer.Get(),0,0,0,0,state.local.Get(),face,nullptr);
        ctx->CopySubresourceRegion(proofAtlas.Get(),0,(face%3)*1024,(face/3)*1024,0,proofTransfer.Get(),0,nullptr);
    }
    auto low=input.occlusion->low,high=input.occlusion->high;
    if(input.occlusion->visual){const auto& v=*input.occlusion->visual;low={std::min(low.x,v.low.x),std::min(low.y,v.low.y),std::min(low.z,v.low.z)};high={std::max(high.x,v.high.x),std::max(high.y,v.high.y),std::max(high.z,v.high.z)};}
    matrices[6]=SunShadowMatrix(low,high,ClimateSunDirection(input.scene.climate));
    auto hash=Hash(state.localKeys[0],matrices[6].data(),sizeof(Matrix));
    const bool animated=state.animate||HasAnimatedCasterGeometry();
    DrawCached(ctx,frame,matrices[6],proofSunDepth.Get(),1024,24,hash,AnimatedCasterGeneration(),animated);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(proofTransfer.Get(),proofSun.Get());
    ctx->CopySubresourceRegion(proofAtlas.Get(),0,3072,0,0,proofTransfer.Get(),0,nullptr);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return false;
    {GPU_PASS_SCOPE(immediate,"shadows: lighting proof");immediate->ExecuteCommandList(list.Get(),TRUE);}
    static uint64_t inspected=~0ull;
    if(MvmDebugLog_Active()&&inspected!=input.proof.generation){
        inspected=input.proof.generation;
        D3D11_TEXTURE2D_DESC d{};d.Width=6;d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> samples;
        if(SUCCEEDED(state.device->CreateTexture2D(&d,nullptr,&samples))){
            for(UINT face=0;face<6;++face){const UINT x=(face%3)*1024+512,y=(face/3)*1024+512;
                const D3D11_BOX box{x,y,0,x+1,y+1,1};immediate->CopySubresourceRegion(samples.Get(),0,face,0,0,proofAtlas.Get(),0,&box);}
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(SUCCEEDED(immediate->Map(samples.Get(),0,D3D11_MAP_READ,0,&mapped))){
                const auto z=static_cast<const float*>(mapped.pData);
                MvmDebugLog_LinefAlways("environment.proof.shadow","generation=%llu origin=(%.2f %.2f %.2f) centerDepth=(%.7f %.7f %.7f %.7f %.7f %.7f) triangles=%zu actors=%zu",
                    inspected,input.proof.position.x,input.proof.position.y,input.proof.position.z,z[0],z[1],z[2],z[3],z[4],z[5],state.world->indices.size()/3,size_t(AnimatedCasterVertexCount()));
                immediate->Unmap(samples.Get(),0);
            }
        }
    }
    *atlas=proofAtlasView.Get();return true;
}
bool SkyVisibilityMaps(ID3D11ShaderResourceView** maps,Matrix* matrices) {
    if(!maps||!matrices||!skyArrayView||skyDone<SkyVisibilityDirections)return false;
    *maps=skyArrayView.Get();
    for(unsigned i=0;i<SkyVisibilityDirections;++i)matrices[i]=skyMatrices[i];
    return true;
}
void BindShadowMaps(ID3D11DeviceContext *ctx, bool compute) {
    auto cb = state.ready ? state.frame.Get() : nullptr;
    if (compute)
        ctx->CSSetConstantBuffers(2, 1, &cb);
    else
        ctx->PSSetConstantBuffers(2, 1, &cb);
    auto self=state.viewmodelSunSrv.Get();
    if(compute)ctx->CSSetShaderResources(21,1,&self);else ctx->PSSetShaderResources(21,1,&self);
    ID3D11ShaderResourceView* receiver[]={state.receiverSunSrv.Get(),state.receiverLocalSrv.Get()};
    if(compute)ctx->CSSetShaderResources(19,2,receiver);else ctx->PSSetShaderResources(19,2,receiver);
    if(!compute){auto shelter=state.shelterSrv.Get();ctx->PSSetShaderResources(13,1,&shelter);
        auto gust=state.gustSrv.Get();ctx->PSSetShaderResources(30,1,&gust);}
    auto nearSun = state.nearSunSrv.Get();
    auto closeSun=state.closeSunSrv.Get();
    auto detailSun=state.detailSunSrv.Get();
    if(compute)ctx->CSSetShaderResources(18,1,&detailSun);else ctx->PSSetShaderResources(18,1,&detailSun);
    if(compute)ctx->CSSetShaderResources(16,1,&closeSun);else ctx->PSSetShaderResources(16,1,&closeSun);
    if (compute) ctx->CSSetShaderResources(15, 1, &nearSun);
    else ctx->PSSetShaderResources(15, 1, &nearSun);
    ID3D11ShaderResourceView *srv[] = {state.localSrv.Get(), state.sunSrv.Get()};
    if (compute)
        ctx->CSSetShaderResources(5, 2, srv);
    else
        ctx->PSSetShaderResources(5, 2, srv);
}
} // namespace Filmmaker::Environment
