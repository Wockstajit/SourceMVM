#define NOMINMAX
#include "EnvironmentNativeMaterialDetail.h"
#include "../Relight/RelightLiteGrid.h"
namespace Filmmaker::Environment {
std::atomic<bool> NativeAoRepairReuse{true};
std::atomic<bool> NativeRecordReuse{true};
std::atomic<bool> NativeFineSlices{true};
std::atomic<unsigned> NativeViewDumpRequests{0};
std::atomic<bool> NativeExposureRead{true};
namespace {
// The exposure CS2's tonemapper applies, as its per-view constants carry it: b1[18].x is the scale
// and .y its reciprocal (measured 2026-11-12 on 1.41.x by forcing mat_tonemap_force_scale to 0.5, 1
// and 2: .x followed it exactly, and read 1.089 under the engine's own adaptation in daylight;
// Automation/verify/find-engine-exposure.py repeats the measurement). Published only while the
// two lanes are reciprocal, so a moved layout reads as "unknown" and no light is cut.
inline constexpr unsigned NativeExposureRow=18;
std::atomic<float> g_nativeExposure{0};
std::atomic<unsigned long long> g_nativeExposureTick{0};
void PublishNativeExposure(float scale,float reciprocal){
    const bool valid=std::isfinite(scale)&&std::isfinite(reciprocal)&&scale>1e-3f&&scale<1e3f&&std::abs(scale*reciprocal-1)<1e-3f;
    g_nativeExposure.store(valid?scale:0.f,std::memory_order_relaxed);
    g_nativeExposureTick.store(GetTickCount64(),std::memory_order_relaxed);
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.exposure.engine","b1[%u]=(%.6g %.6g) valid=%u",NativeExposureRow,scale,reciprocal,unsigned(valid));}}
}
}
float NativeExposureScale(){
    // Not from a view that stopped drawing (a menu, a loading screen).
    const auto tick=g_nativeExposureTick.load(std::memory_order_relaxed);
    if(!NativeExposureRead.load(std::memory_order_relaxed)||!tick||GetTickCount64()-tick>1000)return 0;
    return g_nativeExposure.load(std::memory_order_relaxed);
}
using Microsoft::WRL::ComPtr;
using namespace NativeDetail;
ID3D11ShaderResourceView* NativeLiveReflection(unsigned* mips){
    // The same conditions under which the material draws are given this capture as their reflection.
    const auto& set=state.frame.relight;const auto& live=state.probeBinding;
    if(!set.enabled||!(state.probeTraced||set.lite)||set.reflections<=0||!live.reflection)return nullptr;
    if(mips)*mips=live.reflectionMips;
    return live.reflection;
}
namespace NativeDetail {
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
        D3D11_BUFFER_DESC d{};d.ByteWidth=112;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
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
    // CS2's exposure, once a frame from a world view.
    if(nativeViewport&&viewportDesc.ByteWidth>=(NativeExposureRow+1)*16&&viewport.MinDepth==0&&viewport.MaxDepth>.5f&&viewport.Width>=256){
        if(state.exposurePending){D3D11_MAPPED_SUBRESOURCE mapped{};
            if(SUCCEEDED(ctx->Map(state.exposureReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))){
                const auto* row=static_cast<const float*>(mapped.pData);const float scale=row[0],reciprocal=row[1];
                ctx->Unmap(state.exposureReadback.Get(),0);state.exposurePending=false;
                PublishNativeExposure(scale,reciprocal);
            }
        }
        if(!state.exposurePending&&state.exposureFrame!=state.probeFrame&&NativeExposureRead.load(std::memory_order_relaxed)){
            if(!state.exposureReadback){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                device->CreateBuffer(&d,nullptr,&state.exposureReadback);}
            if(state.exposureReadback){const D3D11_BOX row{NativeExposureRow*16,0,0,(NativeExposureRow+1)*16,1,1};
                ctx->CopySubresourceRegion(state.exposureReadback.Get(),0,0,0,0,nativeViewport.Get(),0,&row);
                state.exposurePending=true;state.exposureFrame=state.probeFrame;}
        }
    }
    if(state.viewDumpPending){
        // Both copies were issued together; the second is ready when the first is.
        D3D11_MAPPED_SUBRESOURCE mapped[2]{};
        if(SUCCEEDED(ctx->Map(state.viewDump[0].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped[0]))){
            const bool second=SUCCEEDED(ctx->Map(state.viewDump[1].Get(),0,D3D11_MAP_READ,0,&mapped[1]));
            for(unsigned b=0;b<2;++b){if(b&&!second)break;
                const auto* f=static_cast<const float*>(mapped[b].pData);const auto* u=static_cast<const UINT*>(mapped[b].pData);
                for(UINT row=0;row<state.viewDumpBytes[b]/16&&row<96;++row)
                    MvmDebugLog_LinefAlways("relight.view.dump","b%u[%u]=(%.9g %.9g %.9g %.9g) hex=(%08x %08x %08x %08x)",b?4u:1u,row,
                        f[row*4],f[row*4+1],f[row*4+2],f[row*4+3],u[row*4],u[row*4+1],u[row*4+2],u[row*4+3]);}
            ctx->Unmap(state.viewDump[0].Get(),0);if(second)ctx->Unmap(state.viewDump[1].Get(),0);
            state.viewDumpPending=false;
        }
    }else if(NativeViewDumpRequests.load(std::memory_order_relaxed)&&nativeViewport&&viewport.MinDepth==0&&viewport.MaxDepth>.5f&&viewport.Width>=256){
        NativeViewDumpRequests.fetch_sub(1,std::memory_order_relaxed);
        ID3D11Buffer* sources[2]{nativeViewport.Get(),view.Get()};const UINT bytes[2]{viewportDesc.ByteWidth,vd.ByteWidth};bool ready=true;
        for(unsigned b=0;b<2;++b){
            if(!state.viewDump[b]||state.viewDumpBytes[b]!=bytes[b]){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes[b];d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ready=ready&&SUCCEEDED(device->CreateBuffer(&d,nullptr,state.viewDump[b].ReleaseAndGetAddressOf()));state.viewDumpBytes[b]=bytes[b];}
        }
        if(ready){for(unsigned b=0;b<2;++b)ctx->CopyResource(state.viewDump[b].Get(),sources[b]);state.viewDumpPending=true;}
    }
    state.proofResourcesReady=false;state.clusterMode=0;state.probeBinding={};state.skyboxBinding={};
    std::vector<Relight::LightRecord> localRecords;
    const bool viewmodelPass=viewport.MaxDepth<=.1f+1e-6f;
    auto& recordCache=state.proofRecordCache[viewmodelPass?1:0];
    auto& records=relight?recordCache.records:localRecords;
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
        const bool reuseRecords=recordCache.valid&&NativeRecordReuse.load(std::memory_order_relaxed);
        if(reuseRecords){state.glassCookies=recordCache.glassCookies;++state.proofRecordReuses;}
        else{
        ++state.proofRecordBuilds;records.clear();
        records.reserve(set.lights.size()+
                        size_t(state.proofShadowLights+state.proofStaticLights)*Relight::ShadowFaces);
        // Lit bounds (RelightLitBoundsMath.h) for the world passes' cube records of static-tier lamps.
        // Not with a shadow reach: past it a lamp lights without its shadow map, walls or not.
        auto& litRows=recordCache.litBounds;litRows.clear();
        const bool litBoundsWanted=ShadowLitBounds.load(std::memory_order_relaxed)&&!viewmodelPass&&set.shadows&&set.shadowReach==0;
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
            // Shadow reach: receivers past it skip this light's shadow lookup (ShadowReachDepth).
            const float shadowReach=set.shadowRadiance>0&&!Relight::HasFixtureProjection(light)?
                Relight::LightVisibleReach(Relight::ToRecord(light,set.intensity),light.range,set.shadowRadiance):0.f;
            // One cube record instead of six face records (Relight::ToCubeShadowRecord).
            Relight::LightRecord cube{};
            if(set.cubeShadows&&!g_cubeShadowUnsupported.load(std::memory_order_relaxed)&&
               Relight::ToCubeShadowRecord(light,set.intensity,state.proofMatrices[first].data(),first,cube)){
                if(shadowReach>0&&shadowReach<light.range)cube[Relight::ShadowReachSlot]=Relight::ShadowReachDepth(cube,shadowReach);
                if(set.shadowReach<0)cube[Relight::ShadowReachSlot]=1e-7f; // diagnostic: no lookups
                records.push_back(cube);
                Vec3 litLow{},litHigh{};
                if(litBoundsWanted&&Relight::IsStaticFaceTile(first)&&
                   StaticLampLitBounds((first-Relight::StaticTile)/Relight::ShadowFaces,set.shadowBias,litLow,litHigh)){
                    litRows.resize(records.size());litRows.back()=Relight::ToLitBoundsRow({litLow,litHigh,true});}
                continue;
            }
            for(unsigned face=0;face<Relight::FixtureFaces(light);++face){
                records.push_back(Relight::ToShadowedRecord(light,set.intensity,
                    state.proofMatrices[first+face].data(),first+face));
                if(shadowReach>0&&shadowReach<light.range)
                    records.back()[Relight::ShadowReachSlot]=Relight::ShadowReachDepth(records.back(),shadowReach);
                if(set.shadowReach<0&&!Relight::HasFixtureProjection(light))records.back()[Relight::ShadowReachSlot]=1e-7f; // diagnostic: no lookups
            }
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
        // Zero emitters is a valid blackout. Keep ownership of the native material and
        // its baked-input suppression by binding one inert record instead of falling through.
        if(records.empty())records.emplace_back();
        if(set.cost==2)records.resize(1);
        if(!litRows.empty())litRows.resize(records.size());
        recordCache.glassCookies=state.glassCookies;recordCache.valid=true;
        }
    }else records.resize(7);
    const UINT recordCount=UINT(records.size());
    state.proofMaskWords=Relight::MaskWords(recordCount);
    float farDistance=10000;
    if(state.frame.occlusion) {
        const auto low=state.frame.occlusion->low,high=state.frame.occlusion->high;
        const auto delta=high-low;farDistance=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    }
    // Fine slices only with the owned mask builder: the native kernels (clusters 1 and 2) keep CS2's layout.
    const float sliceUnits=relight&&state.frame.relight.clusters==3&&NativeFineSlices.load(std::memory_order_relaxed)?
        Relight::ClusterFineSliceUnits:0.f;
    auto grid=Relight::MakeClusterGrid(UINT(viewport.Width),UINT(viewport.Height),recordCount,farDistance,128,128,sliceUnits);
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
        state.proofLitBounds.Reset();state.proofLitBoundsSrv.Reset();state.proofUploadedLitBounds.clear();state.proofLitBoundsUploaded=false;
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
    // The records' lit bounds for the cluster bounds pass: one row per record, uploaded when they change.
    static const std::vector<Relight::LitBoundsRow> noLitBounds;
    const auto& litRows=relight&&recordCache.litBounds.size()==recordCount?recordCache.litBounds:noLitBounds;
    bool litBoundsActive=false,litBoundsChanged=false;
    if(!litRows.empty()){
        if(!state.proofLitBounds){D3D11_BUFFER_DESC d{};d.ByteWidth=recordCount*UINT(sizeof(Relight::LitBoundsRow));
            d.StructureByteStride=sizeof(Relight::LitBoundsRow);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            if(FAILED(device->CreateBuffer(&d,nullptr,&state.proofLitBounds))||
               FAILED(device->CreateShaderResourceView(state.proofLitBounds.Get(),nullptr,&state.proofLitBoundsSrv))){
                state.proofLitBounds.Reset();state.proofLitBoundsSrv.Reset();}
            state.proofLitBoundsUploaded=false;}
        if(state.proofLitBounds){
            litBoundsActive=true;
            litBoundsChanged=!state.proofLitBoundsUploaded||state.proofUploadedLitBounds.size()!=litRows.size()||
                std::memcmp(state.proofUploadedLitBounds.data(),litRows.data(),litRows.size()*sizeof(litRows[0]))!=0;
            if(litBoundsChanged)work->UpdateSubresource(state.proofLitBounds.Get(),0,nullptr,litRows.data(),0,0);
        }
    }
    // Bounds that were in use and no longer are (the switch, a shadow reach) rebuild the lists too.
    if(litBoundsActive!=state.proofLitBoundsActive)litBoundsChanged=true;
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
        Relight::ClusterFloatRow nearby;Relight::ClusterUintRow nearbyList;Relight::ClusterFloatRow reach;};
    const bool firstPerson=viewport.MaxDepth<=.1f;
    const Params params{{grid.shift,grid.tilesX,grid.tilesY,grid.sliceCount},
        {recordCount,grid.lights.words,grid.lights.tiles,grid.lights.slices},
        {grid.sliceScale,float(grid.sliceCount-1),viewport.Width,viewport.Height},
        {clusterMode,grid.lights.ones,firstPerson?1u:0u,state.clusterSummaries?grid.size:0u},
        {state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,Relight::FirstPersonLightReach},
        {firstPerson?grid.lights.nearby:0u,0,0,0},
        {state.frame.relight.visibleRadiance,state.frame.relight.tightClusters?1.f:0.f,litBoundsActive?1.f:0.f,0}};
    const UINT fillGroups=(grid.TileCount()+grid.sliceCount+1+63)/64;
    std::vector<uint8_t> viewKey;
    auto key=[&](const void* data,size_t bytes){const auto* p=static_cast<const uint8_t*>(data);viewKey.insert(viewKey.end(),p,p+bytes);};
    key(sunMatrices,sizeof(sunMatrices));key(characterMatrices,sizeof(characterMatrices));key(rebaseParams,sizeof(rebaseParams));
    key(&params,sizeof(params));key(&viewport,sizeof(viewport));key(&clustered,sizeof(clustered));key(&engineClustered,sizeof(engineClustered));
    const bool gated=relight&&state.frame.relight.hoist&&state.proofViewGate.Prepare(device,work,view.Get(),nativeViewport.Get(),viewKey,
        {(recordCount+63)/64,clustered?(recordCount+63)/64:0u,clustered?fillGroups:0u},authoredChanged||litBoundsChanged,engineClustered?Relight::NativeClusterDispatches(grid):std::array<UINT,4>{});
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
        ID3D11ShaderResourceView* litView=litBoundsActive?state.proofLitBoundsSrv.Get():nullptr;work->CSSetShaderResources(2,1,&litView);
        work->CSSetShader(state.clusterBounds.Get(),nullptr,0);dispatch(1,(recordCount+63)/64);
        litView=nullptr;work->CSSetShaderResources(2,1,&litView);
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
    {GPU_PASS_SCOPE(ctx,"lights: rebase + clusters");ctx->ExecuteCommandList(commands.Get(),TRUE);}
    if(engineClustered)state.nativeClusters.Commit();
    if(gated)state.proofViewGate.Commit();
    if(authoredChanged){state.proofUploadedRecords=records;++state.proofAuthoredUploads;state.proofAuthoredBytes+=records.size()*sizeof(records[0]);}
    if(litBoundsActive&&litBoundsChanged){state.proofUploadedLitBounds=litRows;state.proofLitBoundsUploaded=true;}
    state.proofLitBoundsActive=litBoundsActive;
    if(relight){
        const Vec3 low=state.frame.occlusion?state.frame.occlusion->low:Vec3{-32768,-32768,-32768};
        const Vec3 high=state.frame.occlusion?state.frame.occlusion->high:Vec3{32768,32768,32768};
        state.probeBinding=Relight::AmbientProbe(device,low,high,Relight::AmbientFill(state.frame.relight));
        // Lite: the bounce grid once its first bake is done, the map-wide fill until then.
        state.liteGrid=false;
        if(state.frame.relight.lite&&state.frame.occlusion&&state.probeBinding.constants){
            const auto grid=Relight::PrepareLiteGrid(device,state.frame.occlusion,state.frame.relight);
            if(grid.constants){state.probeBinding=grid;state.liteGrid=true;}
        }else if(!state.frame.relight.lite&&state.liteReleasedProbes)Relight::EndLiteGrid();
        if(state.frame.relight.lite&&state.probeBinding.constants&&state.frame.relight.reflections>0)
            Relight::AddLiteReflection(device,state.frame.relight,state.frame.camera,state.probeBinding);
    }
    if(relight&&state.frame.relight.probeCheck==2&&state.frame.occlusion) {
        const auto margin=Vec3{32,32,32};
        state.probeBinding=Relight::NeutralProbe(device,state.frame.occlusion->low-margin,state.frame.occlusion->high+margin);
    }
    state.probeTraced=false;
    // Lite keeps no bounce volume: drop the traced one's memory when the mode is switched on.
    if(relight&&state.frame.relight.lite&&!state.liteReleasedProbes){Relight::EndProbeGi();Relight::EndScreenGi();}
    state.liteReleasedProbes=relight&&state.frame.relight.lite;
    if(relight&&state.frame.relight.gi&&!state.frame.relight.lite&&(!state.frame.relight.probeCheck||state.frame.relight.probeCheck>=3)&&state.frame.relight.cost==0) {
        const auto traced=Relight::PrepareProbeGi(device,ctx,state.frame.occlusion,state.frame.relight,
            records,state.proofAtlas.Get(),state.proofComparison.Get(),state.probeFrame,state.proofStaticLights,state.frame.camera,state.glassCookies,
            ViewForward(state.frame.angles));
        if(traced.constants){state.probeBinding=traced;state.probeTraced=true;}
        // Diagnostics 5/6: the traced binding with the ambient probe's texture, or with its constants.
        if(traced.constants&&state.frame.relight.probeCheck>=5){
            const Vec3 low=state.frame.occlusion?state.frame.occlusion->low:Vec3{-32768,-32768,-32768},high=state.frame.occlusion?state.frame.occlusion->high:Vec3{32768,32768,32768};
            const auto ambient=Relight::AmbientProbe(device,low,high,Relight::AmbientFill(state.frame.relight));
            if(state.frame.relight.probeCheck==5)state.probeBinding.irradiance=ambient.irradiance;
            else{state.probeBinding.constants=ambient.constants;state.probeBinding.packing=ambient.packing;}
        }
    }
    if(relight){
        const auto skybox=state.frame.occlusion?state.frame.occlusion->skybox:nullptr;
        const Vec3 low=skybox?skybox->low:Vec3{-32768,-32768,-32768},high=skybox?skybox->high:Vec3{32768,32768,32768};
        state.skyboxBinding=Relight::SkyboxAmbientProbe(device,low,high,Relight::AmbientFill(state.frame.relight));
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

} // namespace NativeDetail
void ResetNativeMaterial(){PixelStateLayer::Flush(PixelStateLayer::End::Reset);Relight::EndProbes();state=State{};}
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
            "generation=%llu uploads=%u records=%u bytes=%zu draws=%u worldDraws=%u hoist=%u cost=%u authoredUploads=%u recordBuilds=%u recordReuses=%u",
            state.proofPreparedGeneration,state.proofUploads,state.proofRecordCount,
            state.proofAuthoredBytes,state.draws,state.worldDraws,
            unsigned(state.frame.relight.hoist),state.frame.relight.cost,state.proofAuthoredUploads,
            state.proofRecordBuilds,state.proofRecordReuses);}
    }
    state.proofRecordBuilds=state.proofRecordReuses=0;
    if(MvmDebugLog_Active()){
        static ULONGLONG last=0;const auto now=GetTickCount64();
        if(now-last>3000){last=now;const auto stats=PixelStateLayer::TakeStats(true);
            if(stats.adopted||stats.reused){std::string ends;
                for(unsigned i=0;i<unsigned(PixelStateLayer::End::Count);++i)if(stats.ended[i]){
                    char part[48];sprintf_s(part," %s=%llu",PixelStateLayer::EndName(PixelStateLayer::End(i)),stats.ended[i]);ends+=part;}
                MvmDebugLog_LinefAlways("render.pixelstate","setups=%llu reused=%llu ended:%s",stats.adopted,stats.reused,ends.c_str());}}
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
namespace {
Sun FrameSun(const Frame& frame){
    auto climate=EffectiveClimate(frame.scene.climate);

    const auto direction=ClimateSunDirection(climate);Sun next{};next.direction[0]=direction.x;next.direction[1]=direction.y;next.direction[2]=direction.z;
    // Once the sun has set the key light is the moon (ClimateMoonKey): its colour at its power.
    if(ClimateMoonKey(climate)){
        const auto moon=ClimateSunRadiance(climate);
        next.radiance[0]=moon.x*climate[CL_sunRelight];next.radiance[1]=moon.y*climate[CL_sunRelight];next.radiance[2]=moon.z*climate[CL_sunRelight];
        return next;
    }
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
    for(auto& cache:state.proofRecordCache)cache.valid=false;
    state.litDraws=0;
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
    state.sceneTarget=nullptr; // the first main-view lit draw of this frame records it again
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
