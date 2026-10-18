#define NOMINMAX
#include "EnvironmentSunVisibility.h"
#include "../Debug/GpuPassProfiler.h"
#include "EnvironmentAmbientOcclusionMath.h"
#include "EnvironmentSunVisibilityShaders.h"
#include "EnvironmentSunVisibilityMath.h"
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentDepthRead.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <string>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> visibility[4];
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> comparison;
    ComPtr<ID3D11Texture2D> result;
    DepthReadTarget depthRead;
    ComPtr<ID3D11ShaderResourceView> resultSrv;
    ComPtr<ID3D11UnorderedAccessView> resultUav;
    ComPtr<ID3D11Texture2D> preparedDepth;
    D3D11_TEXTURE2D_DESC depthDesc{};
    UINT width=0,height=0;
    bool prepared=false,ready=false;
} state;
struct Row {float x,y,z,w;};
struct Parameters {
    Matrix clipToWorld;Matrix cascade[4];Row rect[4];Row thresholds,camera,viewport,depthInfo,sun,atlas;
    std::array<SunDiskTap,SunSearchTaps> searchDisk;std::array<SunDiskTap,SunFilterTaps> filterDisk;
};
bool Compile(ID3D11Device* device,unsigned samples,ComPtr<ID3D11ComputeShader>& shader) {
    const std::string count=std::to_string(samples);
    const D3D_SHADER_MACRO macros[]{{"MSAA_SAMPLES",count.c_str()},{nullptr,nullptr}};
    ComPtr<ID3DBlob> code,error;
    if(FAILED(CompileEnvironmentShader(SunVisibilityShader,sizeof(SunVisibilityShader)-1,
        "EnvironmentSunVisibility",macros,"Visibility","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&error))){
        MvmDebugLog_LinefAlways("environment.sunvis","compile samples=%u error=%s",samples,
            error?static_cast<const char*>(error->GetBufferPointer()):"compiler unavailable");return false;
    }
    return SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
}
bool Targets(ID3D11Texture2D* input,const D3D11_TEXTURE2D_DESC& source) {
    if(!state.depthRead.Prepare(state.device.Get(),input))return false;
    if(state.depthRead.View()&&state.depthDesc.Width==source.Width&&state.depthDesc.Height==source.Height&&
       state.depthDesc.Format==source.Format&&state.depthDesc.SampleDesc.Count==source.SampleDesc.Count&&
       state.depthDesc.SampleDesc.Quality==source.SampleDesc.Quality)return true;
    state.result.Reset();state.resultSrv.Reset();state.resultUav.Reset();
    D3D11_TEXTURE2D_DESC d{};d.Width=source.Width;d.Height=source.Height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    if(FAILED(state.device->CreateTexture2D(&d,nullptr,&state.result))||
       FAILED(state.device->CreateShaderResourceView(state.result.Get(),nullptr,&state.resultSrv))||
       FAILED(state.device->CreateUnorderedAccessView(state.result.Get(),nullptr,&state.resultUav)))return false;
    state.width=source.Width;state.height=source.Height;state.depthDesc=source;return true;
}
}
void EndSunVisibility(){state=State{};}
void BeginSunVisibility(ID3D11Device* device){
    EndSunVisibility();if(!device)return;
    RenderPrivateTextureScope privateTextures;state.device=device;
    for(unsigned i=0;i<4;++i)if(!Compile(device,1u<<i,state.visibility[i]))return;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sd.MaxLOD=D3D11_FLOAT32_MAX;
    state.ready=SUCCEEDED(device->CreateBuffer(&cb,nullptr,&state.constants))&&
        SUCCEEDED(device->CreateSamplerState(&sd,&state.comparison));
}
void NextSunVisibilityFrame(){state.prepared=false;state.preparedDepth.Reset();}
ID3D11ShaderResourceView* SunVisibility(ID3D11DeviceContext* immediate,const D3D11_VIEWPORT& vp,const SunVisibilityInput& in){
    if(!state.ready||!immediate||!in.cascades||!in.rects||!in.atlas||!in.clipToWorld||
       !AmbientViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth))return nullptr;
    ComPtr<ID3D11DepthStencilView> dsv;immediate->OMGetRenderTargets(0,nullptr,&dsv);
    ComPtr<ID3D11Resource> resource;if(dsv)dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> depth;if(!resource||FAILED(resource.As(&depth)))return nullptr;
    // The result holds the visibility of the nearest opaque surface at each pixel. A blended draw
    // (glass, decals, particles) shades a different surface there, so it keeps the native filter.
    ComPtr<ID3D11BlendState> blend;FLOAT factor[4];UINT mask=0;immediate->OMGetBlendState(&blend,factor,&mask);
    if(blend){D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);if(bd.RenderTarget[0].BlendEnable)return nullptr;}
    if(state.prepared)return state.preparedDepth.Get()==depth.Get()?state.resultSrv.Get():nullptr;
    D3D11_TEXTURE2D_DESC source{};depth->GetDesc(&source);
    if(source.ArraySize!=1||source.MipLevels!=1||source.Width!=UINT(vp.Width)||source.Height!=UINT(vp.Height)||vp.TopLeftX!=0||vp.TopLeftY!=0)
        return nullptr;
    const unsigned samples=source.SampleDesc.Count;
    unsigned shader=0;while(shader<4&&(1u<<shader)!=samples)++shader;if(shader==4||!state.visibility[shader])return nullptr;
    // Only after the main depth prepass: before it the depth buffer is incomplete and the result
    // would be cached for the whole frame.
    ComPtr<ID3D11DepthStencilState> depthState;UINT stencil=0;immediate->OMGetDepthStencilState(&depthState,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};if(depthState)depthState->GetDesc(&dd);
    if(!dd.DepthEnable||(dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO&&dd.DepthFunc!=D3D11_COMPARISON_EQUAL))return nullptr;
    const bool reversed=dd.DepthFunc==D3D11_COMPARISON_GREATER||dd.DepthFunc==D3D11_COMPARISON_GREATER_EQUAL;
    RenderPrivateTextureScope privateTextures;if(!Targets(depth.Get(),source))return nullptr;
    Parameters p{};p.clipToWorld=*in.clipToWorld;
    for(unsigned i=0;i<4;++i){p.cascade[i]=in.cascades[i];std::memcpy(&p.rect[i],in.rects+i*4,sizeof(Row));}
    p.thresholds={.985f,.985f,.985f,1.f};
    p.camera={in.camera.x,in.camera.y,in.camera.z,0};
    p.viewport={vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height};
    p.depthInfo={vp.MinDepth,vp.MaxDepth-vp.MinDepth,reversed?1.f:0.f,0};
    p.sun={in.tanHalfAngle,in.bias,in.tileSize,in.maxSearchDistance};
    p.atlas={in.atlasWidth,in.atlasHeight,1.f/in.atlasWidth,1.f/in.atlasHeight};
    // Fixed world-space quadrature: calculate trigonometry once on the CPU,
    // rather than for every shadowed pixel, cascade and frame.
    static const auto searchDisk=SunDiskTaps<SunSearchTaps,true>();
    static const auto filterDisk=SunDiskTaps<SunFilterTaps,false>();
    p.searchDisk=searchDisk;p.filterDisk=filterDisk;
    // Recorded straight onto the immediate context, as the AO pass is: a deferred context's
    // FinishCommandList/ExecuteCommandList costs CPU every lit frame and is the replay ReShade's
    // d3d11 wrapper crashes in. Only the compute slots used here are saved and restored.
    auto ctx=immediate;
    ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount=256;
    ctx->CSGetShader(previousShader.GetAddressOf(),previousInstances,&previousInstanceCount);
    ID3D11Buffer* previousConstants=nullptr;ctx->CSGetConstantBuffers(0,1,&previousConstants);
    ID3D11SamplerState* previousSampler=nullptr;ctx->CSGetSamplers(0,1,&previousSampler);
    ID3D11ShaderResourceView* previousViews[2]{};ctx->CSGetShaderResources(0,2,previousViews);
    ID3D11UnorderedAccessView* previousUav=nullptr;ctx->CSGetUnorderedAccessViews(0,1,&previousUav);
    DepthReadScope depthReadScope(ctx,state.depthRead);
    ctx->UpdateSubresource(state.constants.Get(),0,nullptr,&p,0,0);
    auto cb=state.constants.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* views[2]{state.depthRead.View(),in.atlas};ctx->CSSetShaderResources(0,2,views);
    auto sampler=state.comparison.Get();ctx->CSSetSamplers(0,1,&sampler);
    auto uav=state.resultUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ctx->CSSetShader(state.visibility[shader].Get(),nullptr,0);
    {GPU_PASS_SCOPE(ctx,"sun visibility (screen)");ctx->Dispatch((source.Width+7)/8,(source.Height+7)/8,1);}
    uav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ID3D11ShaderResourceView* noViews[2]{};ctx->CSSetShaderResources(0,2,noViews);
    UINT keepCounter=~0u;ctx->CSSetUnorderedAccessViews(0,1,&previousUav,&keepCounter);
    ctx->CSSetShaderResources(0,2,previousViews);ctx->CSSetSamplers(0,1,&previousSampler);ctx->CSSetConstantBuffers(0,1,&previousConstants);
    ctx->CSSetShader(previousShader.Get(),previousInstances,previousInstanceCount);
    for(UINT i=0;i<previousInstanceCount;++i)if(previousInstances[i])previousInstances[i]->Release();
    for(auto* view:previousViews)if(view)view->Release();
    if(previousConstants)previousConstants->Release();
    if(previousSampler)previousSampler->Release();
    if(previousUav)previousUav->Release();
    state.prepared=true;state.preparedDepth=depth;
    if(MvmDebugLog_Active()){
        static ULONGLONG last=0;const auto now=GetTickCount64();
        if(now-last>=3000){last=now;
            MvmDebugLog_LinefAlways("environment.sunvis","owned=1 depth=%ux%u samples=%u tanHalf=%.5f bias=%.6f tile=%.0f reversed=%u depthDirect=%u depthFormat=%u depthBind=0x%x",
                source.Width,source.Height,samples,in.tanHalfAngle,in.bias,in.tileSize,unsigned(reversed),unsigned(state.depthRead.Direct()),unsigned(source.Format),source.BindFlags);
        }
    }
    return state.resultSrv.Get();
}
} // namespace Filmmaker::Environment
