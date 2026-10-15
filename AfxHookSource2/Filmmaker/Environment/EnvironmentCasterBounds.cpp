#define NOMINMAX
#include "EnvironmentCasterBounds.h"
#include "EnvironmentCasterBoundsShaders.h"
#include "EnvironmentShaderCompiler.h"
#include <climits>
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT MaxBounds=8192,BlockVertices=1536;
// Parses one completed readback. `padding` grows every block (late bounds); any malformed block
// fails the whole read so the caller falls back to keeping every caster.
bool ParseBounds(ID3D11DeviceContext* immediate,ID3D11Buffer* readback,UINT blocks,bool wait,float padding,
                 std::vector<CasterBounds>& result){
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(immediate->Map(readback,0,D3D11_MAP_READ,wait?0:D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)))return false;
    const auto data=static_cast<const CasterBounds*>(mapped.pData);
    for(UINT i=0;i<blocks;++i)if(data[i].valid!=2)result.push_back(data[i]);
    immediate->Unmap(readback,0);
    for(auto& b:result){
        if(!ValidCasterBounds(b)){result.clear();return false;}
        b.low=b.low-Vec3{padding,padding,padding};b.high=b.high+Vec3{padding,padding,padding};
    }
    return true;
}
}
bool CasterBoundsReader::Initialize(ID3D11Device* device) {
    if(!device)return false;
    ComPtr<ID3DBlob> code,error;
    if(FAILED(CompileEnvironmentShader(CasterBoundsShader,sizeof(CasterBoundsShader)-1,"EnvironmentCasterBounds",
        nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||
       FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;
    D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateBuffer(&d,nullptr,&constants)))return false;
    d.ByteWidth=MaxBounds*sizeof(CasterBounds);d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(CasterBounds);
    if(FAILED(device->CreateBuffer(&d,nullptr,&output)))return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_UNKNOWN;v.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    v.Buffer.NumElements=MaxBounds;
    if(FAILED(device->CreateUnorderedAccessView(output.Get(),&v,&view)))return false;
    d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(auto& slot:readback)if(FAILED(device->CreateBuffer(&d,nullptr,&slot)))return false;
    return true;
}
bool CasterBoundsReader::Read(ID3D11DeviceContext* immediate,const CasterVertexStream* streams,UINT streamCount,
                             std::vector<CasterBounds>& result,bool waitForGpu) {
    result.clear();if(!immediate||!shader||!readback[0])return false;
    UINT blocks=0;for(UINT i=0;i<streamCount;++i){const auto& s=streams[i];
        if(s.count &&(!s.buffer||s.stride<12||s.count%3||uint64_t(s.count)*s.stride>UINT_MAX))return false;
        const uint64_t next=uint64_t(blocks)+(s.gpuCount?1:(uint64_t(s.count)+BlockVertices-1)/BlockVertices);
        if(next>MaxBounds)return false;blocks=UINT(next);}
    if(!blocks)return true;
    ComPtr<ID3D11Device> device;immediate->GetDevice(&device);
    std::vector<ComPtr<ID3D11ShaderResourceView>> inputs(streamCount);
    for(UINT i=0;i<streamCount;++i){const auto& s=streams[i];if(!s.count)continue;
        D3D11_BUFFER_DESC d{};s.buffer->GetDesc(&d);
        if(uint64_t(s.count)*s.stride>d.ByteWidth)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;v.BufferEx.NumElements=d.ByteWidth/4;v.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(device->CreateShaderResourceView(s.buffer,&v,&inputs[i])))return false;
    }
    // Dispatched on the immediate context with only the compute slots used here saved and put back:
    // replaying a deferred command list is what ReShade's d3d11 wrapper crashes in (2026-10-13).
    ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount=256;
    immediate->CSGetShader(previousShader.GetAddressOf(),previousInstances,&previousInstanceCount);
    ID3D11Buffer* previousConstants=nullptr;immediate->CSGetConstantBuffers(0,1,&previousConstants);
    ID3D11ShaderResourceView* previousViews[2]{};immediate->CSGetShaderResources(0,2,previousViews);
    ID3D11UnorderedAccessView* previousUav=nullptr;immediate->CSGetUnorderedAccessViews(0,1,&previousUav);
    immediate->CSSetShader(shader.Get(),nullptr,0);
    auto cb=constants.Get();immediate->CSSetConstantBuffers(0,1,&cb);
    auto uav=view.Get();immediate->CSSetUnorderedAccessViews(0,1,&uav,nullptr);UINT base=0;
    for(UINT i=0;i<streamCount;++i){const auto& s=streams[i];if(!s.count)continue;
        ID3D11ShaderResourceView* srvs[]{inputs[i].Get(),s.gpuCount};immediate->CSSetShaderResources(0,2,srvs);
        const UINT values[]{s.count,s.stride,base,s.gpuCount?1u:0u};immediate->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);
        const UINT groups=s.gpuCount?1:(s.count+BlockVertices-1)/BlockVertices;immediate->Dispatch(groups,1,1);base+=groups;
    }
    UINT keepCounter=~0u;immediate->CSSetUnorderedAccessViews(0,1,&previousUav,&keepCounter);
    immediate->CSSetShaderResources(0,2,previousViews);immediate->CSSetConstantBuffers(0,1,&previousConstants);
    immediate->CSSetShader(previousShader.Get(),previousInstances,previousInstanceCount);
    for(UINT i=0;i<previousInstanceCount;++i)if(previousInstances[i])previousInstances[i]->Release();
    for(auto* v:previousViews)if(v)v->Release();
    if(previousConstants)previousConstants->Release();if(previousUav)previousUav->Release();

    const unsigned slot=nextSlot;nextSlot=(nextSlot+1)%Slots;
    D3D11_BOX box{0,0,0,blocks*UINT(sizeof(CasterBounds)),1,1};
    immediate->CopySubresourceRegion(readback[slot].Get(),0,0,0,0,output.Get(),0,&box);
    slotBlocks[slot]=blocks;slotFilled[slot]=true;
    if(waitForGpu)return ParseBounds(immediate,readback[slot].Get(),blocks,true,0.f,result);
    // The oldest slot is the read issued two calls ago: complete without a stall at any normal
    // queue depth. A slot not yet filled (first frames) or still in flight leaves keep-everything.
    const unsigned oldest=nextSlot;
    if(!slotFilled[oldest])return false;
    return ParseBounds(immediate,readback[oldest].Get(),slotBlocks[oldest],false,LateCasterBoundsPadding,result);
}
}
