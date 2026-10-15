#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentFoliageShaders.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentFoliage.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
namespace Filmmaker { void MvmDebugLog_LinefAlways(const char*,const char*,...){} bool MvmDebugLog_Active(){return false;} }
void Check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
struct Vertex {
    float p[3],uv[2]{},normal[3]{0,0,1},tangent[4]{1,0,0,1};
    unsigned bone[4]{},instance=0;
    float color[4]{1,.5f,1,1};
};
int main(int argc,char **argv){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    auto buffer=[&](const void *data,UINT bytes,UINT flags,UINT stride=0){
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=flags;d.Usage=D3D11_USAGE_DEFAULT;
        if(stride){d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=stride;}
        D3D11_SUBRESOURCE_DATA initial{data,0,0};ComPtr<ID3D11Buffer> result;
        Check(SUCCEEDED(device->CreateBuffer(&d,data?&initial:nullptr,&result)),"Buffer");return result;
    };
    auto compile=[&](const std::string &source){
        ComPtr<ID3DBlob> code,errors;
        HRESULT hr=CompileEnvironmentShader(source.data(),source.size(),"FoliageTest",nullptr,"Main","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&errors);
        if(FAILED(hr)&&errors)std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
        Check(SUCCEEDED(hr),"Compile foliage VS");return code;
    };
    const std::string source=std::string("#define foliageNoise g_tNoiseMap\n")+FoliageDeformationShader+R"(
        cbuffer Material:register(b0){float4 g_flSwayAmount;float4 g_flFlutterAmount;}
        cbuffer PerViewConstantBuffer_t:register(b1){float4 pad[18];float3 unusedClock;float g_flGameTime;}
        cbuffer PerViewConstantBufferCsgo_t:register(b3){float4 windPad[17];float4 g_vWindDirection;float4 g_vWindStrengthFreqMulHighStrength;}
        struct Out{float4 p:SV_Position;float3 world:TEXCOORD0;};
        Out Main(float3 p:POSITION,float4 color:COLOR0){Out o;
            o.world=FoliagePosition(p,color.xyz,g_flSwayAmount,g_flFlutterAmount,g_flGameTime,g_vWindDirection.xyz,g_vWindStrengthFreqMulHighStrength.x);
            o.p=float4(o.world,1);return o;}
    )";
    auto ours=compile(source);
    std::vector<char> native;
    if(argc>1){std::ifstream input(argv[1],std::ios::binary);native.assign(std::istreambuf_iterator<char>(input),{});Check(!native.empty(),"Read locally extracted native VS");}
    struct Program{ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11GeometryShader> so;ComPtr<ID3D11InputLayout> layout;};
    auto program=[&](const void *code,size_t bytes,bool full){
        Program p;Check(SUCCEEDED(device->CreateVertexShader(code,bytes,nullptr,&p.vs)),"Create VS");
        D3D11_SO_DECLARATION_ENTRY out{0,"TEXCOORD",0,0,3,0};UINT stride=12;
        Check(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(code,bytes,&out,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&p.so)),"VS pass-through stream output");
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,p),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(Vertex,color),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(Vertex,uv),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(Vertex,normal),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(Vertex,tangent),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_UINT,0,offsetof(Vertex,bone),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",13,DXGI_FORMAT_R32_UINT,0,offsetof(Vertex,instance),D3D11_INPUT_PER_VERTEX_DATA,0}};
        Check(SUCCEEDED(device->CreateInputLayout(elements,full?7:2,code,bytes,&p.layout)),"VS input layout");
        return p;
    };
    auto ourProgram=program(ours->GetBufferPointer(),ours->GetBufferSize(),false);
    Program nativeProgram;if(!native.empty())nativeProgram=program(native.data(),native.size(),true);
    // Deterministic, asymmetric 3D texture catches wrong channels, addressing and coordinate scales.
    std::vector<float> texels(16*16*16*4);unsigned random=17;
    for(auto &v:texels){random=random*1664525u+1013904223u;v=(random>>8)/16777215.f;}
    D3D11_TEXTURE3D_DESC td{};td.Width=td.Height=td.Depth=16;td.MipLevels=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{texels.data(),16*16,16*16*16};ComPtr<ID3D11Texture3D> texture;ComPtr<ID3D11ShaderResourceView> noise;
    Check(SUCCEEDED(device->CreateTexture3D(&td,&init,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&noise)),"3D noise");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Native wrap sampler");
    auto s=sampler.Get();ctx->VSSetSamplers(0,1,&s);auto n=noise.Get();ctx->VSSetShaderResources(12,1,&n);
    float identity[12]{1,0,0,0,0,1,0,0,0,0,1,0};unsigned instance[8]{};
    auto transforms=buffer(identity,sizeof(identity),D3D11_BIND_SHADER_RESOURCE,48),instances=buffer(instance,sizeof(instance),D3D11_BIND_SHADER_RESOURCE,32);
    ComPtr<ID3D11ShaderResourceView> ts,is;
    Check(SUCCEEDED(device->CreateShaderResourceView(transforms.Get(),nullptr,&ts))&&SUCCEEDED(device->CreateShaderResourceView(instances.Get(),nullptr,&is)),"Native structured input");
    ID3D11ShaderResourceView *srvs[]{ts.Get(),is.Get()};ctx->VSSetShaderResources(13,2,srvs);
    std::array<float,52> material{};std::array<float,100> clock{};std::array<float,164> wind{};
    auto m=buffer(material.data(),sizeof(material),D3D11_BIND_CONSTANT_BUFFER),c=buffer(clock.data(),sizeof(clock),D3D11_BIND_CONSTANT_BUFFER),w=buffer(wind.data(),sizeof(wind),D3D11_BIND_CONSTANT_BUFFER);
    auto b=m.Get();ctx->VSSetConstantBuffers(0,1,&b);b=c.Get();ctx->VSSetConstantBuffers(1,1,&b);b=w.Get();ctx->VSSetConstantBuffers(3,1,&b);
    constexpr unsigned count=128;
    std::vector<Vertex> vertices(count);
    for(unsigned i=0;i<count;++i){auto &v=vertices[i];v.p[0]=-2200+i*31.f;v.p[1]=std::sin(float(i))*2500;v.p[2]=i*5.f;
        v.color[0]=(i%9)/8.f;v.color[1]=(i%13)/12.f;v.color[2]=(i%17)/16.f;}
    auto vb=buffer(vertices.data(),UINT(vertices.size()*sizeof(Vertex)),D3D11_BIND_VERTEX_BUFFER);
    b=vb.Get();UINT stride=sizeof(Vertex),zero=0;ctx->IASetVertexBuffers(0,1,&b,&stride,&zero);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    auto output=buffer(nullptr,count*12,D3D11_BIND_STREAM_OUTPUT);
    D3D11_BUFFER_DESC rd{};rd.ByteWidth=count*12;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> readback;Check(SUCCEEDED(device->CreateBuffer(&rd,nullptr,&readback)),"Readback");
    auto run=[&](Program &p){
        ctx->UpdateSubresource(m.Get(),0,nullptr,material.data(),0,0);ctx->UpdateSubresource(c.Get(),0,nullptr,clock.data(),0,0);ctx->UpdateSubresource(w.Get(),0,nullptr,wind.data(),0,0);
        ctx->VSSetShader(p.vs.Get(),nullptr,0);ctx->GSSetShader(p.so.Get(),nullptr,0);ctx->IASetInputLayout(p.layout.Get());
        auto dest=output.Get();ctx->SOSetTargets(1,&dest,&zero);ctx->Draw(count,0);ctx->SOSetTargets(0,nullptr,nullptr);
        ctx->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};
        Check(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)),"Map outputs");
        std::vector<float> result(count*3);memcpy(result.data(),map.pData,count*12);ctx->Unmap(readback.Get(),0);return result;
    };
    float maxError=0;
    for(int trial=0;trial<36;++trial){
        material[0]=trial%3==0?0:.2f;material[1]=.15f;material[2]=.25f;material[3]=1.2f;
        material[4]=trial%3==1?0:1;material[5]=.5f;material[6]=1;material[7]=1;
        wind[68]=.25f;wind[69]=-.12f;wind[70]=.03f;wind[72]=trial%4==0?0:.7f;
        clock[75]=trial*137.23f;
        auto result=run(ourProgram);
        Check(result==run(ourProgram),"Paused clock must hold exact shadow pose");
        if(wind[72]==0)for(unsigned i=0;i<count;++i)for(unsigned j=0;j<3;++j)Check(result[3*i+j]==vertices[i].p[j],"Zero native wind disables deformation");
        else {
            clock[75]+=.125f;Check(result!=run(ourProgram),"Advancing native time animates leaves");clock[75]-=.125f;
            Check(result==run(ourProgram),"Seek restores deterministic native pose");
        }
        if(!native.empty()){
            auto expected=run(nativeProgram);
            for(size_t i=0;i<result.size();++i){float error=std::abs(result[i]-expected[i]);maxError=std::max(maxError,error);
                if(error>.015f){std::cerr<<"Native mismatch trial="<<trial<<" component="<<i<<" ours="<<result[i]<<" native="<<expected[i]<<" error="<<error<<'\n';return 1;}}
        }
    }
    // The CPU upload snapshot must be current and belong to a reflected wind
    // binding. GPU-only writes/partial uploads have no proven bound and fail open.
    D3D11_TEXTURE3D_DESC normalizedDesc{};normalizedDesc.Width=normalizedDesc.Height=normalizedDesc.Depth=1;
    normalizedDesc.MipLevels=1;normalizedDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    normalizedDesc.Usage=D3D11_USAGE_IMMUTABLE;normalizedDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    unsigned char normalizedTexel[4]{127,127,127,255};D3D11_SUBRESOURCE_DATA normalizedInitial{normalizedTexel,4,4};
    ComPtr<ID3D11Texture3D> normalizedTexture;ComPtr<ID3D11ShaderResourceView> normalizedNoise;
    Check(SUCCEEDED(device->CreateTexture3D(&normalizedDesc,&normalizedInitial,&normalizedTexture))&&
        SUCCEEDED(device->CreateShaderResourceView(normalizedTexture.Get(),nullptr,&normalizedNoise)),"Normalized noise has a proven range");
    auto normalizedView=normalizedNoise.Get();ctx->VSSetShaderResources(12,1,&normalizedView);
    NoteFoliageShader(ourProgram.vs.Get(),ours->GetBufferPointer(),ours->GetBufferSize());
    auto observe=[&](){
        ctx->VSSetShader(ourProgram.vs.Get(),nullptr,0);
        auto nativeWind=w.Get();ctx->VSSetConstantBuffers(3,1,&nativeWind);
        RequestFoliageInputs(true);NextFoliageFrame();ObserveFoliageInputs(ctx.Get());
    };
    Vec3 cpuWind;float cpuStrength=0;
    observe();Check(!ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength),"Unknown upload must not block on GPU readback");
    NoteFoliageConstantUpdate(w.Get(),nullptr,wind.data());observe();
    Check(ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength)&&cpuWind.x==wind[68]&&cpuStrength==wind[72],"Full CPU upload is the exact reflected wind source");
    D3D11_BOX partial{272,0,0,288,1,1};
    NoteFoliageConstantUpdate(w.Get(),&partial,wind.data()+68);observe();
    Check(!ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength),"Partial upload cannot retain stale strength");
    D3D11_BOX fullWind{272,0,0,304,1,1};
    NoteFoliageConstantUpdate(w.Get(),&fullWind,wind.data()+68);observe();
    Check(ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength)&&cpuStrength==wind[72],"Box offset CPU upload uses the right source bytes");
    InvalidateFoliageConstantCopy(w.Get());observe();
    Check(!ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength),"GPU copy invalidates CPU upload proof");
    NoteFoliageConstantMap(w.Get(),D3D11_MAP_WRITE_DISCARD,wind.data());observe();
    Check(!ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength),"Mapped write is incomplete until Unmap");
    NoteFoliageConstantUnmap(w.Get());observe();
    Check(ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength)&&cpuWind.y==wind[69],"Completed CPU Map write updates wind without GPU readback");
    wind[72]=NAN;NoteFoliageConstantUpdate(w.Get(),nullptr,wind.data());observe();
    Check(!ReadFoliageWind(ctx.Get(),cpuWind,cpuStrength),"Nonfinite wind cannot shrink the conservative bound");
    ResetFoliageInputs();
    // Reflection-based bridge accepts the real shader; it does not mutate native bindings.
    if(!native.empty()){
        NoteFoliageShader(nativeProgram.vs.Get(),native.data(),native.size());
        RequestFoliageInputs(true);NextFoliageFrame();ObserveFoliageInputs(ctx.Get());
        unsigned long long generation=0;Check(BindFoliageInputs(ctx.Get(),generation)&&generation>0,"Native clock/wind/noise snapshot bridge");
        auto first=generation;RequestFoliageInputs(false);NextFoliageFrame();ObserveFoliageInputs(ctx.Get());
        Check(BindFoliageInputs(ctx.Get(),generation)&&generation>first,"Next frame invalidates animated shadow maps");
        ResetFoliageInputs();Check(!BindFoliageInputs(ctx.Get(),generation),"Device reset releases native resources");
    }
    std::cout<<"Foliage motion/pause/seek tests passed; native="<<!native.empty()<<" vertices="<<count*36<<" maxPositionError="<<maxError<<'\n';
}
