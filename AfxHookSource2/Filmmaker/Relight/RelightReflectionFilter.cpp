#define NOMINMAX
#include "RelightReflectionFilter.h"
#include "../Environment/EnvironmentShaderCompiler.h"
namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char FilterShader[]=R"HLSL(
TextureCubeArray<float4> source:register(t0);
SamplerState linearMip:register(s0);
RWTexture2DArray<float4> output:register(u0);
cbuffer Params:register(b0){uint size;float3 unused;float4 taps[32];}
float3 Direction(uint face,float2 uv){
    float2 c=uv*2-1;
    return normalize(face==0?float3(1,-c.y,-c.x):face==1?float3(-1,-c.y,c.x):face==2?float3(c.x,1,c.y):
        face==3?float3(c.x,-1,-c.y):face==4?float3(c.x,-c.y,1):float3(-c.x,-c.y,-1));
}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
    if(id.x>=size||id.y>=size)return;
    float3 n=Direction(id.z,(float2(id.xy)+.5)/size);
    float3 axis=abs(n.z)<.999?float3(0,0,1):float3(1,0,0);
    float3 tangent=normalize(cross(axis,n)),bitangent=cross(n,tangent);
    float3 sum=0;float weight=0;
    [loop]for(uint i=0;i<32;++i){
        float4 tap=taps[i];if(tap.z<=0)continue;
        float3 d=tangent*tap.x+bitangent*tap.y+n*tap.z;
        sum+=source.SampleLevel(linearMip,float4(d,0),tap.w).rgb*tap.z;weight+=tap.z;
    }
    output[id]=float4(sum/max(weight,1e-6),1);
}
)HLSL";
}
bool ReflectionFilter::Initialize(ID3D11Device* device,unsigned inputSize){
    initialized=false;
    if(!device||!inputSize||(inputSize&(inputSize-1)))return false;
    size=inputSize;mips=1;for(auto n=size;n>1;n>>=1)++mips;
    ComPtr<ID3DBlob> code,error;
    if(FAILED(Environment::CompileEnvironmentShader(FilterShader,sizeof(FilterShader)-1,"RelightReflectionFilter",nullptr,
        "Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&error))||
       FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=mips;d.ArraySize=6;d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&texture)))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=d.Format;s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    s.TextureCubeArray.MipLevels=mips;s.TextureCubeArray.NumCubes=1;
    if(FAILED(device->CreateShaderResourceView(texture.Get(),&s,&view)))return false;
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(device->CreateSamplerState(&sd,&sampler)))return false;
    outputs.resize(mips);constants.resize(mips);
    for(unsigned mip=1;mip<mips;++mip){
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=d.Format;u.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        u.Texture2DArray.MipSlice=mip;u.Texture2DArray.ArraySize=6;
        struct Params{UINT size,pad[3];std::array<ReflectionFilterTap,ReflectionFilterSamples> taps;};
        const Params p{size>>mip,{},ReflectionFilterKernel(mip,mips,size)};
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(p);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;
        const D3D11_SUBRESOURCE_DATA initial{&p};
        if(FAILED(device->CreateUnorderedAccessView(texture.Get(),&u,&outputs[mip]))||
           FAILED(device->CreateBuffer(&b,&initial,&constants[mip])))return false;
    }
    initialized=true;return true;
}
bool ReflectionFilter::Apply(ID3D11DeviceContext* ctx,ID3D11Texture2D* source,ID3D11ShaderResourceView* input){
    if(!initialized||!ctx||!source||!input)return false;
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    if(d.Width!=size||d.Height!=size||d.ArraySize!=6||d.MipLevels!=mips||d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;
    for(unsigned face=0;face<6;++face)ctx->CopySubresourceRegion(texture.Get(),face*mips,0,0,0,source,face*mips,nullptr);
    ctx->CSSetShaderResources(0,1,&input);auto ss=sampler.Get();ctx->CSSetSamplers(0,1,&ss);
    ctx->CSSetShader(shader.Get(),nullptr,0);
    for(unsigned mip=1;mip<mips;++mip){
        auto cb=constants[mip].Get();auto uav=outputs[mip].Get();ctx->CSSetConstantBuffers(0,1,&cb);
        ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);const unsigned groups=((size>>mip)+7)/8;ctx->Dispatch(groups,groups,6);
    }
    ID3D11ShaderResourceView* noView=nullptr;ID3D11UnorderedAccessView* noOutput=nullptr;
    ctx->CSSetShaderResources(0,1,&noView);ctx->CSSetUnorderedAccessViews(0,1,&noOutput,nullptr);
    return true;
}
}
