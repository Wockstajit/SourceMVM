#define NOMINMAX
#include "RelightProbes.h"
#include "RelightProbeGiShaders.h"
#include "../Environment/EnvironmentShadowMaps.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Debug/GpuPassProfiler.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <new>
namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT Voxels=GiVoxelResolution,Probes=GiProbeResolution;
// Background sweep plus a 16-probe-wide neighbourhood refreshed every frame.
// Authored edits temporarily double background work, without lowering ray quality.
constexpr UINT ProbeSlicesPerFrame=4,EditedProbeSlicesPerFrame=8;
constexpr UINT EditedProbeFrames=3*Probes/EditedProbeSlicesPerFrame;
// World-anchored quadrature is deterministic: a long noise history only delays
// moving occluders and refreshed shadow tiles. Publish each deterministic update directly;
// sparse background probes must not need multiple sweeps to catch up with current lighting.
constexpr float HistoryBlend=1.f;
// Static map voxelization is spread over a few frames at map load instead of one hitch.
constexpr UINT StaticTrianglesPerFrame=2000000;
constexpr UINT StaticMaxDimension=512;
// Three background sweeps settle the static-map solution. Reuse it while the
// camera and authored lighting are unchanged; animated direct shadows stay live.
constexpr UINT StableTransportFrames=48;
bool g_unboundedProbeBox=false;
struct Volume {
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Constants {ProbeRow lowSize,grid,giLowSize,giInfo,sky,ground,staticOffset,staticDims,probeInfo,shift,voxelTarget,voxelDims,reflection,visibleInfo,reflectionHistory;};
constexpr UINT ReflectionSize=GiReflectionSize;
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> work;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11PixelShader> staticPs;
    ComPtr<ID3D11ComputeShader> inject,trace,shift,compose,reflect,reflectNorm,markVisible;
    // Live reflection cube (six faces, full mips) and its normalization row.
    ComPtr<ID3D11Texture2D> reflection;ComPtr<ID3D11ShaderResourceView> reflectionSrv;ComPtr<ID3D11UnorderedAccessView> reflectionUav;
    ComPtr<ID3D11Buffer> reflectionNorm;ComPtr<ID3D11UnorderedAccessView> reflectionNormUav;UINT reflectionMips=0;Vec3 reflectionOrigin{};
    ComPtr<ID3D11Texture2D> reflectionHistory;ComPtr<ID3D11ShaderResourceView> reflectionHistorySrv;
    ComPtr<ID3D11Buffer> reflectionNormHistory;ComPtr<ID3D11ShaderResourceView> reflectionNormHistorySrv;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> params,draw,records,cb2,brickOffsets,brickRecords,visibleList,visibleDispatch;
    ComPtr<ID3D11ShaderResourceView> visibleListSrv;ComPtr<ID3D11UnorderedAccessView> visibleListUav;
    ComPtr<ID3D11ShaderResourceView> recordsSrv,brickOffsetsSrv,brickRecordsSrv;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> sampler,receiverSampler;
    ComPtr<ID3D11Buffer> cameraReadback;
    bool cameraReadPending=false;ULONGLONG cameraReadTime=0;
    UINT stableTransportFrames=0;Vec3 transportCamera{};
    float transportBounce=-1,transportReflections=-1;
    unsigned transportSteps=0;int transportDiagnostic=-1;
    Vec3 cameraReadOrigin{},cameraReadSky{};float cameraReadBounce=0;
    uint64_t cameraReadFrame=0;
    Volume staticVoxels,radiance,backRadiance,direction,history[2],distance[2],composed,visibleMask;
    // Static build inputs, released once the map is voxelized.
    ComPtr<ID3D11Buffer> staticVertices,staticIndices,staticAlbedo;ComPtr<ID3D11ShaderResourceView> staticAlbedoSrv;
    UINT fastHistoryUntil=0;uint64_t authoredKey=0;
    UINT staticTriangles=0,staticDone=0,updates=0;UINT staticDims[3]{};Vec3 staticLow{};
    std::shared_ptr<const Environment::OcclusionMesh> world,staticFailedWorld;
    ProbePacking packing{};Constants constants{};
    GiRegion region{};bool hasRegion=false;
    UINT current=0;
    uint64_t frame=~0ull,visibleFrame=~0ull,recordsKey=0;UINT recordCount=0;
    std::vector<LightRecord> uploadedRecords;
    bool ready=false,initialized=false,failed=false;
} state;
bool Buffer(UINT size,UINT bind,ComPtr<ID3D11Buffer>& out,const void* data=nullptr,UINT stride=0) {
    D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.BindFlags=bind;d.Usage=data?D3D11_USAGE_IMMUTABLE:D3D11_USAGE_DEFAULT;
    d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
    D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;
    return SUCCEEDED(state.device->CreateBuffer(&d,data?&initial:nullptr,out.ReleaseAndGetAddressOf()));
}
bool Structured(UINT count,UINT stride,const void* data,ComPtr<ID3D11Buffer>& buffer,ComPtr<ID3D11ShaderResourceView>& srv) {
    return Buffer(count*stride,D3D11_BIND_SHADER_RESOURCE,buffer,data,stride)&&
        SUCCEEDED(state.device->CreateShaderResourceView(buffer.Get(),nullptr,srv.ReleaseAndGetAddressOf()));
}
bool Texture(Volume& out,UINT width,UINT height,UINT depth,DXGI_FORMAT format,bool mips=false) {
    out=Volume{};
    D3D11_TEXTURE3D_DESC d{};d.Width=width;d.Height=height;d.Depth=depth;d.MipLevels=mips?0:1;d.Format=format;
    d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    if(mips){d.BindFlags|=D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;}
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=format;u.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE3D;u.Texture3D.WSize=depth;
    return SUCCEEDED(state.device->CreateTexture3D(&d,nullptr,&out.texture))&&
        SUCCEEDED(state.device->CreateShaderResourceView(out.texture.Get(),nullptr,&out.srv))&&
        SUCCEEDED(state.device->CreateUnorderedAccessView(out.texture.Get(),&u,&out.uav));
}
bool Compile(const char* entry,const char* profile,ComPtr<ID3DBlob>& code) {
    ComPtr<ID3DBlob> errors;
    if(SUCCEEDED(Environment::CompileEnvironmentShader(kProbeGiShader,strlen(kProbeGiShader),"RelightProbeGi",nullptr,
        entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&errors)))return true;
    MvmDebugLog_LinefAlways("relight.probe.gi","shader=%s failure=%s",entry,errors?static_cast<const char*>(errors->GetBufferPointer()):"compiler unavailable");
    return false;
}
bool Initialize() {
    if(state.initialized)return true;if(state.failed)return false;state.failed=true;
    ComPtr<ID3DBlob> vs,gs,staticPs,inject,trace,shift,compose,reflect,reflectNorm,markVisible;
    if(!Compile("VoxelVS","vs_5_0",vs)||!Compile("VoxelGS","gs_5_0",gs)||!Compile("VoxelStaticPS","ps_5_0",staticPs)||
       !Compile("InjectCS","cs_5_0",inject)||!Compile("ProbeCS","cs_5_0",trace)||
       !Compile("ShiftCS","cs_5_0",shift)||!Compile("ComposeCS","cs_5_0",compose)||
       !Compile("ReflectionCS","cs_5_0",reflect)||!Compile("ReflectionNormCS","cs_5_0",reflectNorm)||!Compile("MarkVisibleCS","cs_5_0",markVisible))return false;
    auto d=state.device.Get();
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    if(FAILED(d->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.vs))||
       FAILED(d->CreateGeometryShader(gs->GetBufferPointer(),gs->GetBufferSize(),nullptr,&state.gs))||
       FAILED(d->CreatePixelShader(staticPs->GetBufferPointer(),staticPs->GetBufferSize(),nullptr,&state.staticPs))||
       FAILED(d->CreateComputeShader(inject->GetBufferPointer(),inject->GetBufferSize(),nullptr,&state.inject))||
       FAILED(d->CreateComputeShader(trace->GetBufferPointer(),trace->GetBufferSize(),nullptr,&state.trace))||
       FAILED(d->CreateComputeShader(shift->GetBufferPointer(),shift->GetBufferSize(),nullptr,&state.shift))||
       FAILED(d->CreateComputeShader(compose->GetBufferPointer(),compose->GetBufferSize(),nullptr,&state.compose))||
       FAILED(d->CreateComputeShader(reflect->GetBufferPointer(),reflect->GetBufferSize(),nullptr,&state.reflect))||
       FAILED(d->CreateComputeShader(reflectNorm->GetBufferPointer(),reflectNorm->GetBufferSize(),nullptr,&state.reflectNorm))||
       FAILED(d->CreateComputeShader(markVisible->GetBufferPointer(),markVisible->GetBufferSize(),nullptr,&state.markVisible))||
       FAILED(d->CreateInputLayout(&element,1,vs->GetBufferPointer(),vs->GetBufferSize(),&state.layout))||
       FAILED(d->CreateDeferredContext(0,&state.work)))return false;
    std::array<ProbeRow,3600> rows{};
    D3D11_BUFFER_DESC cb2{};cb2.ByteWidth=sizeof(rows);cb2.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{rows.data(),0,0};
    if(!Buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,state.params)||
       !Buffer(sizeof(Environment::Matrix),D3D11_BIND_CONSTANT_BUFFER,state.draw)||
       FAILED(d->CreateBuffer(&cb2,&initial,&state.cb2))||
       !Texture(state.radiance,Voxels,Voxels,Voxels,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.backRadiance,Voxels,Voxels,Voxels,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.direction,Voxels,Voxels,Voxels,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.history[0],Probes,Probes,Probes*6,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.history[1],Probes,Probes,Probes*6,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.distance[0],Probes,Probes,Probes*6,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.distance[1],Probes,Probes,Probes*6,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.composed,Probes,Probes,Probes*6,DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !Texture(state.visibleMask,Probes,Probes,Probes,DXGI_FORMAT_R32_UINT))return false;
    {
        D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=ReflectionSize;t.MipLevels=0;t.ArraySize=6;t.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
        t.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if(FAILED(d->CreateTexture2D(&t,nullptr,&state.reflection)))return false;
        state.reflection->GetDesc(&t);state.reflectionMips=t.MipLevels;
        D3D11_TEXTURE2D_DESC history=t;history.BindFlags=D3D11_BIND_SHADER_RESOURCE;history.MiscFlags=0;
        D3D11_SHADER_RESOURCE_VIEW_DESC hv{};hv.Format=t.Format;hv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        hv.Texture2DArray.MipLevels=1;hv.Texture2DArray.ArraySize=6;
        if(FAILED(d->CreateTexture2D(&history,nullptr,&state.reflectionHistory))||
           FAILED(d->CreateShaderResourceView(state.reflectionHistory.Get(),&hv,&state.reflectionHistorySrv)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=t.Format;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        v.TextureCubeArray.MipLevels=t.MipLevels;v.TextureCubeArray.NumCubes=1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=t.Format;u.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;u.Texture2DArray.ArraySize=6;
        if(FAILED(d->CreateShaderResourceView(state.reflection.Get(),&v,&state.reflectionSrv))||
           FAILED(d->CreateUnorderedAccessView(state.reflection.Get(),&u,&state.reflectionUav)))return false;
        D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ru{};ru.Format=DXGI_FORMAT_R32_TYPELESS;ru.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        ru.Buffer.NumElements=4;ru.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(d->CreateBuffer(&b,nullptr,&state.reflectionNorm))||
           FAILED(d->CreateUnorderedAccessView(state.reflectionNorm.Get(),&ru,&state.reflectionNormUav)))return false;
        b.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC nv{};nv.Format=DXGI_FORMAT_R32_TYPELESS;nv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        nv.BufferEx.NumElements=4;nv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(d->CreateBuffer(&b,nullptr,&state.reflectionNormHistory))||
           FAILED(d->CreateShaderResourceView(state.reflectionNormHistory.Get(),&nv,&state.reflectionNormHistorySrv)))return false;
    }
    {
        D3D11_BUFFER_DESC list{};list.ByteWidth=16+Probes*Probes*Probes*4;
        list.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        list.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements=list.ByteWidth/4;srv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=srv.Format;uav.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements=list.ByteWidth/4;uav.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_BUFFER_DESC args{};args.ByteWidth=12;args.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if(FAILED(d->CreateBuffer(&list,nullptr,&state.visibleList))||
           FAILED(d->CreateShaderResourceView(state.visibleList.Get(),&srv,&state.visibleListSrv))||
           FAILED(d->CreateUnorderedAccessView(state.visibleList.Get(),&uav,&state.visibleListUav))||
           FAILED(d->CreateBuffer(&args,nullptr,&state.visibleDispatch)))return false;
    }
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(d->CreateRasterizerState(&raster,&state.raster))||FAILED(d->CreateDepthStencilState(&depth,&state.depth))||
       FAILED(d->CreateSamplerState(&sampler,&state.sampler)))return false;
    // Receiver queries already gather and weight individual cage corners. Filtering
    // those texel-centre reads again wastes texture bandwidth and blends across bands.
    // Reflection cones retain their separate linear/mip sampler.
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    if(FAILED(d->CreateSamplerState(&sampler,&state.receiverSampler)))return false;
    state.initialized=true;state.failed=false;return true;
}
// Whole-map voxels from the actual render geometry and its material albedo, built once per map.
bool StaticGeometry(const std::shared_ptr<const Environment::OcclusionMesh>& world,ID3D11DeviceContext* immediate) {
    if(state.world==world)return true;
    if(state.staticFailedWorld==world)return false;
    state.ready=false;state.hasRegion=false;state.world.reset();state.staticFailedWorld=world;
    auto releaseInputs=[](){
        state.staticVertices.Reset();state.staticIndices.Reset();state.staticAlbedo.Reset();state.staticAlbedoSrv.Reset();
        state.staticVoxels=Volume{};
    };
    releaseInputs();
    const auto* visual=world->visual&&world->visual->renderGeometry?world->visual.get():nullptr;
    const auto& positions=visual?visual->rigidPositions:world->positions;
    const auto& indices=visual?visual->rigidIndices:world->indices;
    size_t vertexCount=positions.size(),indexCount=indices.size();
    if(indices.size()%3||(visual&&visual->rigidAlbedo.size()!=indices.size()/3))return false;
    Vec3 low=world->low,high=world->high;
    if(visual){
        for(const auto& batch:visual->batches){
            if(batch.indices.size()%3)return false;
            vertexCount+=batch.vertices.size();indexCount+=batch.indices.size();
        }
        low={(std::min)(low.x,visual->low.x),(std::min)(low.y,visual->low.y),(std::min)(low.z,visual->low.z)};
        high={(std::max)(high.x,visual->high.x),(std::max)(high.y,visual->high.y),(std::max)(high.z,visual->high.z)};
    }
    if(vertexCount<3||indexCount<3||vertexCount>UINT_MAX/sizeof(Vec3)||indexCount>UINT_MAX/sizeof(uint32_t))return false;
    const UINT triangles=UINT(indexCount/3);
    // Aligned to the cell so region cells map onto static cells by an integer offset.
    const float cell=GiCell;
    state.staticLow={std::floor(low.x/cell)*cell-cell,std::floor(low.y/cell)*cell-cell,std::floor(low.z/cell)*cell-cell};
    const float extent[3]{high.x-state.staticLow.x,high.y-state.staticLow.y,high.z-state.staticLow.z};
    for(unsigned axis=0;axis<3;++axis)state.staticDims[axis]=(std::min)(StaticMaxDimension,UINT(std::ceil(extent[axis]/cell))+2);
    state.constants.staticDims={float(state.staticDims[0]),float(state.staticDims[1]),float(state.staticDims[2]),0};
    // Upload borrowed rigid/collision arrays directly. Convert only a bounded slice of each
    // interleaved batch, instead of copying the entire map and growing three CPU vectors.
    // Use the immediate context: deferred updates would retain all slice payloads in a list.
    constexpr size_t UploadElements=16384;
    try {
        std::vector<Vec3> positionSlice(visual&&!visual->batches.empty()?UploadElements:0);
        std::vector<uint32_t> wordSlice(UploadElements);
        if(!Buffer(UINT(vertexCount*sizeof(Vec3)),D3D11_BIND_VERTEX_BUFFER,state.staticVertices)||
           !Buffer(UINT(indexCount*sizeof(uint32_t)),D3D11_BIND_INDEX_BUFFER,state.staticIndices)||
           !Structured(triangles,4,nullptr,state.staticAlbedo,state.staticAlbedoSrv)||
           !Texture(state.staticVoxels,state.staticDims[0],state.staticDims[1],state.staticDims[2],DXGI_FORMAT_R32_UINT)){
            releaseInputs();return false;
        }
        auto upload=[&](ID3D11Buffer* buffer,size_t offset,size_t bytes,const void* data){
            if(!bytes)return;
            const D3D11_BOX box{UINT(offset),0,0,UINT(offset+bytes),1,1};
            immediate->UpdateSubresource(buffer,0,&box,data,0,0);
        };
        upload(state.staticVertices.Get(),0,positions.size()*sizeof(Vec3),positions.data());
        upload(state.staticIndices.Get(),0,indices.size()*4,indices.data());
        if(visual)upload(state.staticAlbedo.Get(),0,visual->rigidAlbedo.size()*4,visual->rigidAlbedo.data());
        else {
            std::fill(wordSlice.begin(),wordSlice.end(),0x808080u);
            for(size_t at=0;at<triangles;at+=UploadElements)
                upload(state.staticAlbedo.Get(),at*4,(std::min)(UploadElements,size_t(triangles)-at)*4,wordSlice.data());
        }
        size_t vertexBase=positions.size(),indexBase=indices.size();
        if(visual)for(const auto& batch:visual->batches){
            for(size_t at=0;at<batch.vertices.size();at+=UploadElements){
                const size_t count=(std::min)(UploadElements,batch.vertices.size()-at);
                for(size_t i=0;i<count;++i)positionSlice[i]=batch.vertices[at+i].position;
                upload(state.staticVertices.Get(),(vertexBase+at)*sizeof(Vec3),count*sizeof(Vec3),positionSlice.data());
            }
            for(size_t at=0;at<batch.indices.size();at+=UploadElements){
                const size_t count=(std::min)(UploadElements,batch.indices.size()-at);
                for(size_t i=0;i<count;++i)wordSlice[i]=UINT(vertexBase)+batch.indices[at+i];
                upload(state.staticIndices.Get(),(indexBase+at)*4,count*4,wordSlice.data());
            }
            // Cutout and animated batches are foliage, grilles and fences: porous to light.
            const uint32_t porous=(batch.cutoff>0||batch.animation.Active())?0x01000000u:0u;
            std::fill(wordSlice.begin(),wordSlice.end(),batch.albedo|porous);
            const size_t batchTriangles=batch.indices.size()/3;
            for(size_t at=0;at<batchTriangles;at+=UploadElements)
                upload(state.staticAlbedo.Get(),(indexBase/3+at)*4,(std::min)(UploadElements,batchTriangles-at)*4,wordSlice.data());
            vertexBase+=batch.vertices.size();indexBase+=batch.indices.size();
        }
    }catch(const std::bad_alloc&){
        releaseInputs();MvmDebugLog_LinefAlways("relight.probe.gi","static geometry allocation failed; GI unavailable for this map instance");return false;
    }
    const UINT zero[4]{};state.work->ClearUnorderedAccessViewUint(state.staticVoxels.uav.Get(),zero);
    state.staticTriangles=triangles;state.staticDone=0;
    state.world=world;state.staticFailedWorld.reset();
    if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.probe.gi","static triangles=%u dims=%ux%ux%u cell=%.0f cpuScratchBytes=%zu",
        state.staticTriangles,state.staticDims[0],state.staticDims[1],state.staticDims[2],cell,
        UploadElements*(sizeof(uint32_t)+(visual&&!visual->batches.empty()?sizeof(Vec3):0)));
    return true;
}
void Upload(ID3D11DeviceContext* work) {work->UpdateSubresource(state.params.Get(),0,nullptr,&state.constants,0,0);}
void BindVoxelizer(ID3D11DeviceContext* work,ID3D11PixelShader* ps,ID3D11UnorderedAccessView* target,UINT raster) {
    auto cb=state.params.Get();work->VSSetConstantBuffers(0,1,&cb);work->GSSetConstantBuffers(0,1,&cb);work->PSSetConstantBuffers(0,1,&cb);
    const auto draw=Environment::Identity();
    auto object=state.draw.Get();work->UpdateSubresource(object,0,nullptr,&draw,0,0);
    work->VSSetConstantBuffers(1,1,&object);work->PSSetConstantBuffers(1,1,&object);
    work->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,&target,nullptr);
    work->RSSetState(state.raster.Get());work->OMSetDepthStencilState(state.depth.Get(),0);
    const D3D11_VIEWPORT vp{0,0,float(raster),float(raster),0,1};work->RSSetViewports(1,&vp);
    work->VSSetShader(state.vs.Get(),nullptr,0);work->GSSetShader(state.gs.Get(),nullptr,0);work->PSSetShader(ps,nullptr,0);
    work->IASetInputLayout(state.layout.Get());work->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void Unbind(ID3D11DeviceContext* work) {
    ID3D11UnorderedAccessView* none[8]{};ID3D11ShaderResourceView* views[22]{};
    work->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,none,nullptr);
    work->CSSetUnorderedAccessViews(0,8,none,nullptr);work->CSSetShaderResources(0,22,views);
    work->GSSetShader(nullptr,nullptr,0);work->PSSetShaderResources(0,16,views);
}
// Rows shared by the volume the material reads and the tracing that writes it.
void SetRegion(const GiRegion& region) {
    const auto low=region.low;const float size=region.size;
    state.constants.lowSize={low.x,low.y,low.z,size};state.constants.giLowSize=state.constants.lowSize;
    state.constants.grid[0]=float(Voxels);state.constants.grid[1]=size/Voxels;
    state.constants.giInfo[0]=1;state.constants.giInfo[1]=size/Voxels;state.constants.giInfo[2]=float(Voxels);
    for(unsigned axis=0;axis<3;++axis)
        state.constants.staticOffset[axis]=std::round(((&low.x)[axis]-(&state.staticLow.x)[axis])/GiCell);
    state.packing=PackProbe(low,low+Vec3{size,size,size},Probes);
    // Diagnostic (probecheck 4 in the frame): unbounded record box; sampling clamps to the border.
    if(g_unboundedProbeBox)for(unsigned axis=0;axis<3;++axis){state.packing.probe[3][axis]=-1000;state.packing.probe[4][axis]=1001;}
    // A constant buffer can only be updated whole: a partial UpdateSubresource box is invalid for
    // one and is dropped, which had left cb2 all zeros, so every probe-lit program (props, the
    // first-person weapon and hands) read zero ambient and zero bounce and rendered black.
    static std::array<ProbeRow,3600> rows{};rows.fill({});
    std::copy(state.packing.volume.begin(),state.packing.volume.end(),rows.begin());
    state.work->UpdateSubresource(state.cb2.Get(),0,nullptr,rows.data(),0,0);
    state.region=region;
}
bool Rebin(const std::vector<LightRecord>& records) {
    const auto bricks=BinGiLights(records,state.region);
    return Structured(UINT(bricks.offsets.size()),4,bricks.offsets.data(),state.brickOffsets,state.brickOffsetsSrv)&&
           Structured(UINT(bricks.records.size()),4,bricks.records.data(),state.brickRecords,state.brickRecordsSrv);
}
ProbeBinding CurrentBinding(const Scene& scene,Vec3 low,Vec3 high) {
    const auto neutral=NeutralProbe(state.device.Get(),low,high);
    ProbeBinding out{state.cb2.Get(),state.composed.srv.Get(),neutral.sunOcclusion,state.packing,state.receiverSampler.Get()};
    out.distance=state.distance[state.current].srv.Get();
    if(scene.reflections>0){out.reflection=state.reflectionSrv.Get();out.reflectionNorm=state.reflectionNorm.Get();
        out.reflectionMips=state.reflectionMips;out.reflectionOrigin=state.reflectionOrigin;}
    return out;
}

}
void MarkVisibleProbeGi(ID3D11DeviceContext* immediate,
    ID3D11ShaderResourceView* positions,UINT width,UINT height,uint64_t frame) {
    if(!state.ready||state.frame!=frame||!positions||!width||!height)return;
    if(state.stableTransportFrames>=StableTransportFrames)return;
    if(state.visibleFrame==frame)return;
    auto work=state.work.Get();work->ClearState();
    const auto saved=state.constants;
    state.constants.visibleInfo={float(width),float(height),2,0};
    state.constants.ground[1]=4;Upload(work);
    auto cb=state.params.Get();work->CSSetConstantBuffers(0,1,&cb);
    const UINT zero[4]{};work->ClearUnorderedAccessViewUint(state.visibleMask.uav.Get(),zero);
    work->ClearUnorderedAccessViewUint(state.visibleListUav.Get(),zero);
    const UINT args[4]{0,6,1,0};const D3D11_BOX header{0,0,0,16,1,1};
    work->UpdateSubresource(state.visibleList.Get(),0,&header,args,0,0);
    auto listOutput=state.visibleListUav.Get();work->CSSetUnorderedAccessViews(7,1,&listOutput,nullptr);
    work->CSSetShaderResources(16,1,&positions);
    auto mask=state.visibleMask.uav.Get();work->CSSetUnorderedAccessViews(0,1,&mask,nullptr);
    work->CSSetShader(state.markVisible.Get(),nullptr,0);
    const D3D11_BOX dispatchBox{0,0,0,12,1,1};
    {GPU_PASS_SCOPE(work,"gi: visible probe mask");
     work->Dispatch((width+15)/16,(height+15)/16,1);
     Unbind(work);
     work->CopySubresourceRegion(state.visibleDispatch.Get(),0,0,0,0,state.visibleList.Get(),0,&dispatchBox);}
    // Retain the receiver list for the next PrepareProbeGi. Never mutate transport,
    // visibility or normalization while native materials are consuming this frame.
    state.constants=saved;Upload(work);
    ComPtr<ID3D11CommandList> list;if(FAILED(work->FinishCommandList(FALSE,&list)))return;
    immediate->ExecuteCommandList(list.Get(),TRUE);state.visibleFrame=frame;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>3000){last=now;
        MvmDebugLog_LinefAlways("relight.probe.gi.visible","frame=%llu depth=%ux%u stride=2 priority=next-frame region=(%.0f %.0f %.0f)",
            frame,width,height,state.region.low.x,state.region.low.y,state.region.low.z);}}
}
void EndProbeGi(){state=State{};}
ProbeBinding PrepareProbeGi(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<const Environment::OcclusionMesh>& world,const Scene& scene,
    const std::vector<LightRecord>& records,ID3D11ShaderResourceView* shadow,ID3D11SamplerState* comparison,uint64_t frame,unsigned residentStaticLights,Vec3 camera,ID3D11ShaderResourceView* glassCookies,Vec3 viewForward) {
    if(state.device.Get()!=device){EndProbeGi();state.device=device;}
    if(g_unboundedProbeBox!=(scene.probeCheck==4)){g_unboundedProbeBox=scene.probeCheck==4;state.hasRegion=false;}
    if(!world||records.empty()||!Initialize()||!StaticGeometry(world,immediate))return {};
    if(state.frame!=frame) {
        state.frame=frame;
        auto work=state.work.Get();
        if(state.staticDone<state.staticTriangles){
            const UINT count=(std::min)(StaticTrianglesPerFrame,state.staticTriangles-state.staticDone);
            UINT maxDim=(std::max)({state.staticDims[0],state.staticDims[1],state.staticDims[2]});
            state.constants.grid[1]=GiCell;
            state.constants.voxelTarget={state.staticLow.x,state.staticLow.y,state.staticLow.z,maxDim*GiCell};
            state.constants.voxelDims={float(state.staticDims[0]),float(state.staticDims[1]),float(state.staticDims[2]),float(state.staticDone)};
            Upload(work);
            BindVoxelizer(work,state.staticPs.Get(),state.staticVoxels.uav.Get(),maxDim*2);
            auto albedo=state.staticAlbedoSrv.Get();work->PSSetShaderResources(10,1,&albedo);
            UINT stride=sizeof(Vec3),offset=0;auto vb=state.staticVertices.Get();work->IASetVertexBuffers(0,1,&vb,&stride,&offset);
            work->IASetIndexBuffer(state.staticIndices.Get(),DXGI_FORMAT_R32_UINT,0);
            work->DrawIndexed(count*3,state.staticDone*3,0);
            Unbind(work);
            state.staticDone+=count;
            if(state.staticDone>=state.staticTriangles){
                state.staticVertices.Reset();state.staticIndices.Reset();state.staticAlbedo.Reset();state.staticAlbedoSrv.Reset();
                if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.probe.gi","static voxelization complete triangles=%u",state.staticTriangles);
            }
            ComPtr<ID3D11CommandList> commands;
            if(SUCCEEDED(work->FinishCommandList(FALSE,&commands))){GPU_PASS_SCOPE(immediate,"gi: static voxelization");immediate->ExecuteCommandList(commands.Get(),TRUE);}
            return {};
        }
        // Region: follows the camera in whole-probe steps; the history moves with it.
        const auto region=ViewGiRegion(camera,viewForward);
        const uint64_t authored=ProbeLightingKey(scene);
        const bool authoredChanged=authored!=state.authoredKey;
        if(authoredChanged){state.authoredKey=authored;state.fastHistoryUntil=state.updates+EditedProbeFrames;}
        const uint64_t lighting=authored^(uint64_t(residentStaticLights)<<32)^(uint64_t(records.size())<<48);
        bool rebin=lighting!=state.recordsKey||!state.brickOffsets;
        const bool resized=!state.records||state.recordCount!=records.size();
        // Camera-dependent shadow budgets can move a light to another atlas tile
        // without changing any authored setting or the number of records.
        const bool changed=state.uploadedRecords.size()!=records.size()||
            std::memcmp(state.uploadedRecords.data(),records.data(),records.size()*sizeof(LightRecord))!=0;
        const bool cameraChanged=Dot(camera-state.transportCamera,camera-state.transportCamera)>1e-6f;
        const bool presentationChanged=scene.bounce!=state.transportBounce||scene.reflections!=state.transportReflections||
            scene.bounceSteps!=state.transportSteps||scene.probeCheck!=state.transportDiagnostic;
        if(rebin||changed||cameraChanged||presentationChanged)state.stableTransportFrames=0;
        state.transportCamera=camera;
        state.transportBounce=scene.bounce;state.transportReflections=scene.reflections;
        state.transportSteps=scene.bounceSteps;state.transportDiagnostic=scene.probeCheck;
        if(resized||changed){
            if(resized&&!Structured(UINT(records.size()),sizeof(LightRecord),nullptr,state.records,state.recordsSrv))return {};
            work->UpdateSubresource(state.records.Get(),0,nullptr,records.data(),0,0);
            state.uploadedRecords=records;state.recordCount=UINT(records.size());
            if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
                MvmDebugLog_LinefAlways("relight.probe.gi.records","count=%u resized=%u contentChanged=%u authoredChanged=%u",
                    state.recordCount,unsigned(resized),unsigned(changed),unsigned(lighting!=state.recordsKey));}}
        }
        state.recordsKey=lighting;
        const bool regionChanged=!state.hasRegion||!(region==state.region);
        if(!regionChanged&&state.ready&&state.stableTransportFrames>=StableTransportFrames){
            if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
                MvmDebugLog_LinefAlways("relight.probe.gi.cache","reused=1 settledFrames=%u camera=(%.1f %.1f %.1f) authored=%llu",
                    state.stableTransportFrames,camera.x,camera.y,camera.z,state.authoredKey);}}
            return CurrentBinding(scene,world->low,world->high);
        }
        if(regionChanged)state.stableTransportFrames=0;
        if(regionChanged){
            if(state.hasRegion){
                const auto moved=GiProbeShift(state.region,region);
                state.constants.shift={float(moved[0]),float(moved[1]),float(moved[2]),0};Upload(work);
                auto cb=state.params.Get();work->CSSetConstantBuffers(0,1,&cb);
                auto input=state.history[state.current].srv.Get();work->CSSetShaderResources(11,1,&input);
                auto distances=state.distance[state.current].srv.Get();work->CSSetShaderResources(15,1,&distances);
                auto movedDistance=state.distance[1-state.current].uav.Get();work->CSSetUnorderedAccessViews(3,1,&movedDistance,nullptr);
                auto output=state.history[1-state.current].uav.Get();work->CSSetUnorderedAccessViews(4,1,&output,nullptr);
                work->CSSetShader(state.shift.Get(),nullptr,0);
                {GPU_PASS_SCOPE(work,"gi: region shift");work->Dispatch(Probes/8,Probes/8,Probes*6);}
                Unbind(work);state.current=1-state.current;
            }else{
                const float unknown[4]{0,0,0,-1};work->ClearUnorderedAccessViewFloat(state.history[state.current].uav.Get(),unknown);
                const float empty[4]{};work->ClearUnorderedAccessViewFloat(state.distance[state.current].uav.Get(),empty);
            }
            SetRegion(region);state.hasRegion=true;rebin=true;
        }
        if(rebin&&!Rebin(records))return {};
        const auto skyRadiance=SkyAmbient(scene,scene.indirect);
        state.constants.sky={skyRadiance.x,skyRadiance.y,skyRadiance.z,0};
        // Prioritize retained visible cages in this frame's single transport snapshot.
        // The depth-derived list is captured during the preceding frame.
        const bool visibleRetained=!regionChanged&&state.visibleFrame+1==frame;
        state.constants.ground={float(state.updates++%4096u),0,visibleRetained?1.f:0.f,0};
        ++state.stableTransportFrames;
        state.constants.giInfo[3]=scene.bounce;
        const bool edited=state.updates<=state.fastHistoryUntil;
        state.constants.grid[2]=float(records.size());state.constants.grid[3]=float(state.updates);
        state.constants.probeInfo={float(edited?EditedProbeSlicesPerFrame:ProbeSlicesPerFrame),HistoryBlend,GiFeedbackGain,edited?1.f:0.f};
        state.constants.voxelTarget={region.low.x,region.low.y,region.low.z,region.size};
        state.constants.voxelDims={float(Voxels),float(Voxels),float(Voxels),0};
        const auto captureDelta=camera-state.reflectionOrigin;
        const bool blendReflection=state.ready&&!authoredChanged&&!presentationChanged&&Dot(captureDelta,captureDelta)<GiProbeSpacing*GiProbeSpacing*16;
        state.constants.reflectionHistory={blendReflection?.8f:0,0,0,0};
        state.reflectionOrigin=StableReflectionOrigin(camera,state.reflectionOrigin,state.ready);
        const auto capture=state.reflectionOrigin;
        state.constants.reflection={capture.x,capture.y,capture.z,float(ReflectionSize)};
        Upload(work);
        // Indirect occupancy belongs to the static map. Animated bodies and weapons
        // crossing coarse cells must not change probe visibility or the reflection capture.
        auto cb=state.params.Get();work->CSSetConstantBuffers(0,1,&cb);
        auto sampler=state.sampler.Get();work->CSSetSamplers(0,1,&sampler);work->CSSetSamplers(1,1,&comparison);
        // Each iteration injects the preceding probe solution and traces another
        // disjoint set of probe slices. Generate reflection mips after the final
        // injection and compose once; the one-step default retains its cost.
        const unsigned bounceSteps=std::clamp(scene.bounceSteps,1u,3u);
        for(unsigned bounceStep=0;bounceStep<bounceSteps;++bounceStep){
        if(bounceSteps>1){
            state.constants.grid[3]=float(state.updates*bounceSteps+bounceStep);
            state.constants.ground[0]=state.constants.grid[3];
            state.constants.ground[1]=0;
            Upload(work);
        }
        {
            ID3D11ShaderResourceView* srvs[10]{state.staticVoxels.srv.Get(),state.recordsSrv.Get(),shadow,nullptr,nullptr,nullptr,
                nullptr,state.brickOffsetsSrv.Get(),state.brickRecordsSrv.Get(),state.history[state.current].srv.Get()};
            work->CSSetShaderResources(0,10,srvs);
            work->CSSetShaderResources(14,1,&glassCookies);
            auto distances=state.distance[state.current].srv.Get();work->CSSetShaderResources(15,1,&distances);
            ID3D11UnorderedAccessView* uavs[]{state.radiance.uav.Get(),state.direction.uav.Get()};
            auto backOutput=state.backRadiance.uav.Get();work->CSSetUnorderedAccessViews(6,1,&backOutput,nullptr);
            work->CSSetUnorderedAccessViews(1,2,uavs,nullptr);work->CSSetShader(state.inject.Get(),nullptr,0);
            GPU_PASS_SCOPE(work,"gi: inject");
            work->Dispatch(Voxels/8,Voxels/8,Voxels);
            Unbind(work);
            // Surface rays share exact-cell occupancy and face radiance. Roughness
            // filtering belongs to the reflection cube's mip chain, not wall opacity.
        }
        {
            GPU_PASS_SCOPE(work,"gi: trace");
            work->CSSetConstantBuffers(0,1,&cb);work->CSSetSamplers(0,1,&sampler);
            // Ping-pong: untraced slices carry over by copy, traced ones blend old (SRV) into new (UAV).
            const UINT next=1-state.current;
            work->CopyResource(state.history[next].texture.Get(),state.history[state.current].texture.Get());
            work->CopyResource(state.distance[next].texture.Get(),state.distance[state.current].texture.Get());
            ID3D11ShaderResourceView* srvs[13]{state.staticVoxels.srv.Get(),nullptr,nullptr,state.radiance.srv.Get(),state.direction.srv.Get(),nullptr,
                nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,state.history[state.current].srv.Get()};
            work->CSSetShaderResources(0,13,srvs);
            auto backInput=state.backRadiance.srv.Get();work->CSSetShaderResources(19,1,&backInput);
            auto distances=state.distance[state.current].srv.Get();work->CSSetShaderResources(15,1,&distances);
            auto mask=state.visibleMask.srv.Get();work->CSSetShaderResources(17,1,&mask);
            auto distanceOutput=state.distance[next].uav.Get();work->CSSetUnorderedAccessViews(3,1,&distanceOutput,nullptr);
            auto uav=state.history[next].uav.Get();work->CSSetUnorderedAccessViews(4,1,&uav,nullptr);
            work->CSSetShader(state.trace.Get(),nullptr,0);
            state.constants.ground[1]=0;Upload(work);
            work->Dispatch(Probes/8,Probes/8,UINT(state.constants.probeInfo[0])*6);
            state.constants.ground[1]=1;Upload(work);
            work->Dispatch(2,2,16*6);
            if(regionChanged){
                state.constants.ground[1]=2;Upload(work);
                work->Dispatch(Probes/8,Probes/8,Probes*6);
            }
            if(visibleRetained){
                auto compact=state.visibleListSrv.Get();work->CSSetShaderResources(18,1,&compact);
                state.constants.ground[1]=4;Upload(work);
                work->DispatchIndirect(state.visibleDispatch.Get(),0);
            }
            Unbind(work);state.current=next;
        }
        }
        {
            GPU_PASS_SCOPE(work,"gi: compose");
            work->CSSetConstantBuffers(0,1,&cb);
            auto input=state.history[state.current].srv.Get();work->CSSetShaderResources(5,1,&input);
            // The filter follows the same relocated origins as tracing and visibility.
            auto distances=state.distance[state.current].srv.Get();work->CSSetShaderResources(15,1,&distances);
            auto staticVoxels=state.staticVoxels.srv.Get();
            work->CSSetShaderResources(0,1,&staticVoxels);
            auto output=state.composed.uav.Get();work->CSSetUnorderedAccessViews(5,1,&output,nullptr);
            work->CSSetShader(state.compose.Get(),nullptr,0);work->Dispatch(Probes/8,Probes/8,Probes);
            Unbind(work);
            // Diagnostic 3: the traced volume's layout and bindings with constant contents.
            if(scene.probeCheck==3){const float constant[4]{.5f,.5f,.5f,1};work->ClearUnorderedAccessViewFloat(state.composed.uav.Get(),constant);}
        }
        if(scene.reflections>0){
            GPU_PASS_SCOPE(work,"gi: reflection capture");
            // A retained capture point keeps voxel silhouettes and ambient normalization
            // stable during walking. Authored light edits still enter this frame.
            work->CSSetConstantBuffers(0,1,&cb);work->CSSetSamplers(0,1,&sampler);
            ID3D11ShaderResourceView* srvs[5]{nullptr,nullptr,nullptr,state.radiance.srv.Get(),state.direction.srv.Get()};
            work->CopyResource(state.reflectionHistory.Get(),state.reflection.Get());
            work->CopyResource(state.reflectionNormHistory.Get(),state.reflectionNorm.Get());
            auto history=state.reflectionHistorySrv.Get();work->CSSetShaderResources(20,1,&history);
            auto oldNorm=state.reflectionNormHistorySrv.Get();work->CSSetShaderResources(21,1,&oldNorm);
            work->CSSetShaderResources(0,5,srvs);
            auto backInput=state.backRadiance.srv.Get();work->CSSetShaderResources(19,1,&backInput);
            auto output=state.reflectionUav.Get();work->CSSetUnorderedAccessViews(6,1,&output,nullptr);
            work->CSSetShader(state.reflect.Get(),nullptr,0);work->Dispatch((ReflectionSize+7)/8,(ReflectionSize+7)/8,6);
            Unbind(work);
            work->GenerateMips(state.reflectionSrv.Get());
            auto composed=state.composed.srv.Get();work->CSSetShaderResources(13,1,&composed);
            work->CSSetShaderResources(21,1,&oldNorm);
            auto norm=state.reflectionNormUav.Get();work->CSSetUnorderedAccessViews(7,1,&norm,nullptr);
            work->CSSetShader(state.reflectNorm.Get(),nullptr,0);work->Dispatch(1,1,1);
            ID3D11ShaderResourceView* none=nullptr;work->CSSetShaderResources(13,1,&none);
            Unbind(work);
        }
        ComPtr<ID3D11CommandList> commands;if(FAILED(work->FinishCommandList(FALSE,&commands)))return {};
        {GPU_PASS_SCOPE(immediate,"gi (probe pass, total)");immediate->ExecuteCommandList(commands.Get(),TRUE);}
        // All exposed probes are traced on their first frame, including a camera cut.
        // Publish the complete first field immediately; never display unknown border cells.
        if(!state.ready){state.ready=true;
            if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.probe.gi","published resolution=%u region=%.0f slices/frame=%u blend=%.2f",Probes,region.size,ProbeSlicesPerFrame,HistoryBlend);}
    }
    // Debug readback must not flush/wait for the current lighting work. Retain the
    // staging buffer and its sampled inputs until a nonblocking map succeeds.
    if(MvmDebugLog_Active()&&state.ready&&scene.reflections>0&&state.reflectionNorm){
        const auto now=GetTickCount64();D3D11_MAPPED_SUBRESOURCE m{};
        if(state.cameraReadPending){
            const auto hr=immediate->Map(state.cameraReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
            if(SUCCEEDED(hr)){
                const auto f=static_cast<const float*>(m.pData);const auto p=state.cameraReadOrigin,s=state.cameraReadSky;
                MvmDebugLog_LinefAlways("relight.probe.gi.camera","sampleFrame=%llu camera=(%.0f %.0f %.0f) luminance=%.4f axis=(%.4f %.4f %.4f) sky=(%.3f %.3f %.3f) bounce=%.2f",
                    state.cameraReadFrame,p.x,p.y,p.z,f[3],f[0],f[1],f[2],s.x,s.y,s.z,state.cameraReadBounce);
                immediate->Unmap(state.cameraReadback.Get(),0);state.cameraReadPending=false;
            }else if(hr!=DXGI_ERROR_WAS_STILL_DRAWING)state.cameraReadPending=false;
        }
        if(!state.cameraReadPending&&now-state.cameraReadTime>=2000){
            D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(state.cameraReadback||SUCCEEDED(device->CreateBuffer(&d,nullptr,&state.cameraReadback))){
                immediate->CopyResource(state.cameraReadback.Get(),state.reflectionNorm.Get());
                state.cameraReadPending=true;state.cameraReadTime=now;state.cameraReadFrame=state.frame;
                state.cameraReadOrigin=state.reflectionOrigin;
                state.cameraReadSky={state.constants.sky[0],state.constants.sky[1],state.constants.sky[2]};state.cameraReadBounce=scene.bounce;
            }
        }
    }
    if(!state.ready)return {};
    return CurrentBinding(scene,world->low,world->high);
}
}
