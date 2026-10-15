#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentParticleShadows.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker {bool MvmDebugLog_Active(){return true;}void MvmDebugLog_LinefAlways(const char* tag,const char* format,...){std::cerr<<tag<<": ";va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);std::cerr<<'\n';} }
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    BeginParticleShadows(device.Get());
    const char* source=R"(
cbuffer PerViewConstantBuffer_t:register(b0){float4x4 projection:packoffset(c0);float4x4 g_matProjectionToWorld:packoffset(c4);float4 fade:packoffset(c8);}
cbuffer SpritecardVertexControls_t:register(b1){float4 radius;}
struct V{float4 uv:TEXCOORD0;float4 color:COLOR0;float3 world:TEXCOORD2;float3 spare:TEXCOORD3;float4 blend:TEXCOORD4;float4 light:TEXCOORD5;float4 position:SV_Position;float clip:SV_ClipDistance;};
V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};V o=(V)0;o.world=float3(p[id]*radius.x,.5);o.position=mul(projection,float4(p[id]*radius.x,.95,1));o.color=float4(1,1,1,fade.x);o.uv=float4(.25,.5,.75,.5);o.blend.x=fade.y;o.clip=1;return o;}
)";
    ComPtr<ID3DBlob> code,error;Check(SUCCEEDED(CompileEnvironmentShader(source,strlen(source),"SpriteTest",nullptr,"Main","vs_5_0",0,&code,&error)),"native fixture compile");
    ComPtr<ID3D11VertexShader> native;Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&native)),"native VS");
    native->SetPrivateData(WKPDID_D3DDebugObjectName,strlen("spritecard.vfx_vs"),"spritecard.vfx_vs");
    // Exercise cold cache population from native shader creation workers together.
    std::atomic<unsigned> waiting{0},tagged{0};std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&]{
        ComPtr<ID3D11VertexShader> variant;
        Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&variant)),"worker native VS");
        ++waiting;while(!start.load())std::this_thread::yield();
        NoteParticleShadowShader(variant.Get(),code->GetBufferPointer(),code->GetBufferSize());
        const GUID tag={0xe47b4628,0xdfe5,0x4bb4,{0xb7,0xe9,0xe7,0x58,0x76,0x57,0x14,0x42}};
        UINT slot=99,size=4;if(SUCCEEDED(variant->GetPrivateData(tag,&size,&slot))&&slot==0)++tagged;
    });
    while(waiting.load()!=8)std::this_thread::yield();start=true;
    for(auto& worker:workers)worker.join();
    Check(tagged.load()==8,"concurrent cold shader cache retains valid captures for every worker");
    NoteParticleShadowShader(native.Get(),code->GetBufferPointer(),code->GetBufferSize());
    Check(ParticleShadowStream().buffer!=nullptr,"particle buffers initialized");
    {const GUID tag={0xe47b4628,0xdfe5,0x4bb4,{0xb7,0xe9,0xe7,0x58,0x76,0x57,0x14,0x42}};UINT value=99,bytes=4;Check(SUCCEEDED(native->GetPrivateData(tag,&bytes,&value))&&value==0,"reflected capture tag");}
    auto buffer=[&](UINT bytes){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> b;Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&b)),"constant buffer");return b;};
    auto view=buffer(144),radius=buffer(16),material=buffer(320);
    float controls[80]{};controls[7]=controls[19]=controls[33]=1;const UINT alphaChannel=1;memcpy(&controls[76],&alphaChannel,4);const UINT flags=32768|1024;memcpy(&controls[78],&flags,4);ctx->UpdateSubresource(material.Get(),0,nullptr,controls,0,0);
    float r[4]={1};ctx->UpdateSubresource(radius.Get(),0,nullptr,r,0,0);
    const float pixels[8]={1,1,1,0,1,1,1,1};D3D11_TEXTURE2D_DESC td{};td.Width=2;td.Height=1;td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels,32,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> sheet;
    Check(SUCCEEDED(device->CreateTexture2D(&td,&initial,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&sheet)),"sheet");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"sampler");
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;bd.RenderTarget[0].BlendOp=bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> blend;Check(SUCCEEDED(device->CreateBlendState(&bd,&blend)),"blend");
    td.Width=td.Height=64;td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> depth,readback;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth)),"depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;ComPtr<ID3D11DepthStencilView> dsv;Check(SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&dd,&dsv)),"dsv");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&readback)),"readback");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)),"raster");ctx->RSSetState(raster.Get());
    Matrix identity=Identity();
    float nativeMaxDepth=.95f;
    auto capture=[&](float alpha,float frame){
        float values[36]{};memcpy(values,identity.data(),64);memcpy(values+16,identity.data(),64);values[32]=alpha;values[33]=frame;ctx->UpdateSubresource(view.Get(),0,nullptr,values,0,0);
        auto v=view.Get(),m=material.Get(),r=radius.Get();auto s=sheet.Get();auto sam=sampler.Get();ctx->VSSetConstantBuffers(0,1,&v);ctx->VSSetConstantBuffers(1,1,&r);ctx->PSSetConstantBuffers(4,1,&m);ctx->PSSetShaderResources(8,1,&s);ctx->PSSetSamplers(0,1,&sam);
        ctx->VSSetShader(native.Get(),nullptr,0);ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);D3D11_VIEWPORT vp{0,0,640,480,0,nativeMaxDepth};ctx->RSSetViewports(1,&vp);
        const bool accepted=BeginParticleShadowDraw(ctx.Get(),3,1);if(accepted){ctx->Draw(3,0);Check(EndParticleShadowDraw(ctx.Get()),"end capture");}return accepted;
    };
    auto coverage=[&](){
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);auto target=dsv.Get();ctx->OMSetRenderTargets(0,nullptr,target);ctx->OMSetDepthStencilState(nullptr,0);ctx->OMSetBlendState(nullptr,nullptr,~0u);D3D11_VIEWPORT vp{0,0,64,64,0,1};ctx->RSSetViewports(1,&vp);
        DrawParticleShadows(ctx.Get(),identity);ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(readback.Get(),depth.Get());D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)),"map");UINT count=0;for(UINT y=0;y<64;++y){auto row=reinterpret_cast<const float*>(static_cast<const char*>(map.pData)+y*map.RowPitch);for(UINT x=0;x<64;++x)count+=row[x]<.6f;}ctx->Unmap(readback.Get(),0);return count/4096.f;
    };
    Check(capture(.5f,.5f),"accept opacity sprites including clip-distance signature");
    NextParticleShadowFrame();Check(ParticleShadowStream().count==3,"publish complete native pose");
    const auto fraction=coverage();Check(fraction>.20f&&fraction<.30f,"sheet interpolation and particle fade give 25% coverage, not an opaque billboard");
    SetParticleShadowsEnabled(false);Check(coverage()==0&&ParticleShadowStream().count==0,"A/B toggle omits smoke without altering captured native state");SetParticleShadowsEnabled(true);
    NextParticleShadowFrame();Check(ParticleShadowStream().count==0&&coverage()==0,"empty next frame clears ended smoke");
    Check(capture(1,0),"transparent sheet capture");Check(coverage()==0,"transparent texture does not cast a rectangular shadow");
    NextParticleShadowFrame();NextParticleShadowFrame();Check(capture(0,1),"zero particle opacity capture");Check(coverage()==0,"faded particle has no shadow");
    NextParticleShadowFrame();NextParticleShadowFrame();
    const float rgbPixels[8]={0,0,0,1,1,1,1,1};ctx->UpdateSubresource(texture.Get(),0,nullptr,rgbPixels,32,0);
    const UINT luminanceChannel=6;memcpy(&controls[76],&luminanceChannel,4);ctx->UpdateSubresource(material.Get(),0,nullptr,controls,0,0);
    Check(capture(1,0),"RGB opacity capture");Check(coverage()==0,"RGB-encoded transparent texel with opaque alpha does not make a rectangle");
    NextParticleShadowFrame();NextParticleShadowFrame();Check(capture(.5f,.5f),"RGB interpolated capture");const auto rgbFraction=coverage();Check(rgbFraction>.20f&&rgbFraction<.30f,"RGB opacity obeys frame blend and fade");
    NextParticleShadowFrame();NextParticleShadowFrame();nativeMaxDepth=.1f;Check(capture(.5f,.5f),"first-person sprite pass retains world pose");Check(coverage()>.20f&&coverage()<.30f,"first-person projection depth does not move the shadow caster");
    NextParticleShadowFrame();NextParticleShadowFrame();controls[7]=0;controls[0]=1;ctx->UpdateSubresource(material.Get(),0,nullptr,controls,0,0);
    Check(capture(1,1),"premultiplied emissive sprite capture");Check(coverage()==0,"pure self-lit flash with zero output alpha never casts darkness");
    NextParticleShadowFrame();NextParticleShadowFrame();controls[0]=.2f;controls[1]=.8f;ctx->UpdateSubresource(material.Get(),0,nullptr,controls,0,0);
    Check(capture(.5f,.5f),"scene-lit smoke retains authored additive output");const auto litFraction=coverage();Check(litFraction>.20f&&litFraction<.30f,"scene-lit smoke uses physical sheet density rather than zero additive output alpha");
    bd.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;Check(SUCCEEDED(device->CreateBlendState(&bd,&blend)),"additive blend");Check(!capture(1,1),"additive flash/glow never casts darkness");
    EndParticleShadows();std::cout<<"Particle shadows: animated alpha, fade, publication, expiry and additive rejection passed\n";
}
