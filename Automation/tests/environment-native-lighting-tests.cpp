#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeLightingMath.h"
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
    // Reproduce the production shared upload: CPU reflection/probe list rows
    // are filled first; the GPU's five-row cluster copy contains zeros there.
    // Copy only the unwritten spans, preserving later CPU data and source offsets.
    {
        std::array<unsigned,20> gpu{},cpu{};
        for(unsigned i=0;i<20;++i)gpu[i]=100+i;
        std::fill(gpu.begin()+4,gpu.begin()+12,0);
        const unsigned reflections[4]{7,8,1,0},probes[4]{9,10,1,0};
        ConstantPatchBatch patches;patches.Write(16,16,reflections);patches.Write(32,16,probes);
        patches.Flush([&](unsigned offset,unsigned size,const void* data){std::memcpy(reinterpret_cast<char*>(cpu.data())+offset,data,size);});
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=80;
        D3D11_SUBRESOURCE_DATA data{gpu.data(),0,0};ComPtr<ID3D11Buffer> source,destination,readback;
        Check(SUCCEEDED(device->CreateBuffer(&desc,&data,&source)),"GPU cluster-row fixture");
        data.pSysMem=cpu.data();Check(SUCCEEDED(device->CreateBuffer(&desc,&data,&destination)),"CPU reflection/probe fixture");
        patches.ForEachUnwritten(0,80,[&](unsigned offset,unsigned size){
            const D3D11_BOX box{offset,0,0,offset+size,1,1};
            ctx->CopySubresourceRegion(destination.Get(),0,offset,0,0,source.Get(),0,&box);
        });
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&readback)),"Lighting precedence readback");
        ctx->CopyResource(readback.Get(),destination.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"Map lighting precedence");
        const auto* actual=static_cast<const unsigned*>(mapped.pData);
        for(unsigned i=0;i<20;++i)Check(actual[i]==(i>=4&&i<12?cpu[i]:gpu[i]),"GPU copy preserves later reflection/probe rows and GPU grid rows");
        ctx->Unmap(readback.Get(),0);
    }
    auto compile=[&](const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;
        auto hr=D3DCompile(text.data(),text.size(),"LightingTest",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"Compile fixture");return code;};
    const std::string declaration="cbuffer Lighting:register(b3){float4 l[25];float4 reflections[9];float4 padding[1143];float4 probes[6];float4 middle[762];float4 sunDirection;float4 radiance;float4 contact;float4 misc[5];float4 flags;float4 thresholds[2];float4 cascades[16];float4 gap[16];float4 rects[4];};cbuffer View:register(b4){float4 v[40];};";
    const auto code=compile(declaration+R"(
float4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{
    if(((uint)p.x+s)%5==0)discard;
    uint cascade=((uint)p.x%4)*4;
    uint reflection=(uint)v[0].x,probe=(uint)v[0].y;
    float3 owned=l[0].xyz-abs(l[6].xyx)+l[24].yyy+radiance.xyz;
    owned+=cascades[cascade].xyz+cascades[cascade+1].xyz;
    owned+=reflections[reflection].xyz+reflections[reflection+8].xyz;
    owned+=probes[probe].xyz+probes[probe+5].xyz;
    owned+=rects[(uint)p.y%4].xyz+flags.xyz;
    float3 native=l[4].xyz+l[11].xyz+l[6].zwz+l[24].xzw;
    return float4(owned+native+v[2].xyz,radiance.w+flags.w+s*.02);
})","ps_5_0");
    std::vector<uint8_t> patched;NativeSharedLightingInfo layout;
    if(!NativeSharedLightingProgram(code->GetBufferPointer(),code->GetBufferSize(),patched,layout)){
        ComPtr<ID3DBlob> assembly;D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&assembly);
        if(assembly)std::cerr<<static_cast<const char*>(assembly->GetBufferPointer());Check(false,"Split owned lighting");
    }
    ComPtr<ID3D11PixelShader> original,shared;
    Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&original)),"Original PS");
    Check(SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&shared)),"Shared PS");
    std::vector<ConstantPatchBatch::Range> covered;
    for(unsigned row=0;row<NativeLightingRows;++row)for(unsigned c=0;c<4;++c)if(NativeOwnedLightingMask(row)&(1u<<c))covered.push_back({row*16+c*4,row*16+c*4+4});
    Check(NativeSharedLightingCovered(layout,covered),"Owned-byte coverage accepted");
    auto missing=covered;missing.erase(std::remove_if(missing.begin(),missing.end(),[](auto r){return r.begin==1957*16;}),missing.end());
    Check(!NativeSharedLightingCovered(layout,missing),"Incomplete sun matrix coverage falls back");
    Check(!NativeSharedLightingCovered(layout,{}),"Missing data falls back");
    for(const auto& body:{std::string("return l[6];"),std::string("return l[(uint)p.x];"),std::string("return l[11+(uint)p.x];"),std::string("return l[0]+reserved;")}){
        const auto c=compile(declaration+(body.find("reserved")!=std::string::npos?"cbuffer Occupied:register(b11){float4 reserved;};":"")+
            "float4 main(float4 p:SV_Position):SV_Target{"+body+"}","ps_5_0");
        Check(!NativeSharedLightingProgram(c->GetBufferPointer(),c->GetBufferSize(),patched,layout),"Mixed/native-relative/occupied slot falls back");
    }
    Check(!NativeSharedLightingProgram(code->GetBufferPointer(),31,patched,layout),"Truncated container rejected");
    auto vsCode=compile("float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"Vertex shader");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=NativeLightingRows*16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> lighting,owned,view;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&lighting))&&SUCCEEDED(device->CreateBuffer(&bd,nullptr,&owned)),"Lighting buffers");
    bd.ByteWidth=40*16;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&view)),"View buffer");
    for(unsigned samples:{1u,4u,8u}){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> image,resolved,staging;ComPtr<ID3D11RenderTargetView> rtv;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&image))&&SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&rtv)),"MSAA target");
        d.SampleDesc.Count=1;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&resolved)),"Resolve target");
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Readback target");
        auto draw=[&](ID3D11PixelShader* ps,const std::array<float,NativeLightingRows*4>& values){
            ctx->UpdateSubresource(lighting.Get(),0,nullptr,values.data(),0,0);auto b=lighting.Get();ctx->PSSetConstantBuffers(3,1,&b);b=owned.Get();ctx->PSSetConstantBuffers(11,1,&b);b=view.Get();ctx->PSSetConstantBuffers(4,1,&b);
            const float clear[]{-.5f,-.25f,-.125f,-1};ctx->ClearRenderTargetView(rtv.Get(),clear);auto target=rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
            D3D11_VIEWPORT vp{0,0,8,8,0,1};ctx->RSSetViewports(1,&vp);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
            if(samples>1)ctx->ResolveSubresource(resolved.Get(),0,image.Get(),0,d.Format);else ctx->CopyResource(resolved.Get(),image.Get());
            ctx->CopyResource(staging.Get(),resolved.Get());D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Map readback");
            std::vector<uint16_t> pixels(8*8*4);for(unsigned y=0;y<8;++y)std::memcpy(pixels.data()+y*32,static_cast<const char*>(m.pData)+y*m.RowPitch,64);ctx->Unmap(staging.Get(),0);return pixels;};
        for(unsigned frame=0;frame<3;++frame){
            std::array<float,NativeLightingRows*4> sharedValues{};
            for(unsigned i=0;i<sharedValues.size();++i)sharedValues[i]=float((i*13+frame*7)%31)*.002f;
            ctx->UpdateSubresource(owned.Get(),0,nullptr,sharedValues.data(),0,0);
            std::vector<uint16_t> prior;
            for(unsigned material=0;material<4;++material){
                std::array<float,NativeLightingRows*4> native{};
                for(unsigned i=0;i<native.size();++i)native[i]=float((i*7+material*19)%43)*.003f;
                std::array<float,160> viewRows{};viewRows[8]=material*.01f;ctx->UpdateSubresource(view.Get(),0,nullptr,viewRows.data(),0,0);
                auto reference=native;
                for(unsigned row=0;row<NativeLightingRows;++row)for(unsigned c=0;c<4;++c)if(NativeOwnedLightingMask(row)&(1u<<c))reference[row*4+c]=sharedValues[row*4+c];
                auto expected=draw(original.Get(),reference),actual=draw(shared.Get(),native);
                Check(expected==actual,"Shared lighting matches clone with native changes, partial rows, dynamic records and discard");
                if(material)Check(actual!=prior,"Native material changes remain visible");prior=actual;
            }
        }
        std::cout<<"Shared lighting matches reference at "<<samples<<" samples across frames/materials\n";
    }
    if(argc>2){
        std::ifstream file(argv[2],std::ios::binary);std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(file)),{});Check(!fixture.empty(),"Read native fixture");
        std::vector<uint8_t> atmosphere,dynamic,ao;bool cascades=false;unsigned screen=0;NativeSunAbiInfo abi{};HasNativeSunMaterialAbi(fixture.data(),fixture.size(),&abi);
        Check(NativeAtmosphereProgram(fixture.data(),fixture.size(),atmosphere,true,&cascades,&screen),"Native atmosphere fixture");
        Check(NativeSharedAoProgram(atmosphere.data(),atmosphere.size(),ao),"Native atmosphere AO split");
        Check(NativeSharedLightingProgram(ao.data(),ao.size(),patched,layout),"Native atmosphere lighting split");
        ComPtr<ID3D11PixelShader> ps;Check(SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps)),"Native shared atmosphere PS");
        Filmmaker::Relight::IrradianceProgramInfo info;
        Check(Filmmaker::Relight::DynamicIrradianceProgram(atmosphere.data(),atmosphere.size(),dynamic,&info,abi.probeSlot),"Native GI fixture");
        Check(NativeSharedAoProgram(dynamic.data(),dynamic.size(),ao),"Native GI AO split");
        Check(NativeSharedLightingProgram(ao.data(),ao.size(),patched,layout),"Native GI lighting split");ps.Reset();Check(SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps)),"Native shared GI PS");
        std::cout<<"Captured native atmosphere/GI programs accept shared lighting\n";
    }
}
