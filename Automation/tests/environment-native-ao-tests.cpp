#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAoMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAtmosphereMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialMath.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool value,const char* why){if(!value){std::cerr<<why<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    bool hardware=argc>1&&std::string(argv[1])=="--hardware";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"D3D11 device");
    auto compile=[&](const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;
        auto hr=D3DCompile(text.data(),text.size(),"AoTest",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"Compile fixture");return code;};
    const std::string declaration="cbuffer View:register(b4){float4 v[40];};";
    const auto code=compile(declaration+R"(
float4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{
    if(((uint)p.x+s)%5==0)discard;
    return float4(v[16].xy*p.xy,-abs(v[16].z)+v[2].x,v[16].w+v[17].w+s*.02);
})","ps_5_0");
    std::vector<uint8_t> patched;
    Check(NativeSharedAoProgram(code->GetBufferPointer(),code->GetBufferSize(),patched),"Split AO addressing");
    ComPtr<ID3D11PixelShader> original,shared;
    Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&original)),"Original PS");
    Check(SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&shared)),"Shared PS");
    std::vector<ComPtr<ID3D11PixelShader>> unchangedShaders;
    for(const auto& body:{std::string("return v[2]+v[17];"),std::string("return v[16].wwww+v[2];")}){
        const auto c=compile(declaration+"float4 main(float4 p:SV_Position):SV_Target{"+body+"}","ps_5_0");
        bool unchanged=false;
        Check(!NativeSharedAoProgram(c->GetBufferPointer(),c->GetBufferSize(),patched,&unchanged)&&unchanged&&patched.empty(),
            "Prove that absent xyz reads or w-only reads need no AO repair");
        ComPtr<ID3D11PixelShader> ps;
        Check(SUCCEEDED(device->CreatePixelShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&ps)),"Unchanged native PS");
        unchangedShaders.push_back(std::move(ps));
    }
    for(const auto& body:{std::string("return v[16];"),std::string("return v[(uint)p.x];"),std::string("return v[16].xyxy+reserved;")}){
        const auto c=compile(declaration+(body.find("reserved")!=std::string::npos?"cbuffer Occupied:register(b12){float4 reserved;};":"")+
            "float4 main(float4 p:SV_Position):SV_Target{"+body+"}","ps_5_0");
        bool unchanged=true;
        Check(!NativeSharedAoProgram(c->GetBufferPointer(),c->GetBufferSize(),patched,&unchanged)&&!unchanged,
            "Mixed xyz/w, relative rows and occupied slots must not be classified as unused");
    }
    bool unchanged=true;
    Check(!NativeSharedAoProgram(nullptr,0,patched,&unchanged)&&!unchanged,"Truncated container rejected, without reuse proof");
    auto vsCode=compile("float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"Vertex shader");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=40*16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> view,ao;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&view)),"View buffer");bd.ByteWidth=16;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&ao)),"AO buffer");
    for(unsigned samples:{1u,4u,8u}){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> image,resolved,staging;ComPtr<ID3D11RenderTargetView> rtv;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&image))&&SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&rtv)),"MSAA target");
        d.SampleDesc.Count=1;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&resolved)),"Resolve target");
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Readback target");
        auto draw=[&](ID3D11PixelShader* ps,const std::array<float,160>& values){
            ctx->UpdateSubresource(view.Get(),0,nullptr,values.data(),0,0);auto b=view.Get();ctx->PSSetConstantBuffers(4,1,&b);b=ao.Get();ctx->PSSetConstantBuffers(12,1,&b);
            const float clear[]{-.5f,-.25f,-.125f,-1};ctx->ClearRenderTargetView(rtv.Get(),clear);auto target=rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
            D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
            if(samples>1)ctx->ResolveSubresource(resolved.Get(),0,image.Get(),0,d.Format);else ctx->CopyResource(resolved.Get(),image.Get());
            ctx->CopyResource(staging.Get(),resolved.Get());D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Map readback");
            std::vector<uint16_t> pixels(8*8*4);for(unsigned y=0;y<8;++y)std::memcpy(pixels.data()+y*32,static_cast<const char*>(m.pData)+y*m.RowPitch,64);ctx->Unmap(staging.Get(),0);return pixels;};
        std::vector<uint16_t> prior;
        for(unsigned change=0;change<3;++change){
            std::array<float,160> native{};native[64]=9;native[65]=8;native[66]=7;native[67]=.6f+change*.03f;native[8]=.1f*change;native[71]=.3f;
            const std::array<float,4> row{1.f/(8+change*4),1.f/(8+change*8),.5f,0};ctx->UpdateSubresource(ao.Get(),0,nullptr,row.data(),0,0);
            auto reference=native;std::copy_n(row.begin(),3,reference.begin()+64);
            auto expected=draw(original.Get(),reference),actual=draw(shared.Get(),native);
            Check(expected==actual,"Shared AO matches cloned-view output with discard and changing native/view values");
            for(const auto& ps:unchangedShaders)
                Check(draw(ps.Get(),reference)==draw(ps.Get(),native),"Skipping unused AO repair is pixel-exact, retaining mutable native rows and w");
            if(change)Check(actual!=prior,"Scene/viewport changes affect output");prior=actual;
        }
        std::cout<<"AO shared view matches reference at "<<samples<<" samples\n";
    }
    if(argc>2){
        std::ifstream file(argv[2],std::ios::binary);std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(file)),{});Check(!fixture.empty(),"Read native fixture");
        std::vector<uint8_t> atmosphere,dynamic;bool cascades=false;unsigned screen=0;NativeSunAbiInfo abi{};HasNativeSunMaterialAbi(fixture.data(),fixture.size(),&abi);
        Check(NativeAtmosphereProgram(fixture.data(),fixture.size(),atmosphere,true,&cascades,&screen),"Native atmosphere fixture");
        const bool expectUnchanged=argc>3&&std::string(argv[3])=="--unused";
        bool nativeUnchanged=false;
        const bool split=NativeSharedAoProgram(atmosphere.data(),atmosphere.size(),patched,&nativeUnchanged);
        Check(expectUnchanged?(!split&&nativeUnchanged):split,"Native atmosphere AO split or proven unused");
        const auto& atmosphereResult=nativeUnchanged?atmosphere:patched;
        ComPtr<ID3D11PixelShader> ps;Check(SUCCEEDED(device->CreatePixelShader(atmosphereResult.data(),atmosphereResult.size(),nullptr,&ps)),"Native atmosphere PS");
        Filmmaker::Relight::IrradianceProgramInfo info;
        Check(Filmmaker::Relight::DynamicIrradianceProgram(atmosphere.data(),atmosphere.size(),dynamic,&info,abi.probeSlot),"Native GI fixture");
        const bool giSplit=NativeSharedAoProgram(dynamic.data(),dynamic.size(),patched,&nativeUnchanged);
        Check(expectUnchanged?(!giSplit&&nativeUnchanged):giSplit,"Native GI AO split or proven unused");
        const auto& dynamicResult=nativeUnchanged?dynamic:patched;
        ps.Reset();Check(SUCCEEDED(device->CreatePixelShader(dynamicResult.data(),dynamicResult.size(),nullptr,&ps)),"Native GI PS");
        std::cout<<"Captured native atmosphere/GI programs accept "<<(expectUnchanged?"unchanged native view":"shared AO")<<"\n";
    }
}
