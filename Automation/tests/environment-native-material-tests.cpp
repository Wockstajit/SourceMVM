#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterial.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativePostMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowSpatialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderPrivateTextureScope.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderPixelResourceBindings.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAnimatedCasters.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <iostream>
#include <cstring>
#include <cmath>
#include <thread>
#include <fstream>
#include <filesystem>
#include <string>
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeCoverageMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAtmosphereMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentLightingProofShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeSunCsmMath.h"
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker {bool SkyGpuProbe_IsPlayersOnlyGreenSkyDraw(ID3D11DeviceContext*,unsigned,unsigned){return false;} bool MvmDebugLog_Active(){return false;}void MvmDebugLog_LinefAlways(const char*,const char*,...){} }
namespace Filmmaker::Environment {UINT AnimatedCasterVertexCount(AnimatedCasterSubset){return 0;} void RestoreWorldSunTile(ID3D11DeviceContext*){} WorldLook DefaultWorldLook(){return WorldLook{};} ID3D11ShaderResourceView* SkyboxSunShadow(Matrix*){return nullptr;}}
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
void CheckResourceBindings() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"binding device");
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> a,b;ComPtr<ID3D11ShaderResourceView> av,bv;
    Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&a))&&SUCCEEDED(device->CreateTexture2D(&d,nullptr,&b))&&
          SUCCEEDED(device->CreateShaderResourceView(a.Get(),nullptr,&av))&&SUCCEEDED(device->CreateShaderResourceView(b.Get(),nullptr,&bv)),"binding textures");
    std::array<ID3D11ShaderResourceView*,128> native{};
    for(unsigned i=0;i<native.size();++i)native[i]=i%2?av.Get():nullptr;
    ctx->PSSetShaderResources(0,UINT(native.size()),native.data());
    const UINT slots[]{127,4,3,7,4};ID3D11ShaderResourceView* views[]{bv.Get(),bv.Get(),bv.Get(),nullptr,av.Get()};
    Filmmaker::RenderPixelResourceBindings bindings;bindings.Bind(ctx.Get(),5,slots,views);
    auto inspect=[&](bool replaced){
        ID3D11ShaderResourceView* read[128]{};ctx->PSGetShaderResources(0,128,read);
        for(unsigned i=0;i<128;++i){auto expected=native[i];
            if(replaced)for(unsigned j=0;j<5;++j)if(slots[j]==i)expected=views[j];
            const bool equal=read[i]==expected;if(read[i])read[i]->Release();
            Check(equal,replaced?"grouped binding preserves gaps, null and duplicate slots":"grouped restore exactly recovers native bindings");}
    };
    inspect(true);
    {Filmmaker::RenderPixelResourceBindings nested;const UINT slot=4;auto view=bv.Get();
        nested.Bind(ctx.Get(),1,&slot,&view);nested.Restore(ctx.Get());inspect(true);}
    bindings.Restore(ctx.Get());inspect(false);
    bindings.Restore(ctx.Get());inspect(false);
    bindings.Bind(ctx.Get(),5,slots,views);bindings.Bind(ctx.Get(),0,nullptr,nullptr);inspect(false);
}
void CheckLampShadowOcclusion() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"lamp occlusion device");
    auto compile=[](const char* source,const char* profile){ComPtr<ID3DBlob> code,error;
        auto hr=CompileEnvironmentShader(source,strlen(source),"LampOcclusion",nullptr,"Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"lamp occlusion compile");return code;};
    auto ps=compile(R"(
        struct Light{float4 row[30];};StructuredBuffer<Light> lights:register(t15);
        Texture2D<float> atlas:register(t6);SamplerComparisonState comparison:register(s8);
        cbuffer Lighting:register(b3){float4 lighting[1990];}
        cbuffer View:register(b4){float4 view[36];}
        cbuffer Material:register(b0){uint4 flags;}
        float4 Main(float4 position:SV_Position,float3 ndc:TEXCOORD0):SV_Target{
            float4 rect=lights[flags.x].row[13];
            float2 uv=ndc.xy*rect.zw+rect.xy;
            float reference=saturate(ndc.z+lighting[1953].z);
            float visibility=1;
            [branch]if(rect.z>0&&asuint(view[2].x)==0){
            float4 v;
            v.x=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].z,lighting[3].z),reference);
            v.y=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].y,lighting[3].z),reference);
            v.z=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].z,lighting[3].y),reference);
            v.w=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].y,lighting[3].y),reference);
            visibility=dot(v,.25);
            [branch]if(visibility!=0&&visibility!=1){
                v.x=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].z,0),reference);
                v.y=atlas.SampleCmpLevelZero(comparison,uv+float2(lighting[2].y,0),reference);
                v.z=atlas.SampleCmpLevelZero(comparison,uv+float2(0,lighting[3].y),reference);
                v.w=atlas.SampleCmpLevelZero(comparison,uv+float2(0,lighting[3].z),reference);
                visibility=dot(v,lighting[1].x)+visibility*(4*lighting[0].w);
                visibility=atlas.SampleCmpLevelZero(comparison,uv,reference)*lighting[1].y+visibility;
            }
            }
            float3 color=visibility;
            [branch]if(flags.z){float opacity=position.x*view[25].w;color=lerp(color,view[25].rgb,opacity);}
            return float4(color,1);
        })","ps_5_0");
    std::vector<uint8_t> owned;Check(NativeAtmosphereProgram(ps->GetBufferPointer(),ps->GetBufferSize(),owned),"lamp occlusion patch");
    // Fog replacement shrinks the program; a larger result proves that the verified nine-tap kernel was replaced.
    Check(owned.size()>ps->GetBufferSize(),"lamp occlusion exercises the dense shadow kernel");
    auto vsCode=compile(R"(
        struct V{float4 position:SV_Position;float3 ndc:TEXCOORD0;};
        V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V o;o.position=float4(p[id],.5,1);o.ndc=float3(p[id]*.1,.7);return o;})","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> native,modified;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs))&&
          SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&native))&&
          SUCCEEDED(device->CreatePixelShader(owned.data(),owned.size(),nullptr,&modified)),"lamp occlusion shaders validate");
    auto buffer=[&](UINT bytes,const void* data,UINT stride=0){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;
        d.BindFlags=stride?D3D11_BIND_SHADER_RESOURCE:D3D11_BIND_CONSTANT_BUFFER;d.StructureByteStride=stride;
        d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;D3D11_SUBRESOURCE_DATA initial{data,0,0};ComPtr<ID3D11Buffer> b;
        Check(SUCCEEDED(device->CreateBuffer(&d,&initial,&b)),"lamp occlusion buffer");return b;};
    std::array<float,1990*4> lighting{};Filmmaker::Relight::ShadowFilterRows(1,lighting.data());
    lighting[0]=.0174537f;lighting[1]=.001f;lighting[2]=.001f;lighting[1953*4+2]=-.0002f;
    std::array<float,144> view{};std::array<unsigned,4> flags{};
    std::array<float,120> light{};light[52]=light[53]=.5f;light[54]=.25f;light[55]=-.25f;
    auto lcb=buffer(sizeof(lighting),lighting.data()),vcb=buffer(sizeof(view),view.data()),mcb=buffer(sizeof(flags),flags.data());
    auto lb=buffer(sizeof(light),light.data(),480);ComPtr<ID3D11ShaderResourceView> lv;
    Check(SUCCEEDED(device->CreateShaderResourceView(lb.Get(),nullptr,&lv)),"lamp occlusion record view");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::array<float,64> depths;depths.fill(.3f);D3D11_SUBRESOURCE_DATA initial{depths.data(),32,0};
    ComPtr<ID3D11Texture2D> atlas;ComPtr<ID3D11ShaderResourceView> av;
    Check(SUCCEEDED(device->CreateTexture2D(&td,&initial,&atlas))&&SUCCEEDED(device->CreateShaderResourceView(atlas.Get(),nullptr,&av)),"lamp occlusion depth atlas");
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color,read;ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&target)),"lamp occlusion target");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&read)),"lamp occlusion readback");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"lamp occlusion sampler");
    auto rtv=target.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);
    auto b=lcb.Get();ctx->PSSetConstantBuffers(3,1,&b);b=vcb.Get();ctx->PSSetConstantBuffers(4,1,&b);b=mcb.Get();ctx->PSSetConstantBuffers(0,1,&b);
    auto v=av.Get();ctx->PSSetShaderResources(6,1,&v);v=lv.Get();ctx->PSSetShaderResources(15,1,&v);auto s=sampler.Get();ctx->PSSetSamplers(8,1,&s);
    auto draw=[&](ID3D11PixelShader* shader){ctx->PSSetShader(shader,nullptr,0);ctx->Draw(3,0);ctx->CopyResource(read.Get(),color.Get());
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map)),"lamp occlusion pixel read");
        float result=static_cast<const float*>(map.pData)[0];ctx->Unmap(read.Get(),0);return result;};
    Check(draw(native.Get())==0,"native wall blocks the beam");
    Check(draw(modified.Get())==0,"soft shadow kernel preserves full wall occlusion");
    view[8]=1;ctx->UpdateSubresource(vcb.Get(),0,nullptr,view.data(),0,0);
    Check(draw(native.Get())==1,"native view flag reproduces a bypassed wall shadow");
    Check(draw(modified.Get())==0,"owned atlas blocks walls even when the native view bypasses local shadows");
    depths.fill(.9f);ctx->UpdateSubresource(atlas.Get(),0,nullptr,depths.data(),32,0);
    Check(draw(native.Get())==1&&std::abs(draw(modified.Get())-1)<1e-6f,"soft shadow kernel preserves an unobstructed beam");
}
void CheckAtmosphere() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"atmosphere device");
    auto compile=[&](const char* source,const char* profile){ComPtr<ID3DBlob> code,error;
        Check(SUCCEEDED(CompileEnvironmentShader(source,strlen(source),"NativeAtmosphereTest",nullptr,"Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error)),"atmosphere compile");return code;};
    auto source=compile(R"(
        cbuffer Material:register(b0){uint4 flags;}
        cbuffer View:register(b4){float4 fog[36];}
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags.z){float opacity=p.x*fog[25].w;color.rgb=lerp(color.rgb,fog[25].rgb,opacity);}
            return color;
        })","ps_5_0");
    std::vector<uint8_t> owned;Check(NativeAtmosphereProgram(source->GetBufferPointer(),source->GetBufferSize(),owned),"recognize native gradient fog guard and opacity");
    std::vector<uint8_t> twice;Check(!NativeAtmosphereProgram(owned.data(),owned.size(),twice),"owned atmosphere is not patched twice");
    for(size_t end=0;end<64;++end)Check(!NativeAtmosphereProgram(source->GetBufferPointer(),end,twice),"truncated atmosphere program rejected");
    auto unrelated=compile(R"(
        cbuffer Material:register(b0){uint4 flags;}
        cbuffer View:register(b4){float4 other[36];}
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags.z)color.rgb*=p.x*other[26].w;
            return color;
        })","ps_5_0");
    Check(!NativeAtmosphereProgram(unrelated->GetBufferPointer(),unrelated->GetBufferSize(),twice),"unrelated material condition retains native behavior");
    auto vsCode=compile(R"(float4 Main(uint id:SV_VertexID):SV_Position {
        float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],.5,1);})","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> native,modified;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"atmosphere vertex");
    Check(SUCCEEDED(device->CreatePixelShader(source->GetBufferPointer(),source->GetBufferSize(),nullptr,&native)),"native atmosphere pixel");
    Check(SUCCEEDED(device->CreatePixelShader(owned.data(),owned.size(),nullptr,&modified)),"owned atmosphere validates");
    std::array<uint32_t,4> flags{0,0,1,0};std::array<float,144> fog{};fog[25*4+2]=1;fog[25*4+3]=1;
    ComPtr<ID3D11Buffer> material,view;D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{flags.data(),0,0};Check(SUCCEEDED(device->CreateBuffer(&cb,&data,&material)),"atmosphere material constants");
    cb.ByteWidth=sizeof(fog);data.pSysMem=fog.data();Check(SUCCEEDED(device->CreateBuffer(&cb,&data,&view)),"atmosphere fog constants");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color,read;ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&target)),"atmosphere target");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&read)),"atmosphere readback");
    auto rtv=target.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT viewport{0,0,8,8,0,1};ctx->RSSetViewports(1,&viewport);
    auto buffer=material.Get();ctx->PSSetConstantBuffers(0,1,&buffer);buffer=view.Get();ctx->PSSetConstantBuffers(4,1,&buffer);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);
    auto draw=[&](ID3D11PixelShader* shader){ctx->PSSetShader(shader,nullptr,0);ctx->Draw(3,0);ctx->CopyResource(read.Get(),color.Get());
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map)),"atmosphere pixel read");
        std::array<float,4> pixel;std::memcpy(pixel.data(),map.pData,16);ctx->Unmap(read.Get(),0);return pixel;};
    const auto baked=draw(native.Get()),live=draw(modified.Get());
    Check(baked[2]>live[2]+.1f,"native fog visibly adds scene color after lighting");
    for(unsigned i=0;i<4;++i)Check(std::abs(live[i]-std::array<float,4>{.2f,.4f,.6f,.37f}[i])<1e-6f,"owned atmosphere preserves surface light and alpha");
    std::vector<uint8_t> blackCode;ComPtr<ID3D11PixelShader> black;
    Check(NativeCoverageProgram(source->GetBufferPointer(),source->GetBufferSize(),blackCode,true)&&
          SUCCEEDED(device->CreatePixelShader(blackCode.data(),blackCode.size(),nullptr,&black)),"black material diagnostic validates");
    const auto blackPixel=draw(black.Get());
    Check(blackPixel[0]==0&&blackPixel[1]==0&&blackPixel[2]==0&&blackPixel[3]==live[3],"black diagnostic overrides every RGB contribution and preserves alpha");
    flags[2]=0;ctx->UpdateSubresource(material.Get(),0,nullptr,flags.data(),0,0);
    Check(draw(native.Get())==draw(modified.Get()),"native lighting output preserved exactly when fog is disabled");
    // The shipped probe/foliage permutation has a different material layout.
    // Exercise its guard on the GPU, including an alpha discard after the fog.
    auto probeSource=compile(R"(
        cbuffer Material:register(b0){uint4 flags[3];}
        cbuffer View:register(b4){float4 fog[36];}
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags[2].x){float opacity=p.x*fog[25].w;color.rgb=lerp(color.rgb,fog[25].rgb,opacity);}
            if(p.x>4)discard;
            return color;
        })","ps_5_0");
    Check(NativeAtmosphereProgram(probeSource->GetBufferPointer(),probeSource->GetBufferSize(),owned),"recognize probe material fog guard");
    native.Reset();modified.Reset();
    Check(SUCCEEDED(device->CreatePixelShader(probeSource->GetBufferPointer(),probeSource->GetBufferSize(),nullptr,&native))&&
          SUCCEEDED(device->CreatePixelShader(owned.data(),owned.size(),nullptr,&modified)),"probe atmosphere shaders validate");
    material.Reset();std::array<uint32_t,12> probeFlags{};probeFlags[8]=1;
    cb.ByteWidth=sizeof(probeFlags);data.pSysMem=probeFlags.data();
    Check(SUCCEEDED(device->CreateBuffer(&cb,&data,&material)),"probe atmosphere material constants");
    buffer=material.Get();ctx->PSSetConstantBuffers(0,1,&buffer);
    Check(draw(native.Get())[2]>draw(modified.Get())[2]+.1f,"probe fog is replaced independently of lighting");
    const float clear[4]{.17f,.23f,.31f,.43f};ctx->ClearRenderTargetView(target.Get(),clear);draw(modified.Get());
    D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map)),"probe coverage read");
    std::array<float,4> discarded{};std::memcpy(discarded.data(),static_cast<const uint8_t*>(map.pData)+7*16,16);ctx->Unmap(read.Get(),0);
    for(unsigned i=0;i<4;++i)Check(discarded[i]==clear[i],"atmosphere replacement retains material discard coverage");
    auto movedFlag=compile(R"(
        cbuffer Material:register(b0){uint4 flags[29];}
        cbuffer View:register(b4){float4 fog[36];}
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags[28].y){float opacity=p.x*fog[25].w;color.rgb=lerp(color.rgb,fog[25].rgb,opacity);}
            return color;
        })","ps_5_0");
    Check(NativeAtmosphereProgram(movedFlag->GetBufferPointer(),movedFlag->GetBufferSize(),owned),"fog recognized when material constants relocate its flag");
    auto parent=compile(R"(
        cbuffer Material:register(b0){uint4 flags[3];}
        cbuffer View:register(b4){float4 fog[36];}
        StructuredBuffer<float4> light:register(t17);
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags[0].x){
                color.rgb+=light[uint(p.x)].rgb;
                [branch]if(flags[2].x){float opacity=p.x*fog[25].w;color.rgb=lerp(color.rgb,fog[25].rgb,opacity);}
            }
            return color;
        })","ps_5_0");
    Check(NativeAtmosphereProgram(parent->GetBufferPointer(),parent->GetBufferSize(),owned),"fog child replaced without rejecting or disabling its lighting parent");
    auto aoSource=compile(R"(
        cbuffer View:register(b4){uint4 flags[2];}
        Texture2D<float4> visibility:register(t7);SamplerState visibilitySampler:register(s3);
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 color=float4(.2,.4,.6,.37);
            [branch]if(flags[1].x)color.rgb*=visibility.SampleLevel(visibilitySampler,p.xy/8,0).rgb;
            if(p.x>4)discard;
            return color;
        })","ps_5_0");
    Check(!NativeAtmosphereProgram(aoSource->GetBufferPointer(),aoSource->GetBufferSize(),twice),"AO replacement requires an explicit owned-AO request");
    Check(NativeAtmosphereProgram(aoSource->GetBufferPointer(),aoSource->GetBufferSize(),owned,true),"recognize verified native AO guard and t7 sample");
    native.Reset();modified.Reset();
    Check(SUCCEEDED(device->CreatePixelShader(aoSource->GetBufferPointer(),aoSource->GetBufferSize(),nullptr,&native))&&
          SUCCEEDED(device->CreatePixelShader(owned.data(),owned.size(),nullptr,&modified)),"owned AO shader validates");
    std::array<uint32_t,8> aoFlags{};view.Reset();cb.ByteWidth=sizeof(aoFlags);data.pSysMem=aoFlags.data();
    Check(SUCCEEDED(device->CreateBuffer(&cb,&data,&view)),"disabled engine AO constants");
    buffer=view.Get();ctx->PSSetConstantBuffers(4,1,&buffer);
    auto visibilityDesc=td;visibilityDesc.Width=visibilityDesc.Height=1;visibilityDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    visibilityDesc.Usage=D3D11_USAGE_DEFAULT;visibilityDesc.CPUAccessFlags=0;
    const float visibilityPixel[4]{.25f,.25f,.25f,.25f};D3D11_SUBRESOURCE_DATA visibilityData{visibilityPixel,16,0};
    ComPtr<ID3D11Texture2D> visibility;ComPtr<ID3D11ShaderResourceView> visibilityView;
    Check(SUCCEEDED(device->CreateTexture2D(&visibilityDesc,&visibilityData,&visibility))&&
          SUCCEEDED(device->CreateShaderResourceView(visibility.Get(),nullptr,&visibilityView)),"owned AO input texture");
    D3D11_SAMPLER_DESC aoSamplerDesc{};aoSamplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    aoSamplerDesc.AddressU=aoSamplerDesc.AddressV=aoSamplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> aoSampler;Check(SUCCEEDED(device->CreateSamplerState(&aoSamplerDesc,&aoSampler)),"AO fixture sampler");
    auto input=visibilityView.Get();ctx->PSSetShaderResources(7,1,&input);auto sampler=aoSampler.Get();ctx->PSSetSamplers(3,1,&sampler);
    const auto nativeAoOff=draw(native.Get()),ownedAoOn=draw(modified.Get());
    Check(std::abs(nativeAoOff[0]-.2f)<1e-6f&&std::abs(ownedAoOn[0]-.05f)<1e-6f&&ownedAoOn[3]==nativeAoOff[3],
          "owned AO affects native material even when engine AO is disabled and preserves alpha");
    ctx->ClearRenderTargetView(target.Get(),clear);draw(modified.Get());
    Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map)),"owned AO discard read");
    std::memcpy(discarded.data(),static_cast<const uint8_t*>(map.pData)+7*16,16);ctx->Unmap(read.Get(),0);
    Check(discarded==std::array<float,4>{clear[0],clear[1],clear[2],clear[3]},"owned AO retains material discard coverage");
}
void Run(unsigned samples,bool world=false,bool a2c=false){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"device");
    auto compile=[&](const char* source,const char* profile){ComPtr<ID3DBlob> code,error;
        const auto hr=CompileEnvironmentShader(source,strlen(source),"NativeMaterialTest",nullptr,"Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"compile");return code;};
    auto compressedPost=compile(R"(
        Texture2D<float4> color:register(t12);SamplerState s:register(s0);
        float4 Main(float4 p:SV_Position):SV_Target {
            float4 c=color.SampleLevel(s,p.xy,0);
            float inverse=rcp(1-max(c.r,max(c.g,c.b)));
            return float4(c.rgb*inverse,c.a);
        })","ps_5_0");
    Check(NativePostUsesCompressedHdr(compressedPost->GetBufferPointer(),compressedPost->GetBufferSize()),
          "recognize native max-channel HDR decode before composing linear light");
    for(size_t end=0;end<compressedPost->GetBufferSize();++end)
        Check(!NativePostUsesCompressedHdr(compressedPost->GetBufferPointer(),end),"truncated post shader rejected");
    auto vsCode=compile(R"(
        struct V{float4 p:SV_Position;float3 n:TEXCOORD0;float2 uv:TEXCOORD1;};
        V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V v;v.p=float4(p[id],.5,1);v.n=float3(p[id]*.2,1);v.uv=p[id]*.5+.5;return v;})","vs_5_0");
    auto psCode=compile(R"(
        cbuffer PerViewLighting:register(b3){float4 lighting[1990];}
        cbuffer Selection:register(b0){uint selector;}
        struct Fixture {float4 data[30];};StructuredBuffer<Fixture> fixtures:register(t17);
        Texture2D<float4> material:register(t0);SamplerState sampleMaterial:register(s0);
        // Mirrors the shipped baked/shadow bindings so the derived ABI is exercised:
        // three Texture2DArray lightmap inputs at t1-t3 and one comparison sampler.
        Texture2DArray<float4> bakedDirect:register(t1);
        Texture2DArray<float4> bakedIrradiance:register(t2);
        Texture2DArray<float4> bakedBentNormal:register(t3);
        Texture2D<float> shadowAtlas:register(t6);SamplerComparisonState compareShadow:register(s7);
        float4 Main(float4 p:SV_Position,float3 normal:TEXCOORD0,float2 uv:TEXCOORD1):SV_Target{
            if(uv.x<.25)discard;
            float3 n=normalize(normal);
            float facing=dot(n,lighting[1945].xyz);
            float visibility=saturate(uv.x);
            float3 radiance=visibility*lighting[1946].xyz;
            float roughness=material.Sample(sampleMaterial,uv).r;
            float spec=pow(saturate(dot(n,normalize(lighting[1945].xyz+float3(.1,0,1)))),lerp(128,4,roughness));
            float3 diffuse=float3(.2,.1,.05)*facing;
            float3 baked=bakedDirect.Sample(sampleMaterial,float3(uv,0)).rgb
                +bakedIrradiance.Sample(sampleMaterial,float3(uv,0)).rgb
                *bakedBentNormal.Sample(sampleMaterial,float3(uv,0)).r;
            float occlusion=shadowAtlas.SampleCmpLevelZero(compareShadow,uv,p.z);
            float3 local=0;
            [loop]for(uint word=0;word<asuint(lighting[19].z);++word)local+=fixtures[word].data[10].rgb;
            // Keep the fixture term exactly `local`: the baseline-minus-without-fixtures
            // difference measures it directly, so anything multiplied into it is measured too.
            // The baked and occlusion samples cancel in that difference.
            return float4(.1+lighting[selector%1990].xyz*.01+baked*.01+occlusion*.02+local+radiance*(diffuse+spec),.25);
        })","ps_5_0");
    if(!HasNativeSunMaterialAbi(psCode->GetBufferPointer(),psCode->GetBufferSize())){
        auto disassemble=reinterpret_cast<pD3DDisassemble>(GetProcAddress(EnvironmentShaderCompiler(),"D3DDisassemble"));
        ComPtr<ID3DBlob> listing;if(disassemble&&SUCCEEDED(disassemble(psCode->GetBufferPointer(),psCode->GetBufferSize(),0,nullptr,&listing)))
            std::cerr<<static_cast<const char*>(listing->GetBufferPointer());
        Check(false,"native sun ABI fixture recognized");
    }
    NativeSunAbiInfo fixtureAbi{};HasNativeSunMaterialAbi(psCode->GetBufferPointer(),psCode->GetBufferSize(),&fixtureAbi);
    Check((fixtureAbi.flags&32)!=0,"clustered fixture loop ABI recognized");
    // The light SRV slot, comparison sampler and baked array count come from the
    // bytecode itself, so a material permutation nobody listed by hash still works.
    Check(fixtureAbi.lightSlot==17,"light record slot derived from the declaration");
    Check(fixtureAbi.comparisonSampler==7,"shadow comparison sampler derived from the declaration");
    Check(fixtureAbi.lightmapArrayMask==7,"baked lightmap arrays detected at t1-t3");
    // The reflection carrier gets its own program rather than more terms in the one above: that
    // fixture's sun markers are matched as exact instruction patterns, and adding an addend to
    // its final expression is enough to make the compiler fold the radiance multiply into a MAD
    // and stop the whole ABI deriving.
    {
        auto cubeCode=compile(R"(
            TextureCube<float4> envmap:register(t4);
            TextureCubeArray<float4> envmapArray:register(t5);
            Texture2DArray<float4> bakedIrradiance:register(t2);
            SamplerState wrap:register(s0);
            float4 Main(float4 p:SV_Position,float3 normal:TEXCOORD0,float2 uv:TEXCOORD1):SV_Target{
                float3 n=normalize(normal);
                return float4(envmap.Sample(wrap,n).rgb+envmapArray.Sample(wrap,float4(n,0)).rgb
                    +bakedIrradiance.Sample(wrap,float3(uv,0)).rgb,1);
            })","ps_5_0");
        NativeSunAbiInfo cubeAbi{};
        HasNativeSunMaterialAbi(cubeCode->GetBufferPointer(),cubeCode->GetBufferSize(),&cubeAbi);
        Check(cubeAbi.cubeSlotMask==(1u<<4),"reflection cube derived at its declared slot");
        Check(cubeAbi.cubeArraySlotMask==(1u<<5),"reflection cube array kept apart from the plain cube");
        Check(!(cubeAbi.cubeSlotMask&cubeAbi.cubeArraySlotMask),"a slot never counts as both dimensions");
        // A cube is not a lightmap array and must not be mistaken for one, or the unbake would
        // blacken a reflection slot with a Texture2DArray view the shader cannot accept.
        Check(cubeAbi.lightmapArrayMask==2,"cubes do not leak into the lightmap array mask");
    }
    // The unbake replaces exactly the radiance inputs: t1 goes black, t2 takes the fill, and t3
    // (bent normals) is left to the engine. A program without an array in a slot never has that
    // slot touched, whatever else it declares.
    {const auto all=NeutralizeLightmapArrays(fixtureAbi.lightmapArrayMask);
     Check(all.blackSlots==(1u<<1)&&all.fillSlots==(1u<<2),"unbake blackens t1 and fills t2 for a fully baked program");
     Check(!(all.blackSlots&(1u<<3))&&!(all.fillSlots&(1u<<3)),"unbake never touches the bent-normal array at t3");
     const auto irradianceOnly=NeutralizeLightmapArrays(2);
     Check(irradianceOnly.blackSlots==0&&irradianceOnly.fillSlots==(1u<<2),"a program with only an irradiance array gets only the fill");
     const auto normalsOnly=NeutralizeLightmapArrays(4);
     Check(!normalsOnly.Any(),"a bent-normal-only program is left alone");
     Check(!NeutralizeLightmapArrays(0).Any(),"an unbaked program has nothing to replace");}
    // A changed native fixture layout must retain the ordinary material baseline.
    // Keep sun ABI valid: rejecting fixture extraction must not reject sun replay.
    std::vector<uint8_t> changed(static_cast<const uint8_t*>(psCode->GetBufferPointer()),static_cast<const uint8_t*>(psCode->GetBufferPointer())+psCode->GetBufferSize());
    const uint32_t fixtureDeclaration[]={0x040000a2,0x00107000,17,480};bool changedStride=false;
    for(size_t i=0;i+sizeof(fixtureDeclaration)<=changed.size();i+=4)if(!std::memcmp(changed.data()+i,fixtureDeclaration,sizeof(fixtureDeclaration))){
        const uint32_t stride=496;std::memcpy(changed.data()+i+12,&stride,4);changedStride=true;break;}
    Check(changedStride,"fixture declaration found in compiled program");
    auto relocated=changed;
    for(size_t i=0;i+16<=relocated.size();i+=4)if(!std::memcmp(relocated.data()+i,fixtureDeclaration,12)){
        const uint32_t slot=20,stride=480;std::memcpy(relocated.data()+i+8,&slot,4);std::memcpy(relocated.data()+i+12,&stride,4);break;}
    Check(HasNativeSunMaterialAbi(relocated.data(),relocated.size(),&fixtureAbi)&&(fixtureAbi.flags&32),"fixture table binding may move between material permutations");
    Check(fixtureAbi.lightSlot==20,"derived light slot follows a relocated fixture table");
    Check(HasNativeSunMaterialAbi(changed.data(),changed.size(),&fixtureAbi)&&!(fixtureAbi.flags&32),"unknown fixture stride keeps sun support without extracting fixtures");
    Check(fixtureAbi.lightSlot==0&&fixtureAbi.comparisonSampler==0,"an unrecognised fixture stride derives no bindings");
    for(size_t i=0;i<psCode->GetBufferSize();++i)Check(!HasNativeSunMaterialAbi(psCode->GetBufferPointer(),i),"truncated native ABI rejected");
    std::vector<uint8_t> coverageCode;Check(NativeCoverageProgram(psCode->GetBufferPointer(),psCode->GetBufferSize(),coverageCode),"native coverage instrumentation");
    for(size_t end=0;end<psCode->GetBufferSize();++end){std::vector<uint8_t> bad;Check(!NativeCoverageProgram(psCode->GetBufferPointer(),end,bad),"truncated coverage container rejected");}
    ComPtr<ID3D11PixelShader> coveragePs;
    Check(SUCCEEDED(device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coveragePs)),"validate native coverage bytecode");
    auto unrelated=compile("float4 Main():SV_Target{return 1;}","ps_5_0");
    Check(!HasNativeSunMaterialAbi(unrelated->GetBufferPointer(),unrelated->GetBufferSize()),"unrelated PS rejected");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"VS");
    const char* name=world?"csgo_lightmappedgeneric.vfx_vs":"csgo_weapon.vfx_vs";vs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(name)),name);
    Check(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)),"PS");
    NoteNativeMaterialPixelShader(ps.Get(),psCode->GetBufferPointer(),psCode->GetBufferSize());
    // The live engine allocates 2014 entries for this 1990-entry shader ABI.
    std::array<float,2014*4> lighting{};
    for(unsigned i=0;i<lighting.size();++i)lighting[i]=.001f*(1+i%31);
    lighting[1945*4]=lighting[1945*4+1]=0;lighting[1945*4+2]=1;lighting[1945*4+3]=.37f;
    lighting[1946*4]=lighting[1946*4+1]=lighting[1946*4+2]=0;lighting[1953*4]=3;lighting[0]=.23f;
    const unsigned oneWord=1;std::memcpy(&lighting[19*4+2],&oneWord,4);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(lighting);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{lighting.data(),0,0};ComPtr<ID3D11Buffer> lightBuffer;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&data,&lightBuffer)),"native light CB");
    bd.ByteWidth=16;std::array<unsigned,4> selection{};data.pSysMem=selection.data();ComPtr<ID3D11Buffer> selectionBuffer;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&data,&selectionBuffer)),"selection CB");
    std::array<float,120> fixtureData{};fixtureData[40]=.3f;fixtureData[41]=.08f;fixtureData[42]=.02f;
    D3D11_BUFFER_DESC fixtureDesc{};fixtureDesc.ByteWidth=480;fixtureDesc.StructureByteStride=480;
    fixtureDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;fixtureDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA fixtureInput{fixtureData.data(),0,0};ComPtr<ID3D11Buffer> fixtureBuffer;ComPtr<ID3D11ShaderResourceView> fixtureSrv;
    Check(SUCCEEDED(device->CreateBuffer(&fixtureDesc,&fixtureInput,&fixtureBuffer))&&SUCCEEDED(device->CreateShaderResourceView(fixtureBuffer.Get(),nullptr,&fixtureSrv)),"fixture buffer");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=512;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=samples;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color,depth;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&rtv)),"native color");
    auto depthDesc=td;depthDesc.Format=DXGI_FORMAT_D32_FLOAT;depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    Check(SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&depth))&&SUCCEEDED(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv)),"native depth");
    D3D11_TEXTURE2D_DESC materialDesc=td;materialDesc.Width=materialDesc.Height=1;materialDesc.SampleDesc.Count=1;materialDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::array<float,4> rough{{.1f,0,0,1}};D3D11_SUBRESOURCE_DATA materialData{rough.data(),16,0};ComPtr<ID3D11Texture2D> materialTexture;ComPtr<ID3D11ShaderResourceView> materialSrv;
    Check(SUCCEEDED(device->CreateTexture2D(&materialDesc,&materialData,&materialTexture))&&SUCCEEDED(device->CreateShaderResourceView(materialTexture.Get(),nullptr,&materialSrv)),"native material texture");
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)),"native sampler");
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=true;
    ComPtr<ID3D11RasterizerState> raster;Check(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)),"raster");
    D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].RenderTargetWriteMask=7;blendDesc.AlphaToCoverageEnable=a2c;ComPtr<ID3D11BlendState> blend;
    Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&blend)),"RGB-only native write mask");
    auto bind=[&]{ctx->ClearState();auto target=rtv.Get();ctx->OMSetRenderTargets(1,&target,dsv.Get());const float black[4]{};ctx->ClearRenderTargetView(target,black);ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        D3D11_VIEWPORT vp{0,0,512,512,0,world?.95f:.1f};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster.Get());ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
        auto cb=lightBuffer.Get();ctx->PSSetConstantBuffers(3,1,&cb);cb=selectionBuffer.Get();ctx->PSSetConstantBuffers(0,1,&cb);
        auto fixtureView=fixtureSrv.Get();ctx->PSSetShaderResources(17,1,&fixtureView);
        auto texture=materialSrv.Get();auto sample=sampler.Get();ctx->PSSetShaderResources(0,1,&texture);ctx->PSSetSamplers(0,1,&sample);};
    td.SampleDesc.Count=1;td.BindFlags=0;ComPtr<ID3D11Texture2D> resolved;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&resolved)),"MSAA resolve texture");
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> readback;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&readback)),"color readback");
    auto pixel=[&](ID3D11Resource* resource){ComPtr<ID3D11Texture2D> texture;Check(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))),"pixel texture");D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);if(desc.SampleDesc.Count>1){ctx->ResolveSubresource(resolved.Get(),0,resource,0,td.Format);ctx->CopyResource(readback.Get(),resolved.Get());}
        else ctx->CopyResource(readback.Get(),resource);D3D11_MAPPED_SUBRESOURCE map{};
        Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)),"read material pixel");
        std::array<float,4> result{};std::memcpy(result.data(),static_cast<const char*>(map.pData)+256*map.RowPitch+256*16,16);ctx->Unmap(readback.Get(),0);return result;};
    Frame frame;frame.active=true;frame.scene.enabled=true;auto& climate=frame.scene.climate;
    climate[CL_enabled]=climate[CL_relighting]=climate[CL_sunPower]=climate[CL_sunRelight]=1;climate[CL_sunElevation]=90;climate[CL_sunAzimuth]=0;
    Check(!BindNativeMaterial(ctx.Get(),color.Get(),depth.Get(),frame),"empty frame has no native material result");
    climate[CL_ambient]=.1f;
    // The Environment sun override must affect only validated main-world draws,
    // preserve every unrelated lighting constant, and never leak into the skybox
    // or the independently owned relighting pipeline.
    PrepareNativeMaterialFrame(frame);bind();
    auto checkSourceSun=[&](bool suppressed){
        ComPtr<ID3D11Buffer> bound;ctx->PSGetConstantBuffers(3,1,&bound);
        if(!suppressed){Check(bound.Get()==lightBuffer.Get(),"unowned source lighting untouched");return;}
        Check(bound.Get()!=lightBuffer.Get(),"source sun uses private buffer");
        D3D11_BUFFER_DESC desc{};bound->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&staging)),"source sun readback allocation");
        ctx->CopyResource(staging.Get(),bound.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"source sun readback");
        const float* values=static_cast<const float*>(mapped.pData);
        for(unsigned i=0;i<lighting.size();++i)
            Check(values[i]==((i>=1946*4&&i<1946*4+3)?0:lighting[i]),"source override changes only sun radiance");
        ctx->Unmap(staging.Get(),0);
    };
    {NativeMaterialSunScope scope(ctx.Get());checkSourceSun(true);}
    checkSourceSun(false);
    D3D11_VIEWPORT skyViewport{0,0,512,512,.95f,1};ctx->RSSetViewports(1,&skyViewport);
    {NativeMaterialSunScope scope(ctx.Get());checkSourceSun(false);}
    bind();frame.relight.enabled=true;PrepareNativeMaterialFrame(frame);
    {NativeMaterialSunScope scope(ctx.Get());checkSourceSun(false);}
    frame.relight.enabled=false;PrepareNativeMaterialFrame(frame);
    bool readOnlyDepth=false, overlay=false;ComPtr<ID3D11DepthStencilState> readOnlyState;D3D11_DEPTH_STENCIL_DESC readOnlyDesc{};
    readOnlyDesc.DepthEnable=true;readOnlyDesc.DepthFunc=D3D11_COMPARISON_EQUAL;readOnlyDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    Check(SUCCEEDED(device->CreateDepthStencilState(&readOnlyDesc,&readOnlyState)),"native read-only depth state");
    auto render=[&]{NextNativeMaterialFrame();PrepareNativeMaterialFrame(frame);bind();ctx->Draw(3,0);
        if(readOnlyDepth)ctx->OMSetDepthStencilState(readOnlyState.Get(),0);
        if(overlay)ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        const auto before=pixel(color.Get());Check(BeginNativeMaterialDraw(ctx.Get()),"begin isolated native replay");
        ComPtr<ID3D11Buffer> patched;ctx->PSGetConstantBuffers(3,1,&patched);
        D3D11_BUFFER_DESC copyDesc{};patched->GetDesc(&copyDesc);copyDesc.BindFlags=0;copyDesc.Usage=D3D11_USAGE_STAGING;copyDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> patchReadback;Check(SUCCEEDED(device->CreateBuffer(&copyDesc,nullptr,&patchReadback)),"private lighting readback");
        ctx->CopyResource(patchReadback.Get(),patched.Get());D3D11_MAPPED_SUBRESOURCE patchMap{};
        Check(SUCCEEDED(ctx->Map(patchReadback.Get(),0,D3D11_MAP_READ,0,&patchMap)),"read private lighting");
        const auto* actual=static_cast<const float*>(patchMap.pData);
        for(unsigned i=0;i<lighting.size();++i){
            const bool replaced=(i>=1945*4&&i<1945*4+3)||(i>=1946*4&&i<1946*4+3)||i==1953*4;
            if(!replaced||frame.relight.reference)Check(actual[i]==lighting[i],"private light patch preserves every unrelated constant");
        }
        Check(actual[1953*4]==(frame.relight.reference?lighting[1953*4]:0),"native cascades preserved only in reference replay");
        ctx->Unmap(patchReadback.Get(),0);
        ctx->Draw(3,0);Check(BeginNativeMaterialBaseline(ctx.Get()),"matched native baseline");ctx->Draw(3,0);if(BeginNativeMaterialWithoutFixtures(ctx.Get()))ctx->Draw(3,0);Check(BeginNativeMaterialCoverage(ctx.Get()),"separate coverage pass");ctx->Draw(3,0);EndNativeMaterialDraw(ctx.Get());
        ComPtr<ID3D11DepthStencilState> restoredDepth;ctx->OMGetDepthStencilState(&restoredDepth,nullptr);
        Check(restoredDepth.Get()==(readOnlyDepth?readOnlyState.Get():nullptr),"native depth state restored");
        const auto after=pixel(color.Get());Check(before==after,"native target unchanged by replay");
        ComPtr<ID3D11Buffer> restoredCb;ctx->PSGetConstantBuffers(3,1,&restoredCb);Check(restoredCb.Get()==lightBuffer.Get(),"native lighting CB binding restored");
        ComPtr<ID3D11RenderTargetView> restoredTarget;ctx->OMGetRenderTargets(1,&restoredTarget,nullptr);Check(restoredTarget.Get()==rtv.Get(),"native target binding restored");
        ComPtr<ID3D11BlendState> restoredBlend;float factors[4];UINT mask;ctx->OMGetBlendState(&restoredBlend,factors,&mask);Check(restoredBlend.Get()==blend.Get()&&mask==~0u,"native blend restored");
        Check(BindNativeMaterial(ctx.Get(),color.Get(),depth.Get(),frame),"bind matching native replay");ComPtr<ID3D11ShaderResourceView> coverage;ctx->PSGetShaderResources(23,1,&coverage);
        ComPtr<ID3D11Resource> coverageResource;coverage->GetResource(&coverageResource);ComPtr<ID3D11Texture2D> coverageTexture;coverageResource.As(&coverageTexture);
        D3D11_TEXTURE2D_DESC coverageDesc{};coverageTexture->GetDesc(&coverageDesc);coverageDesc.SampleDesc={1,0};coverageDesc.BindFlags=0;
        ComPtr<ID3D11Texture2D> coverageSingle,coverageRead;
        Check(SUCCEEDED(device->CreateTexture2D(&coverageDesc,nullptr,&coverageSingle)),"coverage resolve");
        coverageDesc.Usage=D3D11_USAGE_STAGING;coverageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateTexture2D(&coverageDesc,nullptr,&coverageRead)),"coverage staging");
        if(samples>1)ctx->ResolveSubresource(coverageSingle.Get(),0,coverageResource.Get(),0,DXGI_FORMAT_R8_UNORM);else ctx->CopyResource(coverageSingle.Get(),coverageResource.Get());
        ctx->CopyResource(coverageRead.Get(),coverageSingle.Get());D3D11_MAPPED_SUBRESOURCE coverageMap{};
        Check(SUCCEEDED(ctx->Map(coverageRead.Get(),0,D3D11_MAP_READ,0,&coverageMap)),"coverage read");
        const unsigned covered=static_cast<const unsigned char*>(coverageMap.pData)[256*coverageMap.RowPitch+256];
        Check(a2c?(covered>0&&covered<255):covered==255,"coverage retains native samples independently of alpha payload");Check(static_cast<const unsigned char*>(coverageMap.pData)[256*coverageMap.RowPitch+64]==0,"native discard remains a hole in coverage");ctx->Unmap(coverageRead.Get(),0);
        ComPtr<ID3D11ShaderResourceView> replay;ctx->PSGetShaderResources(22,1,&replay);ComPtr<ID3D11Resource> replayResource;replay->GetResource(&replayResource);return pixel(replayResource.Get());};
    render();
    ComPtr<ID3D11ShaderResourceView> baseView,withoutView;ctx->PSGetShaderResources(24,1,&baseView);ctx->PSGetShaderResources(25,1,&withoutView);
    Check(baseView&&withoutView&&baseView.Get()!=withoutView.Get(),"separate fixture baseline is bound when ambient changes");
    ComPtr<ID3D11Resource> baseResource,withoutResource;baseView->GetResource(&baseResource);withoutView->GetResource(&withoutResource);
    const auto basePixel=pixel(baseResource.Get()),withoutPixel=pixel(withoutResource.Get());
    const float coverageFraction=a2c?.25f:1.f;
    Check(std::abs((basePixel[0]-withoutPixel[0])-.3f*coverageFraction)<.0001f,"matched HDR passes isolate native fixture energy including coverage");
    Check(std::abs(withoutPixel[0]-(.1f+.0023f)*coverageFraction)<.0001f,"indirect/emission baseline remains separate from native direct fixtures");
    climate[CL_ambient]=1;render();
    baseView.Reset();withoutView.Reset();ctx->PSGetShaderResources(24,1,&baseView);ctx->PSGetShaderResources(25,1,&withoutView);
    Check(baseView.Get()==withoutView.Get(),"neutral ambient skips extra fixture replay");
    climate[CL_ambient]=.1f;
    climate[CL_sunElevation]=-5;climate[CL_sunPower]=8;auto nightPowered=render();
    climate[CL_sunPower]=0;auto nightDisabled=render();
    Check(nightPowered==nightDisabled,"Native material cannot receive sun energy from below the horizon");
    climate[CL_sunElevation]=90;climate[CL_sunPower]=1;
    auto lit=render();Check(lit[0]>(a2c?.06f:.3f)&&lit[3]==0,"native RGB write mask and independent coverage preserved");
    {
        frame.relight.enabled=frame.relight.reference=true;
        const auto reference=render();
        Check(reference==basePixel,"reference replay preserves native lighting and material output exactly");
        frame.relight.enabled=frame.relight.reference=false;
        lit=render();
    }
    // Exercise the live HDR resolve -> native tonemap -> multisampled LDR handoff.
    auto toneCode=compile(R"(
        Texture2D<float4> hdr:register(t12);
        float4 Main(float4 p:SV_Position):SV_Target{float4 c=hdr.Load(int3(p.xy,0));return float4(c.rgb/(1+c.rgb),c.a);}
    )","ps_5_0");
    ComPtr<ID3D11PixelShader> tonePs;ComPtr<ID3D11VertexShader> toneVs;
    Check(SUCCEEDED(device->CreatePixelShader(toneCode->GetBufferPointer(),toneCode->GetBufferSize(),nullptr,&tonePs)),"native tonemap PS");
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&toneVs)),"native tonemap VS");
    const char* toneName="post_process.vfx_ps";tonePs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(toneName)),toneName);
    toneName="post_process.vfx_vs";toneVs->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(toneName)),toneName);
    D3D11_TEXTURE2D_DESC toneDesc{};toneDesc.Width=toneDesc.Height=512;toneDesc.ArraySize=toneDesc.MipLevels=toneDesc.SampleDesc.Count=1;
    toneDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;toneDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> nativeResolved;ComPtr<ID3D11ShaderResourceView> nativeResolvedSrv;
    Check(SUCCEEDED(device->CreateTexture2D(&toneDesc,nullptr,&nativeResolved))&&SUCCEEDED(device->CreateShaderResourceView(nativeResolved.Get(),nullptr,&nativeResolvedSrv)),"native resolved HDR");
    std::vector<unsigned short> halfData(512*512*4,0x2e66);ctx->UpdateSubresource(nativeResolved.Get(),0,nullptr,halfData.data(),512*8,0);
    toneDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;toneDesc.SampleDesc.Count=samples;toneDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> nativeLdr,ldrResolved,ldrStaging;ComPtr<ID3D11RenderTargetView> nativeLdrRtv;
    Check(SUCCEEDED(device->CreateTexture2D(&toneDesc,nullptr,&nativeLdr))&&SUCCEEDED(device->CreateRenderTargetView(nativeLdr.Get(),nullptr,&nativeLdrRtv)),"native LDR output");
    toneDesc.SampleDesc.Count=1;toneDesc.BindFlags=0;Check(SUCCEEDED(device->CreateTexture2D(&toneDesc,nullptr,&ldrResolved)),"LDR resolve");
    toneDesc.Usage=D3D11_USAGE_STAGING;toneDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&toneDesc,nullptr,&ldrStaging)),"LDR readback");
    auto ldrPixel=[&](ID3D11Resource* resource){if(samples>1){ctx->ResolveSubresource(ldrResolved.Get(),0,resource,0,toneDesc.Format);ctx->CopyResource(ldrStaging.Get(),ldrResolved.Get());}else ctx->CopyResource(ldrStaging.Get(),resource);
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(ldrStaging.Get(),0,D3D11_MAP_READ,0,&map)),"read tonemap pixel");
        auto value=static_cast<const unsigned char*>(map.pData)[256*map.RowPitch+256*4];ctx->Unmap(ldrStaging.Get(),0);return value;};
    auto ldrTarget=nativeLdrRtv.Get();ctx->OMSetRenderTargets(1,&ldrTarget,nullptr);auto hdrInput=nativeResolvedSrv.Get();ctx->PSSetShaderResources(12,1,&hdrInput);
    ctx->VSSetShader(toneVs.Get(),nullptr,0);ctx->PSSetShader(tonePs.Get(),nullptr,0);D3D11_VIEWPORT toneVp{0,0,512,512,0,.95f};ctx->RSSetViewports(1,&toneVp);
    ctx->OMSetBlendState(nullptr,nullptr,~0u);ctx->Draw(3,0);const auto originalLdr=ldrPixel(nativeLdr.Get());
    Check(BeginNativeMaterialDraw(ctx.Get()),"begin native tonemap replay");ctx->Draw(3,0);EndNativeMaterialDraw(ctx.Get());
    Check(ldrPixel(nativeLdr.Get())==originalLdr,"native LDR unchanged");
    ComPtr<ID3D11ShaderResourceView> restoredInput;ctx->PSGetShaderResources(12,1,&restoredInput);Check(restoredInput.Get()==nativeResolvedSrv.Get(),"native tonemap input restored");
    Check(BindNativeMaterial(ctx.Get(),nativeLdr.Get(),depth.Get(),frame),"tonemapped native material binds to final target");
    ComPtr<ID3D11ShaderResourceView> tonedView;ctx->PSGetShaderResources(22,1,&tonedView);ComPtr<ID3D11Resource> tonedResource;tonedView->GetResource(&tonedResource);
    Check(std::abs(float(ldrPixel(tonedResource.Get()))/255-lit[0]/(1+lit[0]))<.006f,"material follows native nonlinear tonemapping");
    Check(!BindNativeMaterial(ctx.Get(),color.Get(),depth.Get(),frame),"tonemapped material rejects unrelated target");
    frame.hdr=true;
    Check(!BindNativeMaterial(ctx.Get(),resolved.Get(),depth.Get(),frame),"HDR copy without provenance rejected");
    Check(!BindNativeMaterial(ctx.Get(),resolved.Get(),depth.Get(),frame,nativeResolved.Get()),"HDR copy from wrong source rejected");
    Check(!BindNativeMaterial(ctx.Get(),nativeLdr.Get(),depth.Get(),frame,color.Get()),"LDR cannot masquerade as HDR copy");
    Check(!BindNativeMaterial(ctx.Get(),resolved.Get(),nullptr,frame,color.Get()),"HDR copy with wrong depth rejected");
    Check(BindNativeMaterial(ctx.Get(),resolved.Get(),depth.Get(),frame,color.Get()),"proven HDR copy binds linear native material after tonemap");
    ComPtr<ID3D11ShaderResourceView> linearView;ctx->PSGetShaderResources(22,1,&linearView);ComPtr<ID3D11Resource> linearResource;linearView->GetResource(&linearResource);
    const auto linear=pixel(linearResource.Get());Check(std::abs(linear[0]-lit[0])<.00001f&&linear[3]==0,"HDR material preserves linear radiance and authored alpha");
    frame.hdr=false;
    Check(BindNativeMaterial(ctx.Get(),nativeLdr.Get(),depth.Get(),frame),"HDR export leaves LDR replay available");
    readOnlyDepth=true;auto prepassLit=render();Check(std::abs(lit[0]-prepassLit[0])<.00001f,"read-only native material pass preserves lighting");
    rough[0]=.8f;ctx->UpdateSubresource(materialTexture.Get(),0,nullptr,rough.data(),16,0);auto roughLit=render();
    Check(std::abs(lit[0]-roughLit[0])>.005f,"native roughness texture still controls highlight response");
    climate[CL_sunPower]=0;Check(!BindNativeMaterial(ctx.Get(),color.Get(),depth.Get(),frame),"changed sun cannot use stale material result");
    // No Bind call publishes the new light: the pass snapshot must suffice.
    climate[CL_sunPower]=.25f;auto edited=render();Check(edited[0]<roughLit[0],"first frame of a sun edit uses the current snapshot");
    climate[CL_sunPower]=0;
    auto dark=render();Check(dark[0]<lit[0]-(a2c?.02f:.1f),"zero power removes direct light");
    // A translucent biased overlay passes against a deeper supporting surface.
    // Forcing EQUAL would lose it, and forcing opaque blending would quadruple RGB.
    overlay=true; readOnlyDesc.DepthFunc=D3D11_COMPARISON_LESS;
    readOnlyState.Reset();Check(SUCCEEDED(device->CreateDepthStencilState(&readOnlyDesc,&readOnlyState)),"overlay read-only comparison");
    blendDesc.RenderTarget[0].BlendEnable=true;blendDesc.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;blendDesc.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;blendDesc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blend.Reset();Check(SUCCEEDED(device->CreateBlendState(&blendDesc,&blend)),"native overlay blend");
    rasterDesc.DepthBias=-64;raster.Reset();Check(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)),"native overlay depth bias");
    rough[0]=.1f;ctx->UpdateSubresource(materialTexture.Get(),0,nullptr,rough.data(),16,0);climate[CL_sunPower]=1;
    const auto translucent=render();Check(std::abs(translucent[0]-lit[0]*.25f)<.0001f,"overlay retains opacity and biased read-only depth");
    bd.ByteWidth=sizeof(lighting);bd.BindFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> readCb;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&readCb)),"CB readback");ctx->CopyResource(readCb.Get(),lightBuffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(ctx->Map(readCb.Get(),0,D3D11_MAP_READ,0,&mapped)),"read native CB");Check(!std::memcmp(mapped.pData,lighting.data(),sizeof(lighting)),"engine light values unchanged");ctx->Unmap(readCb.Get(),0);
    render();
    bind();auto compositeTarget=nativeLdrRtv.Get();ctx->OMSetRenderTargets(1,&compositeTarget,nullptr);auto compositeInput=nativeResolvedSrv.Get();ctx->PSSetShaderResources(12,1,&compositeInput);
    ctx->VSSetShader(toneVs.Get(),nullptr,0);ctx->PSSetShader(tonePs.Get(),nullptr,0);ctx->RSSetViewports(1,&toneVp);
    ctx->OMSetBlendState(nullptr,nullptr,~0u);ctx->Draw(3,0);
    SetNativeMaterialHdrPass([](ID3D11DeviceContext* context,ID3D11RenderTargetView* target,ID3D11DepthStencilView* depth,ID3D11Texture2D* source,const Frame& snapshot){
        Check(snapshot.hdr&&depth&&source,"pre-tonemap callback receives HDR frame and native provenance");
        const float linear[4]={1,1,1,1};context->ClearRenderTargetView(target,linear);return true;
    });
    Check(BeginNativeMaterialDraw(ctx.Get()),"pre-tonemap HDR composition");ctx->Draw(3,0);EndNativeMaterialDraw(ctx.Get());
    Check(std::abs(int(ldrPixel(nativeLdr.Get()))-128)<=1,"composed linear HDR goes through native tonemap once");
    Check(NativeMaterialComposited(frame),"completed generation prevents a second LDR effect pass");
    ++frame.generation;Check(!NativeMaterialComposited(frame),"different generation cannot claim composition");--frame.generation;
    SetNativeMaterialHdrPass(nullptr);
    frame.active=false;PrepareNativeMaterialFrame(frame);Check(!BindNativeMaterial(ctx.Get(),color.Get(),depth.Get(),frame),"disabled environment unbinds native replay");Check(!BeginNativeMaterialDraw(ctx.Get()),"disabled environment does not replay");
    ResetNativeMaterial();std::cout<<"Native material lighting tests passed, samples="<<samples<<" world="<<world<<"\n";
}

