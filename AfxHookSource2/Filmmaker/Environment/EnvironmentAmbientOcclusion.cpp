#define NOMINMAX
#include "EnvironmentAmbientOcclusion.h"
#include "../Debug/GpuPassProfiler.h"
#include "EnvironmentAmbientOcclusionMath.h"
#include "EnvironmentAmbientOcclusionShaders.h"
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentDepthRead.h"
#include "EnvironmentSkyVisibilityMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> preparePositions[4],horizon,filter;
    ComPtr<ID3D11Buffer> constants,skyConstants;
    ComPtr<ID3D11SamplerState> skySampler;
    ComPtr<ID3D11Texture2D> positions,raw,filtered;
    DepthReadTarget depthRead;
    ComPtr<ID3D11ShaderResourceView> positionsSrv,rawSrv,filteredSrv,neutral;
    ComPtr<ID3D11UnorderedAccessView> positionsUav,rawUav,filteredUav;
    ComPtr<ID3D11Texture2D> preparedDepth;
    D3D11_TEXTURE2D_DESC depthDesc{};
    UINT width=0,height=0;
    bool prepared=false,ready=false;
} state;
struct Row {float x,y,z,w;};
struct Parameters {Row camera,viewport,output,tuning;Matrix clipToWorld;};
struct SkyParameters {Matrix matrices[SkyVisibilityDirections];Row parameters;};
bool Compile(ID3D11Device* device,const char* entry,unsigned samples,ComPtr<ID3D11ComputeShader>& shader) {
    const std::string count=std::to_string(samples);
    const D3D_SHADER_MACRO macros[]{{"MSAA_SAMPLES",count.c_str()},{nullptr,nullptr}};
    ComPtr<ID3DBlob> code,error;
    if(FAILED(CompileEnvironmentShader(AmbientOcclusionShader,sizeof(AmbientOcclusionShader)-1,
        "EnvironmentAmbientOcclusion",macros,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&error))){
        MvmDebugLog_LinefAlways("environment.ao","compile=%s samples=%u error=%s",entry,samples,
            error?static_cast<const char*>(error->GetBufferPointer()):"compiler unavailable");return false;
    }
    return SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
}
bool Targets(ID3D11Texture2D* input,const D3D11_TEXTURE2D_DESC& source,UINT width,UINT height) {
    if(!state.depthRead.Prepare(state.device.Get(),input))return false;
    if(state.depthRead.View()&&state.positions&&state.raw&&state.filtered&&state.width==width&&state.height==height&&
       state.depthDesc.Width==source.Width&&state.depthDesc.Height==source.Height&&
       state.depthDesc.Format==source.Format&&state.depthDesc.SampleDesc.Count==source.SampleDesc.Count&&
       state.depthDesc.SampleDesc.Quality==source.SampleDesc.Quality)return true;
    state.raw.Reset();state.rawSrv.Reset();state.rawUav.Reset();
    state.positions.Reset();state.positionsSrv.Reset();state.positionsUav.Reset();
    state.filtered.Reset();state.filteredSrv.Reset();state.filteredUav.Reset();
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    // Resolve MSAA and unproject once per AO texel, rather than per horizon tap.
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&state.positions))||
       FAILED(state.device->CreateShaderResourceView(state.positions.Get(),nullptr,&state.positionsSrv))||
       FAILED(state.device->CreateUnorderedAccessView(state.positions.Get(),nullptr,&state.positionsUav)))return false;
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&state.raw))||
       FAILED(state.device->CreateShaderResourceView(state.raw.Get(),nullptr,&state.rawSrv))||
       FAILED(state.device->CreateUnorderedAccessView(state.raw.Get(),nullptr,&state.rawUav)))return false;
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&state.filtered))||
       FAILED(state.device->CreateShaderResourceView(state.filtered.Get(),nullptr,&state.filteredSrv))||
       FAILED(state.device->CreateUnorderedAccessView(state.filtered.Get(),nullptr,&state.filteredUav)))return false;
    state.width=width;state.height=height;state.depthDesc=source;return true;
}
}
void EndAmbientOcclusion(){state=State{};}
void BeginAmbientOcclusion(ID3D11Device* device){
    EndAmbientOcclusion();if(!device)return;
    RenderPrivateTextureScope privateTextures;state.device=device;
    const float neutral[4]{1,1,1,1};D3D11_SUBRESOURCE_DATA initial{neutral,16,0};
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateTexture2D(&d,&initial,&texture))||
       FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&state.neutral)))return;
    for(unsigned i=0;i<4;++i)if(!Compile(device,"PreparePositions",1u<<i,state.preparePositions[i]))return;
    if(!Compile(device,"Horizon",1,state.horizon))return;
    if(!Compile(device,"Filter",1,state.filter))return;
    D3D11_BUFFER_DESC sky=cb;sky.ByteWidth=sizeof(SkyParameters);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sd.MaxLOD=D3D11_FLOAT32_MAX;
    state.ready=SUCCEEDED(device->CreateBuffer(&cb,nullptr,&state.constants))&&
        SUCCEEDED(device->CreateBuffer(&sky,nullptr,&state.skyConstants))&&
        SUCCEEDED(device->CreateSamplerState(&sd,&state.skySampler));
}
void AmbientOcclusionSize(UINT& width,UINT& height){
    width=state.prepared?state.width:0;height=state.prepared?state.height:0;
}
ID3D11ShaderResourceView* AmbientOcclusionPositions(ID3D11DeviceContext* ctx,UINT& width,UINT& height){
    width=height=0;if(!ctx||!state.prepared)return nullptr;
    ComPtr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);
    ComPtr<ID3D11Resource> depth;if(dsv)dsv->GetResource(&depth);
    if(depth.Get()!=state.preparedDepth.Get())return nullptr;
    width=state.width;height=state.height;return state.positionsSrv.Get();
}
void NextAmbientOcclusionFrame(){state.prepared=false;state.preparedDepth.Reset();}
ID3D11ShaderResourceView* AmbientOcclusion(ID3D11DeviceContext* immediate,Vec3 camera,
    const D3D11_VIEWPORT& vp,ID3D11Buffer* view,const Matrix& clipToWorld,float strength,const SkyVisibilityInput* sky,bool prepareForGi){
    const bool skyEnabled=sky&&sky->maps&&sky->matrices&&sky->strength>0;
    if(!state.ready||!immediate||!view||(strength<=0&&!skyEnabled&&!prepareForGi)||
       !AmbientViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth))return state.neutral.Get();
    ComPtr<ID3D11DepthStencilView> dsv;immediate->OMGetRenderTargets(0,nullptr,&dsv);
    ComPtr<ID3D11Resource> resource;if(dsv)dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> depth;if(!resource||FAILED(resource.As(&depth)))return state.neutral.Get();
    if(state.prepared)return state.preparedDepth.Get()==depth.Get()?state.filteredSrv.Get():state.neutral.Get();
    D3D11_TEXTURE2D_DESC source{};depth->GetDesc(&source);
    if(source.ArraySize!=1||source.MipLevels!=1||source.Width!=UINT(vp.Width)||source.Height!=UINT(vp.Height)||vp.TopLeftX!=0||vp.TopLeftY!=0)
        return state.neutral.Get();
    const unsigned samples=source.SampleDesc.Count;
    unsigned shader=0;while(shader<4&&(1u<<shader)!=samples)++shader;if(shader==4)return state.neutral.Get();
    // Evaluate only after the main depth prepass. Missing depth yields neutral AO,
    // never a previous frame's visibility or an engine AO texture.
    ComPtr<ID3D11DepthStencilState> depthState;UINT stencil=0;immediate->OMGetDepthStencilState(&depthState,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};if(depthState)depthState->GetDesc(&dd);
    if(!dd.DepthEnable||(dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO&&dd.DepthFunc!=D3D11_COMPARISON_EQUAL))return state.neutral.Get();
    const bool reversed=dd.DepthFunc==D3D11_COMPARISON_GREATER||dd.DepthFunc==D3D11_COMPARISON_GREATER_EQUAL;
    UINT width=(source.Width+1)/2,height=(source.Height+1)/2;
    // Read the native AO dimensions solely to preserve its material UV convention.
    ComPtr<ID3D11ShaderResourceView> original;immediate->PSGetShaderResources(7,1,&original);
    ComPtr<ID3D11Resource> originalResource;if(original)original->GetResource(&originalResource);
    ComPtr<ID3D11Texture2D> originalTexture;
    if(originalResource&&SUCCEEDED(originalResource.As(&originalTexture))){
        D3D11_TEXTURE2D_DESC od{};originalTexture->GetDesc(&od);
        if(od.Width>=width/2&&od.Height>=height/2&&od.Width<=source.Width&&od.Height<=source.Height&&
           od.Width*source.Height==od.Height*source.Width){width=od.Width;height=od.Height;}
    }
    RenderPrivateTextureScope privateTextures;if(!Targets(depth.Get(),source,width,height))return state.neutral.Get();
    const Parameters parameters{{camera.x,camera.y,camera.z,32},{vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height},
        {float(width),float(height),vp.MinDepth,vp.MaxDepth-vp.MinDepth},{strength,reversed?1.f:0.f,0,0},clipToWorld};
    // Recorded straight onto the immediate context: a deferred context's FinishCommandList and
    // ExecuteCommandList cost a lit frame 7-15 ms of CPU, and that replay is the call ReShade's
    // d3d11 wrapper crashes in. Only the compute slots used here are saved and restored.
    auto ctx=immediate;
    ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount=256;
    ctx->CSGetShader(previousShader.GetAddressOf(),previousInstances,&previousInstanceCount);
    ID3D11Buffer* previousConstants[2]{};ctx->CSGetConstantBuffers(0,2,previousConstants);
    ID3D11SamplerState* previousSampler=nullptr;ctx->CSGetSamplers(0,1,&previousSampler);
    ID3D11ShaderResourceView* previousViews[4]{};ctx->CSGetShaderResources(0,4,previousViews);
    ID3D11UnorderedAccessView* previousUav=nullptr;ctx->CSGetUnorderedAccessViews(0,1,&previousUav);
    DepthReadScope depthReadScope(ctx,state.depthRead);
    ctx->UpdateSubresource(state.constants.Get(),0,nullptr,&parameters,0,0);
    // Normal offset and bias are about one sky-map texel of this map, in world units.
    SkyParameters skyParameters{};
    if(skyEnabled){for(unsigned i=0;i<SkyVisibilityDirections;++i)skyParameters.matrices[i]=sky->matrices[i];
        const Vec3 axis{sky->matrices[0][0],sky->matrices[0][1],sky->matrices[0][2]};
        const float texel=2/((std::max)(1e-6f,std::sqrt(Dot(axis,axis)))*float(SkyVisibilitySize));
        skyParameters.parameters={1,(std::min)(1.f,sky->strength),texel*1.5f,texel*1.5f};}
    ctx->UpdateSubresource(state.skyConstants.Get(),0,nullptr,&skyParameters,0,0);
    auto cb=state.constants.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    auto skyCb=state.skyConstants.Get();ctx->CSSetConstantBuffers(1,1,&skyCb);
    auto skySampler=state.skySampler.Get();ctx->CSSetSamplers(0,1,&skySampler);
    ID3D11ShaderResourceView* skyMaps=skyEnabled?sky->maps:nullptr;ctx->CSSetShaderResources(2,1,&skyMaps);
    auto srv=state.depthRead.View();ctx->CSSetShaderResources(0,1,&srv);
    auto uav=state.positionsUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    {GPU_PASS_SCOPE(ctx,"ao: positions, horizon, filter");
    ctx->CSSetShader(state.preparePositions[shader].Get(),nullptr,0);ctx->Dispatch((width+7)/8,(height+7)/8,1);
    uav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    srv=state.positionsSrv.Get();ctx->CSSetShaderResources(3,1,&srv);
    uav=state.rawUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ctx->CSSetShader(state.horizon.Get(),nullptr,0);ctx->Dispatch((width+7)/8,(height+7)/8,1);
    uav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    srv=state.rawSrv.Get();ctx->CSSetShaderResources(1,1,&srv);
    uav=state.filteredUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ctx->CSSetShader(state.filter.Get(),nullptr,0);ctx->Dispatch((width+7)/8,(height+7)/8,1);}
    uav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ID3D11ShaderResourceView* noViews[4]{};ctx->CSSetShaderResources(0,4,noViews);
    UINT keepCounter=~0u;ctx->CSSetUnorderedAccessViews(0,1,&previousUav,&keepCounter);
    ctx->CSSetShaderResources(0,4,previousViews);ctx->CSSetSamplers(0,1,&previousSampler);ctx->CSSetConstantBuffers(0,2,previousConstants);
    ctx->CSSetShader(previousShader.Get(),previousInstances,previousInstanceCount);
    for(UINT i=0;i<previousInstanceCount;++i)if(previousInstances[i])previousInstances[i]->Release();
    for(auto* view:previousViews)if(view)view->Release();
    for(auto* buffer:previousConstants)if(buffer)buffer->Release();
    if(previousSampler)previousSampler->Release();
    if(previousUav)previousUav->Release();
    state.prepared=true;state.preparedDepth=depth;
    if(MvmDebugLog_Active()){
        static ULONGLONG lastView=0;const auto viewNow=GetTickCount64();if(viewNow-lastView>=3000){lastView=viewNow;
            // Diagnostic readback (throttled, debug only): the material addresses t7 through
            // cb4[16] and resolves it against t8 depth. Stale or zero rows collapse every pixel
            // onto one AO texel, which silently removes AO from the frame.
            D3D11_BUFFER_DESC vd{};view->GetDesc(&vd);float rows[4][4]{};
            if(vd.ByteWidth>=17*16){D3D11_BUFFER_DESC sd{};sd.ByteWidth=16*4;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Buffer> staging;const D3D11_BOX box{0,0,0,16,1,1},box16{16*16,0,0,17*16,1,1},box1{16,0,0,32,1,1};
                if(SUCCEEDED(state.device->CreateBuffer(&sd,nullptr,&staging))){
                    immediate->CopySubresourceRegion(staging.Get(),0,0,0,0,view,0,&box);
                    immediate->CopySubresourceRegion(staging.Get(),0,16,0,0,view,0,&box1);
                    immediate->CopySubresourceRegion(staging.Get(),0,32,0,0,view,0,&box16);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))){std::memcpy(rows,mapped.pData,sizeof(rows));immediate->Unmap(staging.Get(),0);}}}
            UINT t7w=0,t7h=0,t8w=0,t8h=0;
            for(UINT slot:{7u,8u}){ComPtr<ID3D11ShaderResourceView> bound;immediate->PSGetShaderResources(slot,1,&bound);
                ComPtr<ID3D11Resource> r;if(bound)bound->GetResource(&r);ComPtr<ID3D11Texture2D> t;
                if(r&&SUCCEEDED(r.As(&t))){D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);(slot==7?t7w:t8w)=td.Width;(slot==7?t7h:t8h)=td.Height;}}
            // Mean and minimum of the texture handed to the material (debug only, throttled).
            float aoMean=-1,aoMin=-1;{
                D3D11_TEXTURE2D_DESC fd{};state.filtered->GetDesc(&fd);fd.Usage=D3D11_USAGE_STAGING;fd.BindFlags=0;fd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                if(SUCCEEDED(state.device->CreateTexture2D(&fd,nullptr,&staging))){
                    immediate->CopyResource(staging.Get(),state.filtered.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))){
                        double sum=0;unsigned count=0;aoMin=1e9f;
                        for(UINT y=0;y<fd.Height;y+=8){const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch);
                            for(UINT x=0;x<fd.Width;x+=8){const uint16_t h=row[x*4];const int e=(h>>10)&31;
                                const float v=(h&32768?-1.f:1.f)*std::ldexp(e?1+float(h&1023)/1024:float(h&1023)/1024,e?e-15:-14);
                                sum+=v;++count;aoMin=(std::min)(aoMin,v);}}
                        immediate->Unmap(staging.Get(),0);aoMean=count?float(sum/count):-1;}}}
            // Raw horizon output at the centre: (ao, world position). (1,0,0,0) means the depth
            // read was rejected; (1,p) a degenerate normal.
            float centre[4]{-1,-1,-1,-1};{
                D3D11_TEXTURE2D_DESC rd{};state.raw->GetDesc(&rd);rd.Width=rd.Height=1;rd.Usage=D3D11_USAGE_STAGING;rd.BindFlags=0;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                if(SUCCEEDED(state.device->CreateTexture2D(&rd,nullptr,&staging))){
                    const D3D11_BOX box{state.width/2,state.height/2,0,state.width/2+1,state.height/2+1,1};
                    immediate->CopySubresourceRegion(staging.Get(),0,0,0,0,state.raw.Get(),0,&box);D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))){std::memcpy(centre,mapped.pData,16);immediate->Unmap(staging.Get(),0);}}}
            MvmDebugLog_LinefAlways("environment.ao.output","mean=%.3f min=%.3f centreRaw=(%.3f %.1f %.1f %.1f) reversed=%u viewport=(%g..%g)",
                aoMean,aoMin,centre[0],centre[1],centre[2],centre[3],unsigned(reversed),vp.MinDepth,vp.MaxDepth);
            MvmDebugLog_LinefAlways("environment.ao.inputs","cb4[0]=(%g %g %g %g) cb4[1]=(%g %g %g %g) cb4[16]=(%g %g %g %g) nativeT7=%ux%u t8=%ux%u",
                rows[0][0],rows[0][1],rows[0][2],rows[0][3],rows[1][0],rows[1][1],rows[1][2],rows[1][3],
                rows[2][0],rows[2][1],rows[2][2],rows[2][3],t7w,t7h,t8w,t8h);}
        static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
            MvmDebugLog_LinefAlways("environment.ao","owned=1 depth=%ux%u samples=%u output=%ux%u radius=32 strength=%g sky=%u skyStrength=%g skyTexel=%.2f nativeAoSampled=0 depthDirect=%u depthFormat=%u depthBind=0x%x",
                source.Width,source.Height,samples,width,height,strength,unsigned(skyEnabled),skyParameters.parameters.y,skyParameters.parameters.z/1.5f,unsigned(state.depthRead.Direct()),unsigned(source.Format),source.BindFlags);}
    }
    return state.filteredSrv.Get();
}
} // namespace Filmmaker::Environment
