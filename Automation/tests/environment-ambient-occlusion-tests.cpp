#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusion.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusionMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusionShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSkyVisibilityMath.h"
#include <string>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <cmath>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker {
bool MvmDebugLog_Active(){return false;}
void MvmDebugLog_LinefAlways(const char*,const char*,...){ }
}
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
float Half(uint16_t value){
    const int exponent=(value>>10)&31;const float fraction=float(value&1023)/1024;
    return (value&32768?-1.f:1.f)*std::ldexp(exponent?1+fraction:fraction,exponent?exponent-15:-14);
}
ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* target){
    ComPtr<ID3DBlob> code,error;
    const auto hr=CompileEnvironmentShader(source,std::strlen(source),"AOTest",nullptr,entry,target,
        D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
    if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
    Check(SUCCEEDED(hr),"Test shader compiles");return code;
}
int main(){
    Check(AmbientHorizon({0,0,1},{3,2,0},32)==0,"A tangent plane has no artificial ambient occlusion");
    Check(AmbientHorizon({0,0,1},{0,0,-3},32)==0,"Geometry behind the normal cannot occlude its hemisphere");
    Check(AmbientHorizon({0,0,1},{0,0,40},32)==0,"The world-space radius excludes distant geometry");
    Check(AmbientHorizon({0,0,1},{0,0,3},32)>.9,"A nearby blocker closes the hemisphere horizon");
    Check(!AmbientViewport(128,128,.95f,1),"Miniature 3D skybox depth cannot become main-world AO");
    Check(!AmbientViewport(128,128,0,.3f),"The viewmodel depth range cannot become main-world AO");
    Check(AmbientViewport(128,128,0,.95f),"The main-world compressed depth range supports AO");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,8,nullptr,0,D3D11_SDK_VERSION,
        &device,&level,&ctx)),"WARP device");
    Check(std::string(AmbientOcclusionShader).find("#define SKY_DIRECTIONS "+std::to_string(SkyVisibilityDirections)+"\n")!=std::string::npos,
          "shader sky direction count matches SkyVisibilityDirections");
    BeginAmbientOcclusion(device.Get());
    // Sky-visibility maps: every slice at depth 0 occludes every receiver, depth 1 exposes it.
    auto skyMaps=[&](float depth){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=1;d.ArraySize=SkyVisibilityDirections;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<float> texels(16,depth);std::vector<D3D11_SUBRESOURCE_DATA> init(SkyVisibilityDirections,{texels.data(),16,0});
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;
        Check(SUCCEEDED(device->CreateTexture2D(&d,init.data(),&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv)),"sky visibility test maps");
        return srv;};
    const auto skyBlocked=skyMaps(0),skyOpen=skyMaps(1);
    Matrix skyIdentity[SkyVisibilityDirections]{};for(auto& m:skyIdentity)for(unsigned i=0;i<4;++i)m[i*5]=1;
    const auto vsCode=Compile(R"(
        float4 VS(uint id:SV_VertexID):SV_Position {
            float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
        })","VS","vs_5_0");
    const auto psCode=Compile(R"(
        cbuffer Surface:register(b0){float4 settings;}
        float PS(float4 pixel:SV_Position):SV_Depth {
            return settings.x>0&&pixel.x<64?settings.z:settings.y;
        })","PS","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"VS");
    Check(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)),"PS");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> surface,view;
    Check(SUCCEEDED(device->CreateBuffer(&cb,nullptr,&surface)),"Surface constants");
    cb.ByteWidth=128;Check(SUCCEEDED(device->CreateBuffer(&cb,nullptr,&view)),"Native inverse projection constants");
    for(unsigned samples:{1u,4u})for(bool reversed:{false,true}){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=128;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&depth)),"MSAA depth texture");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension=samples>1?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&dv,&dsv)),"MSAA depth view");
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc=reversed?D3D11_COMPARISON_GREATER_EQUAL:D3D11_COMPARISON_LESS_EQUAL;
        ComPtr<ID3D11DepthStencilState> write,read;
        Check(SUCCEEDED(device->CreateDepthStencilState(&dd,&write)),"Depth write state");
        dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        Check(SUCCEEDED(device->CreateDepthStencilState(&dd,&read)),"Read-only native forward pass");
        const D3D11_VIEWPORT vp{0,0,128,128,0,1};
        std::array<float,32> matrix{};for(unsigned i=0;i<4;++i)matrix[16+i*5]=1;
        if(reversed){matrix[26]=-1;matrix[30]=1;}
        Matrix clipToWorld{};for(unsigned i=0;i<4;++i)clipToWorld[i*5]=1;
        if(reversed){clipToWorld[10]=-1;clipToWorld[11]=1;}
        ctx->UpdateSubresource(view.Get(),0,nullptr,matrix.data(),0,0);
        auto evaluate=[&](bool obstacle,float strength,const SkyVisibilityInput* sky=nullptr){
            ctx->ClearState();ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(0,nullptr,dsv.Get());
            ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,reversed?0.f:1.f,0);
            ctx->OMSetDepthStencilState(write.Get(),0);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float values[]{obstacle?1.f:0.f,reversed?.2f:.8f,reversed?.6f:.4f,0};
            ctx->UpdateSubresource(surface.Get(),0,nullptr,values,0,0);auto buffer=surface.Get();ctx->PSSetConstantBuffers(0,1,&buffer);
            ctx->Draw(3,0);ctx->OMSetDepthStencilState(read.Get(),0);
            NextAmbientOcclusionFrame();auto visibility=AmbientOcclusion(ctx.Get(),{0,0,-2},vp,view.Get(),clipToWorld,strength,sky);
            Check(visibility!=nullptr,"Owned ambient visibility exists");
            ComPtr<ID3D11DepthStencilView> restored;ctx->OMGetRenderTargets(0,nullptr,&restored);
            Check(restored.Get()==dsv.Get(),"AO restores the native depth binding");
            ComPtr<ID3D11Resource> resource;visibility->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;resource.As(&texture);D3D11_TEXTURE2D_DESC out{};texture->GetDesc(&out);
            if(out.Width==1)return std::vector<float>{1};
            Check(out.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"Native directional AO receives four visibility channels");
            out.Usage=D3D11_USAGE_STAGING;out.BindFlags=0;out.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&out,nullptr,&staging)),"AO readback");
            ctx->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"AO result");
            std::vector<float> valuesOut;
            for(unsigned y=0;y<out.Height;++y){const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch);
                for(unsigned x=0;x<out.Width;++x){
                    Check(row[x*4]==row[x*4+1]&&row[x*4]==row[x*4+2]&&row[x*4]==row[x*4+3],"AO channel replication");
                    valuesOut.push_back(Half(row[x*4]));
                }
            }
            ctx->Unmap(staging.Get(),0);return valuesOut;
        };
        const auto plane=evaluate(false,1);Check(plane.size()>1,"AO computes instead of returning its neutral fallback");
        for(float value:plane)Check(value>.99f&&value<=1,"An open plane remains fully visible");
        const auto corner=evaluate(true,1);
        Check(*std::min_element(corner.begin(),corner.end())<.95f,"A depth step produces local occlusion");
        // A vertical edge has identical geometry in adjacent central rows. With
        // MSAA (no temporal accumulator), changing pixel index must not rotate
        // its horizon quadrature and change its brightness.
        const unsigned side=unsigned(std::sqrt(double(corner.size())));
        for(unsigned x=side/2+2;x<side/2+12;++x)
            Check(std::abs(corner[(side/2-1)*side+x]-corner[(side/2)*side+x])<.002f,
                  "Adjacent pixels on the same straight edge have stable AO quadrature");
        const auto stronger=evaluate(true,2);
        Check(stronger.size()==corner.size(),"Strength does not change AO dimensions");
        for(size_t i=0;i<corner.size();++i)
            Check(std::isfinite(stronger[i])&&stronger[i]>=0&&stronger[i]<=corner[i]+.001f,
                  "Increasing AO strength deepens occlusion without black invalid pixels or brightening");
        const auto removed=evaluate(false,1);
        for(float value:removed)Check(value>.99f,"Removing a blocker updates visibility on the next frame");
        Check(evaluate(true,0)[0]==1,"AO strength zero binds owned neutral visibility");
        const SkyVisibilityInput sealed{skyBlocked.Get(),skyIdentity,1},half{skyBlocked.Get(),skyIdentity,.5f},open{skyOpen.Get(),skyIdentity,1};
        for(float value:evaluate(false,1,&sealed))Check(value<.01f,"a receiver no sky direction reaches loses its sky ambient");
        for(float value:evaluate(false,1,&half))Check(std::abs(value-.5f)<.01f,"sky visibility strength scales the ambient");
        for(float value:evaluate(false,1,&open))Check(value>.99f,"a receiver open to every sky direction keeps full ambient");
        const auto skyOnly=evaluate(false,0,&sealed);
        Check(skyOnly.size()>1,"Sky visibility still computes when contact AO is disabled");
        for(float value:skyOnly)Check(value<.01f,"AO strength zero does not expose a sealed room to sky light");
        for(float value:evaluate(false,0,&half))Check(std::abs(value-.5f)<.01f,"Sky strength stays independent of contact AO strength");
        for(float value:evaluate(true,0,&open))Check(value>.99f,"Disabling contact AO removes its corner shading while preserving open sky");
        ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,reversed?0.f:1.f,0);
        NextAmbientOcclusionFrame();
        auto missing=AmbientOcclusion(ctx.Get(),{0,0,-2},vp,view.Get(),clipToWorld,2,&sealed);
        // No scene depth must stay neutral even with strong AO and a sealed-sky input.
        ComPtr<ID3D11Resource> missingResource;missing->GetResource(&missingResource);
        ComPtr<ID3D11Texture2D> missingTexture;missingResource.As(&missingTexture);
        D3D11_TEXTURE2D_DESC missingDesc{};missingTexture->GetDesc(&missingDesc);
        missingDesc.Usage=D3D11_USAGE_STAGING;missingDesc.BindFlags=0;missingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> missingRead;
        Check(SUCCEEDED(device->CreateTexture2D(&missingDesc,nullptr,&missingRead)),"Empty depth readback texture");
        ctx->CopyResource(missingRead.Get(),missingTexture.Get());D3D11_MAPPED_SUBRESOURCE empty{};
        Check(SUCCEEDED(ctx->Map(missingRead.Get(),0,D3D11_MAP_READ,0,&empty)),"Empty depth visibility readback");
        for(unsigned y=0;y<missingDesc.Height;++y){const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(empty.pData)+y*empty.RowPitch);
            for(unsigned x=0;x<missingDesc.Width;++x)Check(Half(row[x*4])>.99f,"Missing depth never creates black patches");}
        ctx->Unmap(missingRead.Get(),0);
        std::cout<<"samples="<<samples<<" reversed="<<reversed<<" planar=1 cornerMinimum="
            <<*std::min_element(corner.begin(),corner.end())<<" liveRemoval=pass\n";
    }
    EndAmbientOcclusion();std::cout<<"Owned ambient occlusion passed.\n";
}