void CheckProofRebase(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"proof device");
    ComPtr<ID3DBlob> code,error;ComPtr<ID3D11ComputeShader> shader;
    Check(SUCCEEDED(CompileEnvironmentShader(LightingProofRebaseShader,sizeof(LightingProofRebaseShader)-1,"LightingProofRebase",nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error)),"proof coordinate shader compile");
    Check(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)),"proof coordinate shader creation");
    // Enough records to need more than one thread group, so the bounds check and
    // the dispatch arithmetic are both exercised rather than assumed.
    const UINT count=73;
    LightingProof p;p.position={-1701,-554,-21};p.lumens=300;const auto source=ProofOmniRecord(p);
    std::vector<std::array<float,120>> records(count,source);
    const UINT directional=count-1;
    records[directional][32]=.3f;records[directional][33]=.4f;records[directional][34]=.5f;records[directional][35]=0;
    std::array<float,144> view{};view[140]=-1792;view[141]=-512;view[142]=-128;
    const UINT recordBytes=count*480;
    auto buffer=[&](UINT bytes,UINT bind,const void* data,UINT stride=0){
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=bind;d.StructureByteStride=stride;d.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
        D3D11_SUBRESOURCE_DATA initial{data,0,0};ComPtr<ID3D11Buffer> b;Check(SUCCEEDED(device->CreateBuffer(&d,data?&initial:nullptr,&b)),"proof buffer");return b;
    };
    auto authored=buffer(recordBytes,D3D11_BIND_SHADER_RESOURCE,records.data(),480);
    ComPtr<ID3D11ShaderResourceView> authoredSrv;
    Check(SUCCEEDED(device->CreateShaderResourceView(authored.Get(),nullptr,&authoredSrv)),"proof authored view");
    auto native=buffer(576,D3D11_BIND_CONSTANT_BUFFER,view.data());
    const UINT params[4]{count,0,0,0};
    auto rebase=buffer(16,D3D11_BIND_CONSTANT_BUFFER,params);
    auto result=buffer(recordBytes,D3D11_BIND_UNORDERED_ACCESS,nullptr,480);ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(result.Get(),nullptr,&uav)),"proof output");
    auto r=rebase.Get();auto n=native.Get();auto u=uav.Get();auto a=authoredSrv.Get();
    ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,&r);ctx->CSSetConstantBuffers(4,1,&n);
    ctx->CSSetShaderResources(0,1,&a);ctx->CSSetUnorderedAccessViews(0,1,&u,nullptr);
    ctx->Dispatch((count+63)/64,1,1);ctx->ClearState();
    D3D11_BUFFER_DESC d{};d.ByteWidth=recordBytes;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> staging;
    Check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&staging)),"proof staging");ctx->CopyResource(staging.Get(),result.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"proof readback");
    std::vector<std::array<float,120>> outputs(count);memcpy(outputs.data(),mapped.pData,recordBytes);ctx->Unmap(staging.Get(),0);
    const auto& actual=outputs[0];
    Check(outputs[directional][32]==.3f&&outputs[directional][33]==.4f&&outputs[directional][34]==.5f&&outputs[directional][35]==0,"directional sun does not translate with camera");
    // The last record lives in the second thread group, so a light past the first
    // 64 must be rebased too rather than left at its authored world position.
    Check(outputs[64]==actual,"a light beyond the first thread group is rebased");
    Check(outputs[count-2]==actual,"every authored light in the set is rebased");
    Check(actual[32]==91&&actual[33]==-42&&actual[34]==107,"authored lamp becomes camera relative on GPU");
    Check(std::abs(actual[0]*91+actual[3])<1e-6f&&std::abs(actual[5]*-42+actual[7])<1e-6f&&std::abs(actual[10]*107+actual[11]-.5f)<1e-6f,"rebased native frustum retains lamp center");
    Check(actual[40]==source[40]&&actual[37]==source[37],"rebasing preserves radiance and attenuation");
    Matrix matrices[4];const auto outerProjection=SunShadowMatrix({-4000,-3000,-100},{4000,3000,1500},{.3f,.4f,.8f});
    for(unsigned i=0;i<4;++i)matrices[i]=i==3?outerProjection:NearSunShadowMatrix(outerProjection,{-1792,-512,-128},float(64u<<(i*2)));
    auto sunParams=buffer(sizeof(matrices),D3D11_BIND_CONSTANT_BUFFER,matrices);
    auto locals=buffer(recordBytes,D3D11_BIND_UNORDERED_ACCESS,nullptr,480);
    ComPtr<ID3D11UnorderedAccessView> localUav;Check(SUCCEEDED(device->CreateUnorderedAccessView(locals.Get(),nullptr,&localUav)),"local-only light records");
    D3D11_BUFFER_DESC sd{};sd.ByteWidth=sizeof(matrices);sd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    sd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;ComPtr<ID3D11Buffer> sunColumns;
    Check(SUCCEEDED(device->CreateBuffer(&sd,nullptr,&sunColumns)),"GPU sun columns");
    D3D11_UNORDERED_ACCESS_VIEW_DESC su{};su.Format=DXGI_FORMAT_R32_TYPELESS;su.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    su.Buffer.NumElements=sd.ByteWidth/4;su.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;ComPtr<ID3D11UnorderedAccessView> sunUav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(sunColumns.Get(),&su,&sunUav)),"GPU sun column UAV");
    UINT ownParams[]{count,1,0,0};ctx->UpdateSubresource(rebase.Get(),0,nullptr,ownParams,0,0);
    auto sb=sunParams.Get();ID3D11UnorderedAccessView* outputsUav[]{uav.Get(),sunUav.Get(),localUav.Get()};
    ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,&r);ctx->CSSetConstantBuffers(2,1,&sb);
    ctx->CSSetConstantBuffers(4,1,&n);ctx->CSSetShaderResources(0,1,&a);ctx->CSSetUnorderedAccessViews(0,3,outputsUav,nullptr);
    ctx->Dispatch((count+63)/64,1,1);ctx->ClearState();
    ctx->CopyResource(staging.Get(),locals.Get());Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"local record read");
    memcpy(outputs.data(),mapped.pData,recordBytes);ctx->Unmap(staging.Get(),0);
    Check(outputs[0]==actual,"Owned CSM retains local light radiance and shadow projection");
    Check(outputs[directional][40]==0&&outputs[directional][41]==0&&outputs[directional][42]==0,"Native sun and clustered fallback cannot illuminate twice");
    sd.BindFlags=sd.MiscFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> sunRead;Check(SUCCEEDED(device->CreateBuffer(&sd,nullptr,&sunRead)),"Sun coordinate read");
    ctx->CopyResource(sunRead.Get(),sunColumns.Get());Check(SUCCEEDED(ctx->Map(sunRead.Get(),0,D3D11_MAP_READ,0,&mapped)),"Sun columns read");
    std::array<float,64> gpuColumns;memcpy(gpuColumns.data(),mapped.pData,sizeof(gpuColumns));ctx->Unmap(sunRead.Get(),0);
    const Vec3 offset{view[140],view[141],view[142]},receiver{-1780,-500,42};const auto relative=receiver-offset;
    for(unsigned i=0;i<4;++i){
        const auto expected=NativeSunColumns(matrices[i],offset);
        for(unsigned word=0;word<16;++word)Check(std::abs(gpuColumns[i*16+word]-expected[word])<.00001f,"Each cascade uses the current native view offset");
        const auto worldClip=Transform(matrices[i],receiver);const auto* columns=gpuColumns.data()+i*16;
        for(unsigned axis=0;axis<3;++axis){const float relativeClip=columns[axis]*relative.x+columns[4+axis]*relative.y+columns[8+axis]*relative.z+columns[12+axis];
            Check(std::abs(relativeClip-(&worldClip.x)[axis])<.00001f,"Camera-relative CSM projects the same receiver as world geometry");}
    }
    // View-led crops: the camera sits inside every crop before its blend band, and a receiver
    // down a long street stays in a fine crop instead of dropping to the whole-map tile.
    const auto crops=NativeSunCropExtents(3000,5560);
    Check(crops.close==128&&crops.middle==1500&&crops.detail>crops.close&&crops.detail<crops.middle,"sun crops span first person to shadowRange");
    Check(NativeSunCropExtents(100000,2000).middle==2000&&NativeSunCropExtents(1,5560).middle==512,"largest crop clamps to map and to the finest ladder");
    const Vec3 eye{881,2012,281};
    for(Vec3 angles:{Vec3{0,0,0},Vec3{2.7f,85.9f,0},Vec3{-40,200,0},Vec3{89,0,0},Vec3{-89,45,0}}){
        const auto forward=ViewForward(angles);
        Check(std::abs(Dot(forward,forward)-1)<1e-4f,"view forward is unit length");
        for(float half:{crops.close,crops.detail,crops.middle}){
            const auto crop=NativeSunCropMatrix(outerProjection,eye,forward,half,2048);
            const auto at=Transform(crop,eye);
            Check(std::abs(at.x)<.75f&&std::abs(at.y)<.75f,"camera stays inside its sun crop, before the blend band");
            for(int row=0;row<2;++row){const float texels=crop[row*4+3]*1024;
                Check(std::abs(texels-std::round(texels))<.01f,"view-led crops still move in whole texels");}
            for(int i=8;i<16;++i)Check(crop[i]==outerProjection[i],"view-led crops keep full-map caster depth");
        }
    }
    {
        const auto forward=ViewForward({2.7f,85.9f,0});const auto street=eye+forward*1800;
        const auto crop=NativeSunCropMatrix(outerProjection,eye,forward,crops.middle,2048);
        const auto at=Transform(crop,street);
        Check(std::abs(at.x)<.985f&&std::abs(at.y)<.985f,"receiver 1800 units down the view stays in the middle crop");
        const auto centred=NearSunShadowMatrix(outerProjection,eye,524,2048);const auto old=Transform(centred,street);
        Check(std::abs(old.x)>=.985f||std::abs(old.y)>=.985f,"the former camera-centred ladder dropped that receiver to the whole map");
    }
    {
        // Tight depth: receivers in the crop (map heights -200..700) span 0..1 of a far shorter
        // interval, a caster between them and the sun maps below 0 (pancaked, still occluding),
        // and the unit bias therefore covers far fewer world units than the whole-map interval.
        const auto forward=ViewForward({10,40,0});
        const auto wide=NativeSunCropMatrix(outerProjection,eye,forward,crops.close,2048);
        const auto tight=NativeSunCropMatrix(outerProjection,eye,forward,crops.close,2048,-200,700);
        for(int i=0;i<8;++i)Check(tight[i]==wide[i],"tight depth keeps the crop's lateral projection");
        const Vec3 w=Normalize(Vec3{tight[8],tight[9],tight[10]});
        const float wideUnits=1/std::sqrt(wide[8]*wide[8]+wide[9]*wide[9]+wide[10]*wide[10]);
        const float tightUnits=1/std::sqrt(tight[8]*tight[8]+tight[9]*tight[9]+tight[10]*tight[10]);
        Check(tightUnits*4<wideUnits,"the crop depth interval is a fraction of the whole map's");
        for(float z:{-200.f,0.f,281.f,700.f}){const auto at=Transform(tight,{eye.x,eye.y,z});
            Check(at.z>=0&&at.z<=1,"every receiver height inside the crop falls inside its depth interval");}
        const auto upstream=Transform(tight,Vec3{eye.x,eye.y,281}-w*800);
        Check(upstream.z<0,"a caster far toward the sun lies before the interval and is pancaked, not lost");
        const auto whole=Transform(outerProjection,Vec3{eye.x,eye.y,281}-w*800);
        Check(whole.z>=0&&whole.z<=1,"the same caster lies inside the whole-map interval (fixture sanity)");
        ShadowFrustum frustum(tight);
        Check(frustum.Intersects(Vec3{eye.x,eye.y,281}-w*800-Vec3{1,1,1},Vec3{eye.x,eye.y,281}-w*800+Vec3{1,1,1}),
              "sun crop culling keeps casters nearer the sun than the depth interval");
    }
    {
        Check(NativeSkyboxSunRect[1]+NativeSkyboxSunRect[3]*1==0&&
              NativeSkyboxSunRect[1]+NativeSkyboxSunRect[3]*-1==1,
              "skybox clip top maps to texture top rather than mirrored terrain");
        const auto wholeSky=SunShadowMatrix({-11000,-11000,-4000},{24000,24000,2000},{1,0,1});
        const Vec3 mainCamera{500,1000,4064},origin{80,-40,12};
        const auto sky=NativeSkyboxSunMatrix(wholeSky,mainCamera,origin,16,-500,1200,2048);
        const Vec3 tree=origin+Vec3{32,64,20};
        const auto a=Transform(sky,tree),b=Transform(sky,tree+Vec3{0,4,0});
        Check(std::abs(a.x)<.985f&&std::abs(a.y)<.985f&&a.z>=0&&a.z<=1,
              "skybox receiver uses authored origin and miniature scale");
        Check(std::abs(a.x-b.x)*1024>=7.9f,"nearby skybox tree spans pixels despite distant mountain billboards");
        Check(NativeSkyboxSunMatrix(wholeSky,mainCamera,origin,0,-500,1200,2048)==wholeSky,
              "legacy skybox without authored camera retains its full projection");
    }

}
// The thirteen programs whose bindings were read out of the shipped DXBC by hand.
// Deriving them from the bytecode has to reproduce every one exactly, or the
// derivation is not a safe replacement for the table it retires.
void CheckDerivedAbiAgainstShippedShaders(){
    // ctest runs from the build directory, so find the repository by walking up.
    std::error_code located;
    std::filesystem::path directory,search=std::filesystem::current_path(located);
    for(unsigned up=0;up<8&&!search.empty();++up,search=search.parent_path()){
        auto candidate=search/"Automation"/"output"/"lighting-replacement"/"fixture-shaders";
        if(std::filesystem::is_directory(candidate,located)){directory=candidate;break;}
        if(search.parent_path()==search)break;
    }
    struct Known {const char* hash;uint32_t slot,sampler,lightmapMask;};
    static const Known known[]={
        {"947cc1e2af98cf4b",18,6,7},{"cd2f50b4703ab117",20,6,0},{"11b1eacf862919ce",24,6,0},
        {"5eef8b3b25bf9f0d",14,6,7},{"942119fdd8d14bf9",14,6,7},{"a9e29effb0c2a1ea",14,6,7},
        {"3be7673b879d8c5c",14,7,7},{"7d946d9f3d3aac11",14,7,7},{"a6f8e07e40c2f552",14,7,7},
        {"e11f2e014ea4d4b8",14,7,7},{"a8163867e71fb140",17,7,0},{"ce791603a66e078c",16,7,0},
        {"fb9f2812009bb034",16,7,0}};
    std::error_code error;
    if(directory.empty()){
        std::cout<<"derived ABI: skipped, extracted shader evidence is not present in this checkout\n";return;}
    unsigned checked=0,derived=0,scanned=0;
    for(const auto& entry:std::filesystem::directory_iterator(directory,error)){
        if(entry.path().extension()!=".dxbc")continue;
        std::ifstream file(entry.path(),std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        NativeSunAbiInfo info{};HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&info);
        ++scanned;if(info.lightSlot)++derived;
        const auto name=entry.path().stem().string();
        for(const auto& expect:known)if(name==expect.hash){
            ++checked;
            const bool probes=expect.lightmapMask==0;
            Check(info.probeSlot==(probes?expect.slot-2:0)&&info.probeOcclusionSlot==(probes?expect.slot-1:0),
                  ("derived probe pair disagrees with audited bytecode for "+name).c_str());
            Check(info.lightSlot==expect.slot,("derived light slot disagrees with the audited table for "+name).c_str());
            Check(info.comparisonSampler==expect.sampler,("derived comparison sampler disagrees with the audited table for "+name).c_str());
            Check(info.lightmapArrayMask==expect.lightmapMask,("derived lightmap arrays disagree with the audited table for "+name).c_str());
            // Every audited program carries one projected-cookie volume beside its probe pair;
            // a8163867 samples it as t4 inside the clustered light loop.
            Check(info.hasCookie&&info.cookieSlot!=info.probeSlot&&info.cookieSlot!=info.probeOcclusionSlot,
                  ("derived cookie volume missing for "+name).c_str());
            if(name=="a8163867e71fb140")Check(info.cookieSlot==4,"a8163867 samples its cookie volume at t4");
            std::vector<uint8_t> soft;bool cascades=false;
            Check(NativeAtmosphereProgram(bytes.data(),bytes.size(),soft,true,&cascades),
                  ("audited lamp shader can own soft shadows: "+name).c_str());
            Check(soft.size()>bytes.size(),("dense lamp kernel replaces nine taps: "+name).c_str());
            ComPtr<ID3D11Device> device;ComPtr<ID3D11PixelShader> shader;
            Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)),"soft lamp shader validation device");
            Check(SUCCEEDED(device->CreatePixelShader(soft.data(),soft.size(),nullptr,&shader)),
                  ("soft lamp shader validates on D3D11: "+name).c_str());
        }
    }
    Check(checked==std::size(known),"every audited shader was present and checked");
    {
        // First-person weapon/glove layout: one probe volume at lightSlot-1 and no occlusion volume.
        std::ifstream file(directory/"d47bf4fa23a33fe2.dxbc",std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        NativeSunAbiInfo info{};HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&info);
        Check(!bytes.empty()&&info.lightSlot==15&&info.probeSlot==14&&info.probeOcclusionSlot==0&&(info.flags&64),
              "viewmodel programs reach the probe volume through their single volume slot");
        Check(info.hasCookie&&info.cookieSlot==4,"viewmodel cookie volume stays distinct from its probe volume");
    }
    // Register-allocation variants that once fell back to the coarse whole-map sun tile: an ICB
    // with rows past the four selectors, two shading blocks with duplicate guards, a .yzw
    // direction operand, and a .xzw radiance destination. Each must own all four cascades.
    for(const char* variant:{"e758081e0aaec204","42962ccbd186878e","8df08537abc0a71f","05582fb7d3c0c26e"}){
        std::ifstream file(directory/(std::string(variant)+".dxbc"),std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{}),patched;
        Check(!bytes.empty(),(std::string("variant shader evidence present: ")+variant).c_str());
        NativeSunAbiInfo info{};bool cascades=false;
        Check(HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&info),(std::string("variant carries the sun ABI: ")+variant).c_str());
        Check(NativeAtmosphereProgram(bytes.data(),bytes.size(),patched,true,&cascades)&&cascades,
              (std::string("variant owns the native sun cascades: ")+variant).c_str());
    }
    std::cout<<"derived ABI: "<<derived<<'/'<<scanned<<" extracted programs bind a light record buffer, "
             <<checked<<" audited hashes reproduced exactly\n";
}
int main(int argc,char** argv){
    CheckResourceBindings();
    Check(NativeLightingViewport(1600,1200,0,.95f),"main world receives dynamic lighting");
    Check(NativeLightingViewport(1600,1200,0,.1f),"viewmodel receives dynamic lighting");
    Check(NativeLightingViewport(1600,1200,.95f,1),"3D skybox receives dynamic lighting");
    Check(!NativeLightingViewport(1600,1200,1,1),"sky backdrop is not a lit surface");
    Check(!NativeLightingViewport(4096,4096,0,1),"shadow target is not a lighting receiver");
    Check(!NativeLightingViewport(128,128,0,.95f),"small effect target stays outside material lighting");
    Check(!NativeLightingViewport(NAN,1200,0,.95f),"invalid viewport stays outside material lighting");
    CheckAtmosphere();
    CheckLampShadowOcclusion();
    CheckProofRebase();
    CheckDerivedAbiAgainstShippedShaders();
    LightingProof proof;proof.position={-1701,-554,-21};proof.lumens=300;proof.range=900;proof.color={1,.5f,0};
    const auto lamp=ProofOmniRecord(proof);
    Check(std::abs(lamp[40]-3000)<.001f&&lamp[41]>640&&lamp[41]<643&&lamp[42]==0,"omni lumen conversion and sRGB decoding");
    const auto clip=[&](Vec3 p){return Vec3{lamp[0]*p.x+lamp[3],lamp[5]*p.y+lamp[7],lamp[10]*p.z+lamp[11]};};
    Check(std::abs(clip(proof.position).z-.5f)<1e-6f&&std::abs(clip(proof.position).x)<1e-6f,"native frustum centers authored fixture");
    Check(clip(proof.position+Vec3{901,0,0}).x>1,"fixture range bounds native evaluation");
    proof.fixture=false;const auto disabled=ProofOmniRecord(proof);Check(disabled[40]==0&&disabled[41]==0&&disabled[42]==0,"disabled fixture has no radiance");
    proof.fixture=true;proof.intensity=2;Check(ProofOmniRecord(proof)[40]==lamp[40]*2,"fixture radiance scales independently");

    for(int i=1;i<argc;++i){std::ifstream file(argv[i],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{}),coverage;NativeSunAbiInfo info{};
        const bool abi=HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&info);
        // Coverage instrumentation is optional per program; an unsupported layout must not stop the
        // cascade audit of the remaining arguments. Whatever IS produced must still validate.
        const bool covered=NativeCoverageProgram(bytes.data(),bytes.size(),coverage);
        ComPtr<ID3D11Device> device;Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)),"shader verification device");
        ComPtr<ID3D11PixelShader> shader;
        if(covered)Check(SUCCEEDED(device->CreatePixelShader(coverage.data(),coverage.size(),nullptr,&shader)),"shipped instrumented shader validates");
        bool cascades=false;std::vector<uint8_t> atmosphere;const bool fog=NativeAtmosphereProgram(bytes.data(),bytes.size(),atmosphere,true,abi?&cascades:nullptr);
        if(fog){shader.Reset();Check(SUCCEEDED(device->CreatePixelShader(atmosphere.data(),atmosphere.size(),nullptr,&shader)),"shipped atmosphere shader validates");}
        std::cout<<argv[i]<<" abi="<<abi<<" flags="<<info.flags<<" coverage="<<(covered?"validated":"unsupported")<<" atmosphere="<<fog<<" cascades="<<cascades<<"\n";}
    Check(!Filmmaker::CreatingPrivateRenderTexture(),"engine allocations initially visible");
    {
        Filmmaker::RenderPrivateTextureScope outer;
        {Filmmaker::RenderPrivateTextureScope inner;Check(Filmmaker::CreatingPrivateRenderTexture(),"nested private allocation excluded");}
        Check(Filmmaker::CreatingPrivateRenderTexture(),"outer private allocation remains excluded");
        std::thread engineThread([]{Check(!Filmmaker::CreatingPrivateRenderTexture(),"other engine thread remains visible");});engineThread.join();
    }
    Check(!Filmmaker::CreatingPrivateRenderTexture(),"engine allocation tracking restored");
    for(unsigned samples:{1u,4u})for(bool world:{false,true})Run(samples,world);
    Run(4,true,true);
}
