#define NOMINMAX
// The lamp shadow tiers: the budgeted lamps drawn every frame, the cached static tier with its
// refresh rotation, the lit bounds read back from it and the tiled refresh target.
#include "EnvironmentShadowMapsDetail.h"
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
using namespace ShadowDetail;
namespace {
// Lit bounds of the static tier's lamps (Relight/RelightLitBoundsMath.h). Once every face of the
// tier is drawn, one compute pass takes the farthest depth of each 16-texel block of mapStaticBase
// (rigid map geometry only, so no moving caster shortens it), a staging copy brings the 64 values
// per face to the CPU a few frames later, and each lamp gets the box around what that depth lets
// it reach. The results belong to one mapStaticKey.
static_assert(Relight::LitBlocks==8&&Relight::LitBlockTexels==16,"LitBlocksShader's block layout");
constexpr char LitBlocksShader[]=R"HLSL(
Texture2DArray<float> base:register(t0);
RWBuffer<float> blocks:register(u0);
[numthreads(8,8,1)]
void Main(uint3 id:SV_DispatchThreadID){
    float farthest=0;
    [loop]for(uint j=0;j<16;++j)[loop]for(uint i=0;i<16;++i){
        const float d=base.Load(int4(id.x*16+i,id.y*16+j,id.z,0));
        farthest=max(farthest,d>=0&&d<=1?d:1);
    }
    blocks[id.z*64+id.y*8+id.x]=farthest;
}
)HLSL";
struct LitBoundsState {
    ComPtr<ID3D11ShaderResourceView> baseView;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> blocks,readback;
    ComPtr<ID3D11UnorderedAccessView> blocksUav;
    bool failed=false,pending=false;
    // What the readback in flight was recorded for, and what `data` holds.
    struct Tier {uint64_t key=0;std::vector<Matrix> matrices;std::vector<Vec3> positions;std::vector<float> ranges;std::vector<uint8_t> faces;};
    Tier recorded,resolved;
    std::vector<float> data;
    // Boxes for one receiver bias, rebuilt from `data` when the bias changes.
    std::vector<Relight::LitBounds> bounds;float boundsBias=-1;
} litBounds;
} // namespace
void ShadowDetail::ResetLitBounds(){litBounds=LitBoundsState{};}
namespace {
bool EnsureLitBounds(){
    auto& l=litBounds;
    if(l.failed||!mapStaticBase||!state.device)return false;
    const UINT slices=Relight::StaticShadowLimit*Relight::ShadowFaces,values=slices*Relight::LitFaceBlocks;
    if(!l.baseView){
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sr.Texture2DArray.MipLevels=1;sr.Texture2DArray.ArraySize=slices;
        if(FAILED(state.device->CreateShaderResourceView(mapStaticBase.Get(),&sr,&l.baseView))){l.failed=true;return false;}
    }
    if(!l.shader){
        ComPtr<ID3DBlob> code,errors;
        if(FAILED(CompileEnvironmentShader(LitBlocksShader,sizeof(LitBlocksShader)-1,"EnvironmentLitBlocks",nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&errors))||
           FAILED(state.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&l.shader))){
            l.failed=true;MvmDebugLog_LinefAlways("environment.shadow.litbounds","disabled: %s",errors?static_cast<const char*>(errors->GetBufferPointer()):"shader");return false;}
    }
    if(!l.blocks||!l.readback||!l.blocksUav){
        l.blocks.Reset();l.readback.Reset();l.blocksUav.Reset();
        D3D11_BUFFER_DESC b{};b.ByteWidth=values*4;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ua{};ua.Format=DXGI_FORMAT_R32_FLOAT;ua.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ua.Buffer.NumElements=values;
        D3D11_BUFFER_DESC r{};r.ByteWidth=values*4;r.Usage=D3D11_USAGE_STAGING;r.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(state.device->CreateBuffer(&b,nullptr,&l.blocks))||FAILED(state.device->CreateUnorderedAccessView(l.blocks.Get(),&ua,&l.blocksUav))||
           FAILED(state.device->CreateBuffer(&r,nullptr,&l.readback))){l.failed=true;return false;}
    }
    return true;
}
// On the map pass's deferred context, behind the draws and copies of the tier's last faces.
void RecordLitBlocks(ID3D11DeviceContext* ctx,unsigned slices){
    auto& l=litBounds;
    ctx->CSSetShader(l.shader.Get(),nullptr,0);
    auto view=l.baseView.Get();ctx->CSSetShaderResources(0,1,&view);
    auto write=l.blocksUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&write,nullptr);
    ctx->Dispatch(1,1,slices);
    view=nullptr;write=nullptr;ctx->CSSetShaderResources(0,1,&view);ctx->CSSetUnorderedAccessViews(0,1,&write,nullptr);
    ctx->CSSetShader(nullptr,nullptr,0);
    ctx->CopyResource(l.readback.Get(),l.blocks.Get());
}
// On the immediate context: takes the block depths once the GPU has them, without waiting for it.
void ResolveLitBounds(ID3D11DeviceContext* immediate){
    auto& l=litBounds;
    if(!l.pending||!l.readback)return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr=immediate->Map(l.readback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return;
    l.pending=false;
    if(FAILED(hr))return;
    const size_t values=l.recorded.matrices.size()*Relight::LitFaceBlocks;
    l.data.assign(static_cast<const float*>(mapped.pData),static_cast<const float*>(mapped.pData)+values);
    immediate->Unmap(l.readback.Get(),0);
    l.resolved=std::move(l.recorded);l.recorded={};l.bounds.clear();l.boundsBias=-1;
    MvmDebugLog_LinefAlways("environment.shadow.litbounds","resolved lamps=%zu faces=%zu",l.resolved.positions.size(),l.resolved.matrices.size());
}
} // namespace
bool StaticLampLitBounds(unsigned slot,float shadowBias,Vec3& low,Vec3& high){
    auto& l=litBounds;
    if(!mapStaticKey||l.resolved.key!=mapStaticKey||slot>=l.resolved.positions.size())return false;
    if(l.bounds.size()!=l.resolved.positions.size()||l.boundsBias!=shadowBias){
        l.bounds.assign(l.resolved.positions.size(),Relight::LitBounds{});l.boundsBias=shadowBias;
        const float margin=Relight::LitDepthMargin(shadowBias);
        double kept=0;unsigned valid=0,cubes=0;
        for(size_t lamp=0;lamp<l.bounds.size();++lamp){
            const size_t first=lamp*Relight::ShadowFaces;
            l.bounds[lamp]=Relight::LampLitBounds(l.data.data()+first*Relight::LitFaceBlocks,l.resolved.matrices.data()+first,
                l.resolved.faces[lamp],l.resolved.positions[lamp],margin);
            if(!l.bounds[lamp].valid)continue;
            ++valid;
            // Cube lamps only: how much of the range box the lit box keeps.
            if(l.resolved.faces[lamp]!=Relight::ShadowFaces||!(l.resolved.ranges[lamp]>0))continue;
            const float r=l.resolved.ranges[lamp];const Vec3 p=l.resolved.positions[lamp];const auto& b=l.bounds[lamp];
            const float x=(std::max)(0.f,(std::min)(p.x+r,b.high.x)-(std::max)(p.x-r,b.low.x)),y=(std::max)(0.f,(std::min)(p.y+r,b.high.y)-(std::max)(p.y-r,b.low.y)),
                z=(std::max)(0.f,(std::min)(p.z+r,b.high.z)-(std::max)(p.z-r,b.low.z));
            kept+=double(x)*y*z/(8.0*r*r*r);++cubes;
        }
        MvmDebugLog_LinefAlways("environment.shadow.litbounds","boxes=%u of %zu bias=%g margin=%g cubeLamps=%u keptShareOfRangeBox=%.3f",
            valid,l.bounds.size(),shadowBias,margin,cubes,cubes?kept/cubes:0.0);
    }
    const auto& bounds=l.bounds[slot];
    if(!bounds.valid)return false;
    low=bounds.low;high=bounds.high;return true;
}
namespace {
// Tiled lamp face refresh. A refresh batch used to give each of its faces a depth target and a
// transfer texture of its own: per face a copy of the static depth in, a target switch, the caster
// draws, a copy out and a copy into the atlas. Sixty-four faces a frame made that about a
// millisecond of GPU time that was mostly target switches and copies, not triangles. Here a batch
// shares one depth target of 4 x 4 tiles: the static depth is drawn into each tile (a depth-only
// triangle that loads mapStaticBase), the casters are drawn with the tile as viewport and scissor,
// and one copy takes the whole target out before each tile goes to its place in the atlas.
constexpr unsigned TileRefreshColumns=4;
static_assert(TileRefreshColumns*TileRefreshColumns==StaticRefreshBatch,"one tile per face of a batch");
constexpr char TileRestoreShader[]=R"HLSL(
cbuffer Tile:register(b0){int2 origin;int slice;int pad;}
Texture2DArray<float> base:register(t0);
float4 VS(uint id:SV_VertexID):SV_Position{return float4(float2((id<<1)&2,id&2)*2-1,0,1);}
float PS(float4 p:SV_Position):SV_Depth{return base.Load(int4(int2(p.xy)-origin,slice,0));}
)HLSL";
struct TileRefresh {
    ComPtr<ID3D11Texture2D> depth,transfer;ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> baseView;
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11DepthStencilState> always;ComPtr<ID3D11RasterizerState> restoreRaster,casterRaster;
    bool failed=false;
} tileRefresh;
} // namespace
void ShadowDetail::ResetTileRefresh(){tileRefresh=TileRefresh{};}
namespace {
bool EnsureTileRefresh(){
    auto& t=tileRefresh;
    if(t.failed||!mapStaticBase||!state.device||!state.raster)return false;
    if(t.dsv&&t.transfer&&t.baseView&&t.vertex&&t.pixel&&t.constants&&t.always&&t.restoreRaster&&t.casterRaster)return true;
    const auto fail=[&](const char* what){t=TileRefresh{};t.failed=true;MvmDebugLog_LinefAlways("environment.shadow.tilerefresh","disabled: %s",what);return false;};
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=Relight::StaticSize*TileRefreshColumns;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&t.transfer)))return fail("transfer");
    d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&t.depth))||FAILED(state.device->CreateDepthStencilView(t.depth.Get(),&ds,&t.dsv)))return fail("depth target");
    D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sr.Texture2DArray.MipLevels=1;sr.Texture2DArray.ArraySize=Relight::StaticShadowLimit*Relight::ShadowFaces;
    if(FAILED(state.device->CreateShaderResourceView(mapStaticBase.Get(),&sr,&t.baseView)))return fail("static depth view");
    ComPtr<ID3DBlob> vs,ps,errors;
    if(FAILED(CompileEnvironmentShader(TileRestoreShader,sizeof(TileRestoreShader)-1,"EnvironmentTileRestore",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&vs,&errors))||
       FAILED(CompileEnvironmentShader(TileRestoreShader,sizeof(TileRestoreShader)-1,"EnvironmentTileRestore",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&ps,&errors))||
       FAILED(state.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&t.vertex))||
       FAILED(state.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&t.pixel)))return fail("shader");
    D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(state.device->CreateBuffer(&b,nullptr,&t.constants)))return fail("constants");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    if(FAILED(state.device->CreateDepthStencilState(&depth,&t.always)))return fail("depth state");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
    if(FAILED(state.device->CreateRasterizerState(&raster,&t.restoreRaster)))return fail("restore rasterizer");
    // The casters keep the face rasterizer's own settings, with the scissor added.
    state.raster->GetDesc(&raster);raster.ScissorEnable=TRUE;
    if(FAILED(state.device->CreateRasterizerState(&raster,&t.casterRaster)))return fail("caster rasterizer");
    return true;
}
} // namespace
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
    ResolveLitBounds(immediate);
    bool litBlocksRecorded=false;
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
        // Readable by the lit bounds summary (LitBlocksShader).
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
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
    // Budgeted lights keep the cache slot they had: a transient light entering at the front of the
    // budget used to shift every other light to a new slot, and each slot's caches are keyed by
    // the light in it, so all its faces and the sun tier redrew static geometry. order[slot] is the
    // caller's index; outputs below go back to the caller's tiles.
    std::array<unsigned,Relight::ShadowBudgetLimit> order{};
    for(unsigned s=0;s<lights;++s)order[s]=s;
    if(ShadowSlotStability.load(std::memory_order_relaxed)){
        std::array<bool,Relight::ShadowBudgetLimit> taken{},filled{};
        for(unsigned s=0;s<lights&&s<mapSlotCount;++s)for(unsigned i=0;i<lights;++i)
            if(!taken[i]&&positions[i].x==mapSlotPositions[s].x&&positions[i].y==mapSlotPositions[s].y&&
               positions[i].z==mapSlotPositions[s].z&&ranges[i]==mapSlotRanges[s]){order[s]=i;taken[i]=filled[s]=true;break;}
        unsigned next=0;
        for(unsigned s=0;s<lights;++s)if(!filled[s]){while(taken[next])++next;order[s]=next;taken[next]=true;}
    }
    for(unsigned s=0;s<lights;++s){mapSlotPositions[s]=positions[order[s]];mapSlotRanges[s]=ranges[order[s]];}
    mapSlotCount=lights;
    for(unsigned slot=0;slot<lights;++slot){const unsigned i=order[slot];
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
    // Casters that moved since the last frame. The capture takes props every frame too (a parked
    // truck, a flower pot), and a block of triangles whose bounds are exactly last frame's has not
    // moved: its shadow in a cached face is already right, so it does not earn that face the
    // every-frame refresh a walking player does. Such a face still refreshes in the rotation.
    std::vector<CasterBounds> movingBounds;
    const bool movingOnly=ShadowMovingActors.load(std::memory_order_relaxed)&&posedBoundsComplete;
    if(posedBoundsComplete){
        const auto less=[](const CasterBounds& a,const CasterBounds& b){return std::memcmp(&a,&b,sizeof(CasterBounds))<0;};
        if(movingOnly)for(const auto& bounds:posedBounds)
            if(!ValidCasterBounds(bounds)||!std::binary_search(mapPreviousBounds.begin(),mapPreviousBounds.end(),bounds,less))movingBounds.push_back(bounds);
        mapPreviousBounds=posedBounds;std::sort(mapPreviousBounds.begin(),mapPreviousBounds.end(),less);
    }
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
        const unsigned caller=order[light];
        const unsigned faces=projectedSource(positions[caller])?1u:Relight::ShadowFaces;
        for(UINT face=0;face<faces;++face){
            const unsigned tile=caller*6+face;
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
            const auto hash=Hash(ShadowSlotStability.load(std::memory_order_relaxed)?state.worldKey:state.localKeys[0],matrices[tile].data(),sizeof(Matrix));
            {GPU_PASS_SCOPE(ctx,"shadows: sun cascade draws");
            DrawCached(ctx,frame,matrices[tile],mapSunDepth.Get(),Relight::SunCascadeSize,24+cascade,hash,
                       AnimatedCasterGeneration(),animated);}
            GPU_PASS_SCOPE(ctx,"shadows: sun cascade copies");
            ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(mapSunTransfer.Get(),mapSunTexture.Get());
            unsigned x=0,y=0,size=0;Relight::TileOrigin(tile,x,y,size);
            ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunTransfer.Get(),0,nullptr);
            MarkSunTile(x,y,size);
        }
        if(g_sunSummary){GPU_PASS_SCOPE(ctx,"shadows: sun summary");RecordSunPyramid(ctx);}
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
            const bool knownActor=actor&&posedBoundsComplete&&
                (!movingOnly||CasterBoundsIntersect(movingBounds,mapStaticMatrices[i],true,staticPositions[owner],staticShadowReach[owner]));
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
        const bool tiled=batched&&ShadowTileRefresh.load(std::memory_order_relaxed)&&EnsureTileRefresh();
        for(size_t batch=0;batch<selected.size();batch+=batchSize){
        const auto count=unsigned((std::min)(size_t(batchSize),selected.size()-batch));
        std::array<Matrix,batchSize> cullMatrices{};
        for(unsigned j=0;j<count;++j)cullMatrices[j]=mapStaticMatrices[selected[batch+j]];
        PrepareAnimatedCasterCulling(ctx,cullMatrices.data(),count);
        if(tiled){
            auto& t=tileRefresh;
            const auto origin=[](unsigned j){return std::array<UINT,2>{(j%TileRefreshColumns)*Relight::StaticSize,(j/TileRefreshColumns)*Relight::StaticSize};};
            {GPU_PASS_SCOPE(ctx,"shadows: lamp refresh copies");
            // The static depth of every face of the batch, each into its own tile.
            ctx->OMSetRenderTargets(0,nullptr,t.dsv.Get());ctx->OMSetDepthStencilState(t.always.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,~0u);
            ctx->RSSetState(t.restoreRaster.Get());
            ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(t.vertex.Get(),nullptr,0);ctx->PSSetShader(t.pixel.Get(),nullptr,0);
            auto base=t.baseView.Get();ctx->PSSetShaderResources(0,1,&base);
            auto constants=t.constants.Get();ctx->PSSetConstantBuffers(0,1,&constants);
            for(unsigned j=0;j<count;++j){const auto at=origin(j);
                const int tile[4]{int(at[0]),int(at[1]),int(selected[batch+j]),0};
                ctx->UpdateSubresource(constants,0,nullptr,tile,0,0);
                const D3D11_VIEWPORT viewport{float(at[0]),float(at[1]),float(Relight::StaticSize),float(Relight::StaticSize),0,1};
                const D3D11_RECT scissor{LONG(at[0]),LONG(at[1]),LONG(at[0]+Relight::StaticSize),LONG(at[1]+Relight::StaticSize)};
                ctx->RSSetViewports(1,&viewport);ctx->RSSetScissorRects(1,&scissor);ctx->Draw(3,0);
            }
            base=nullptr;ctx->PSSetShaderResources(0,1,&base);constants=nullptr;ctx->PSSetConstantBuffers(0,1,&constants);}
            {GPU_PASS_SCOPE(ctx,"shadows: lamp refresh draws");
            for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];if(!faceDynamic[i])continue;
                const auto at=origin(j);const DrawTile tile{at[0],at[1],t.casterRaster.Get()};
                Draw(ctx,frame,mapStaticMatrices[i],t.dsv.Get(),Relight::StaticSize,true,AnimatedCasterSubset::All,true,&mapFoliageDraws[i],&tile);}}
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            GPU_PASS_SCOPE(ctx,"shadows: lamp refresh copies");
            ctx->CopyResource(t.transfer.Get(),t.depth.Get());
            for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];const auto at=origin(j);
                unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::StaticTile+i,x,y,size);
                const D3D11_BOX box{at[0],at[1],0,at[0]+Relight::StaticSize,at[1]+Relight::StaticSize,1};
                ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,t.transfer.Get(),0,&box);
                MarkLampTile(x,y,size);
                mapFaceDynamic[i]=faceDynamic[i];mapFaceRefresh[i]=mapRefreshFrame;++updatedFaces;
            }
            continue;
        }
        if(batched){
            // The same copies and draws as below, issued phase by phase across the batch.
            {GPU_PASS_SCOPE(ctx,"shadows: lamp refresh copies");
            for(unsigned j=0;j<count;++j)
                ctx->CopySubresourceRegion(mapStaticScratch[j].depth.Get(),0,0,0,0,mapStaticBase.Get(),selected[batch+j],nullptr);}
            {GPU_PASS_SCOPE(ctx,"shadows: lamp refresh draws");
            for(unsigned j=0;j<count;++j){const unsigned i=selected[batch+j];
                if(faceDynamic[i])Draw(ctx,frame,mapStaticMatrices[i],mapStaticScratch[j].dsv.Get(),Relight::StaticSize,true,AnimatedCasterSubset::All,true,&mapFoliageDraws[i]);}}
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            GPU_PASS_SCOPE(ctx,"shadows: lamp refresh copies");
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
                "residentFaces=%u updatedFaces=%u backgroundBudget=%u realtimeFaces=%zu actorFaces=%u actorAge=%llu realtimeAge=%llu geometryChunks=%zu posedBounds=%zu movingBounds=%zu movingOnly=%u boundsComplete=%u boundsMs=%llu cullMs=%llu dynamicMs=%llu",mapStaticDone,updatedFaces,refreshBudget,realtimeFaces.size(),actorFaces,actorAge,realtimeAge,state.worldRanges.size(),posedBounds.size(),movingBounds.size(),unsigned(movingOnly),unsigned(posedBoundsComplete),boundsMilliseconds,cullMs,GetTickCount64()-dynamicStarted);}
        }
        // Hand back every resident face, not just the ones drawn this frame.
        for(unsigned i=0;i<mapStaticDone;++i){auto exact=mapStaticMatrices[i];Relight::GuardStaticFace(exact.data(),1/Relight::StaticFaceGuard);matrices[Relight::StaticTile+i]=exact;}
        if(staticReady)*staticReady=mapStaticDone/Relight::ShadowFaces;
        // The complete tier's depth summary for the lit bounds, once per set of lights.
        if(ShadowLitBounds.load(std::memory_order_relaxed)&&wanted&&mapStaticDone==wanted&&!litBounds.pending&&
           litBounds.resolved.key!=mapStaticKey&&EnsureLitBounds()){
            RecordLitBlocks(ctx,wanted);
            auto& tier=litBounds.recorded;tier.key=mapStaticKey;tier.matrices=mapStaticMatrices;
            tier.positions.assign(staticPositions,staticPositions+staticLights);tier.ranges.assign(staticRanges,staticRanges+staticLights);tier.faces.resize(staticLights);
            for(unsigned l=0;l<staticLights;++l)tier.faces[l]=projectedSource(staticPositions[l])?1:uint8_t(Relight::ShadowFaces);
            litBlocksRecorded=true;
        }
    }
    // Sky visibility converges over a few frames and then costs nothing: two directions per
    // frame, static geometry only (the same rigid set as the cached light tier).
    {
        uint64_t key=Hash(0,&input.occlusion,sizeof(const void*));key=Hash(key,&low,sizeof(low));key=Hash(key,&high,sizeof(high));
        if(key!=skyKey){skyKey=key;skyDone=0;}
        const bool skyWanted=skyIdleFrames<SkyVisibilityIdleFrames;
        if(skyWanted)++skyIdleFrames;
        else if(skyArrayView){skyArray.Reset();skyArrayView.Reset();skyDepth.Reset();skyDsv.Reset();skyDone=0;}
        if(skyWanted&&skyDone<SkyVisibilityDirections&&!skyArrayView){
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
    if(litBlocksRecorded)litBounds.pending=true;
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
} // namespace Filmmaker::Environment
