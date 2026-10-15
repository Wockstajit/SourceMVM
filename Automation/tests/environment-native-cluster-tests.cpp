#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h"
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    bool hardware=argc>1&&std::string(argv[1])=="--hardware";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    auto hr=D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    Check(SUCCEEDED(hr),"D3D11 device");ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    auto compile=[&](const std::string& source,const char* profile){ComPtr<ID3DBlob> code,error;
        auto result=D3DCompile(source.data(),source.size(),"SparseClusterTest",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(result)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(result),"Compile fixture");return code;};
    const std::string source=R"(
cbuffer Lighting:register(b3){uint4 l[24];}
cbuffer View:register(b4){float4 v[36];}
cbuffer Viewport:register(b1){float4 viewport[21];}
StructuredBuffer<uint> masks:register(t13);
StructuredBuffer<float4> lights:register(t19);
Texture2D<float4> surface:register(t0);SamplerState materialSampler:register(s0);
float4 main(float4 p:SV_Position,uint sample:SV_SampleIndex):SV_Target {
    if(((uint)p.x+sample)%7==0)discard;
    float3 projected=float3(p.xy*.75,p.w);
    float3 selected=asuint(v[2].x)?projected:float3(p.xy,p.w);
    uint2 pixel=uint2(selected.xy-viewport[20].xy);
    uint2 tile=pixel>>l[22].x;
    uint tileRow=(tile.y*l[22].y+tile.x)*l[19].z+l[19].y;
    uint sliceRow=(uint)clamp(selected.z*asfloat(l[23].x),0,asfloat(l[23].y))*l[19].z+l[19].x;
    float4 sum=float4(p.xy*.00001,sample*.0001,0);
    uint word=0;
    [loop]while(word<l[19].z){
        uint bits=masks[tileRow+word]&masks[sliceRow+word];
        uint base=word<<5;
        [loop]while(bits){
            uint bit=firstbitlow(bits),record=base+bit;
            float4 a=lights[record];
            sum+=a*surface.Sample(materialSampler,p.xy*.007+a.xy*.125);
            bits&=bits-1;
        }
        ++word;
    }
    return sum+word*.00001;
}
)";
    auto code=compile(source,"ps_5_0");std::vector<uint8_t> patched;
    if(!NativeSparseClusterProgram(code->GetBufferPointer(),code->GetBufferSize(),patched)){
        ComPtr<ID3DBlob> assembly;D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),4,nullptr,&assembly);
        std::cerr<<static_cast<const char*>(assembly->GetBufferPointer());Check(false,"Recognize fixture traversal");}
    ComPtr<ID3D11PixelShader> old,newer;
    Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&old))&&SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&newer)),"Both shaders validate");
    auto vsCode=compile("float4 main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"Screen VS");
    auto buffer=[&](UINT bytes,UINT stride,UINT flags,const void* data){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=flags;
        if(stride){d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;}D3D11_SUBRESOURCE_DATA initial{data};ComPtr<ID3D11Buffer> b;
        Check(SUCCEEDED(device->CreateBuffer(&d,data?&initial:nullptr,&b)),"Buffer");return b;};
    auto srview=[&](ID3D11Buffer* b){ComPtr<ID3D11ShaderResourceView> s;Check(SUCCEEDED(device->CreateShaderResourceView(b,nullptr,&s)),"Buffer SRV");return s;};
    auto cb=buffer(24*16,0,D3D11_BIND_CONSTANT_BUFFER,nullptr),view=buffer(36*16,0,D3D11_BIND_CONSTANT_BUFFER,nullptr),viewport=buffer(21*16,0,D3D11_BIND_CONSTANT_BUFFER,nullptr);
    std::vector<float> radiance(1056*4);for(unsigned i=0;i<radiance.size();++i)radiance[i]=float(int(i*17%71)-35)*.00003f;
    auto light=buffer(UINT(radiance.size()*4),16,D3D11_BIND_SHADER_RESOURCE,radiance.data());auto lightSrv=srview(light.Get());
    // Mipped, varying material data makes implicit derivative changes observable.
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=7;td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::vector<float>> mipData(7);D3D11_SUBRESOURCE_DATA initial[7]{};
    for(unsigned mip=0;mip<7;++mip){unsigned size=64>>mip;mipData[mip].resize(size*size*4);
        for(unsigned i=0;i<mipData[mip].size();++i)mipData[mip][i]=float((i*7+mip*13)%31)*.03f;initial[mip]={mipData[mip].data(),size*16,0};}
    ComPtr<ID3D11Texture2D> material;ComPtr<ID3D11ShaderResourceView> materialSrv;Check(SUCCEEDED(device->CreateTexture2D(&td,initial,&material))&&SUCCEEDED(device->CreateShaderResourceView(material.Get(),nullptr,&materialSrv)),"Mipped texture");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Material sampler");
    unsigned comparisons=0;
    for(unsigned samples:{1u,4u,8u}){
        D3D11_TEXTURE2D_DESC d{};d.Width=64;d.Height=8;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=samples;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> image,flat,staging;ComPtr<ID3D11RenderTargetView> target,flatTarget;ComPtr<ID3D11ShaderResourceView> imageSrv;
        Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&image))&&SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&target))&&SUCCEEDED(device->CreateShaderResourceView(image.Get(),nullptr,&imageSrv)),"MSAA image");
        d.Width*=samples;d.SampleDesc.Count=1;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&flat))&&SUCCEEDED(device->CreateRenderTargetView(flat.Get(),nullptr,&flatTarget)),"Every-sample target");d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Readback");
        auto extractCode=compile(samples>1?"Texture2DMS<float4> t:register(t1);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int2((uint)p.x/"+std::to_string(samples)+",p.y),(uint)p.x%"+std::to_string(samples)+");}":"Texture2D<float4> t:register(t1);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(p.xy,0));}","ps_5_0");ComPtr<ID3D11PixelShader> extract;
        Check(SUCCEEDED(device->CreatePixelShader(extractCode->GetBufferPointer(),extractCode->GetBufferSize(),nullptr,&extract)),"Sample extraction");
        for(unsigned words:{1u,28u,32u,33u})for(unsigned pattern=0;pattern<4;++pattern)for(unsigned mode=0;mode<3;++mode){
            const unsigned tileBase=0,sliceBase=2*words,span=3*words;
            std::vector<uint32_t> masks(span*2,0);for(unsigned tile=0;tile<2;++tile)for(unsigned w=0;w<words;++w){
                uint32_t bits=pattern==0?0:pattern==1?((w+tile)%5==0?0x81000001u:0):pattern==2?(w==words-1?0x40000002u:0):0xffffffffu;
                masks[tile*words+w]=bits;}
            for(unsigned w=0;w<words;++w)masks[sliceBase+w]=w%3?0x10842101u:0;
            for(unsigned tile=0;tile<2;++tile){uint32_t occupied=0;for(unsigned w=0;w<words&&w<32;++w)if(masks[tile*words+w])occupied|=1u<<w;
                for(unsigned w=0;w<words;++w)masks[span+tile*words+w]=occupied;}
            auto mb=buffer(UINT(masks.size()*4),4,D3D11_BIND_SHADER_RESOURCE,masks.data());auto ms=srview(mb.Get());
            std::array<uint32_t,96> rows{};rows[19*4]=sliceBase;rows[19*4+1]=tileBase;rows[19*4+2]=words;rows[22*4]=mode==2?31:5;rows[22*4+1]=2;
            std::array<float,144> viewRows{};viewRows[8]=mode==1?1.f:0.f;std::array<float,84> vp{};vp[82]=64;vp[83]=8;
            ctx->UpdateSubresource(view.Get(),0,nullptr,viewRows.data(),0,0);ctx->UpdateSubresource(viewport.Get(),0,nullptr,vp.data(),0,0);
            auto draw=[&](ID3D11PixelShader* ps,unsigned offset){rows[19*4+3]=offset;ctx->UpdateSubresource(cb.Get(),0,nullptr,rows.data(),0,0);
                auto b=cb.Get();ctx->PSSetConstantBuffers(3,1,&b);b=view.Get();ctx->PSSetConstantBuffers(4,1,&b);b=viewport.Get();ctx->PSSetConstantBuffers(1,1,&b);
                auto m=ms.Get();ctx->PSSetShaderResources(13,1,&m);m=lightSrv.Get();ctx->PSSetShaderResources(19,1,&m);m=materialSrv.Get();ctx->PSSetShaderResources(0,1,&m);auto sam=sampler.Get();ctx->PSSetSamplers(0,1,&sam);
                const float clear[]{-.5,-.25,-.125,-1};ctx->ClearRenderTargetView(target.Get(),clear);auto r=target.Get();ctx->OMSetRenderTargets(1,&r,nullptr);
                D3D11_VIEWPORT v{0,0,64,8,0,1};ctx->RSSetViewports(1,&v);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
                m=imageSrv.Get();ctx->PSSetShaderResources(1,1,&m);r=flatTarget.Get();ctx->OMSetRenderTargets(1,&r,nullptr);v.Width=64.f*samples;ctx->RSSetViewports(1,&v);ctx->PSSetShader(extract.Get(),nullptr,0);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);m=nullptr;ctx->PSSetShaderResources(1,1,&m);
                ctx->CopyResource(staging.Get(),flat.Get());D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Read samples");std::vector<uint32_t> result(64*8*samples*4);
                for(unsigned y=0;y<8;++y)memcpy(result.data()+y*64*samples*4,static_cast<const char*>(map.pData)+y*map.RowPitch,64*samples*16);ctx->Unmap(staging.Get(),0);return result;};
            auto expected=draw(old.Get(),0),actual=draw(newer.Get(),words<=32?span:0);
            Check(actual==expected,"Sparse words retain every sample bit, ordering, derivatives, fallback and exit index");++comparisons;
        }
    }
    Check(!NativeSparseClusterProgram(code->GetBufferPointer(),31,patched),"Truncated bytecode falls back");
    const auto reserved=source.substr(0,source.find("return sum+word"))+"return sum+asfloat(l[19].w);}\n";
    auto reservedCode=compile(reserved,"ps_5_0");
    Check(!NativeSparseClusterProgram(reservedCode->GetBufferPointer(),reservedCode->GetBufferSize(),patched),"Reserved native component retains original program");
    unsigned fixtureCount=0;
    if(argc>2)for(const auto& entry:std::filesystem::directory_iterator(argv[2]))if(entry.path().extension()==L".dxbc"){
        std::ifstream f(entry.path(),std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        if(NativeSparseClusterProgram(bytes.data(),bytes.size(),patched)){ComPtr<ID3D11PixelShader> ps;Check(SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps)),"Captured sparse shader validates");++fixtureCount;}
    }
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto m=reinterpret_cast<D3D11_MESSAGE*>(data.data());debug->GetMessage(i,m,&size);Check(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);}
    std::cout<<comparisons<<" bitwise every-sample traversal comparisons; "<<fixtureCount<<" captured variants validated\n";
}
