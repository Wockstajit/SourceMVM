#define NOMINMAX
#include "EnvironmentCasterBounds.h"
#include "EnvironmentCasterBoundsShaders.h"
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentCasterCull.h"
#include "EnvironmentCasterCullShaders.h"
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

bool CasterShadowCuller::Initialize(ID3D11Device* input){
    if(!input)return false;
    device=input;
    auto compile=[&](const char* source,size_t bytes,const char* entry,ComPtr<ID3D11ComputeShader>& result){
        ComPtr<ID3DBlob> code,error;
        return SUCCEEDED(CompileEnvironmentShader(source,bytes,"EnvironmentCasterCull",nullptr,entry,
            "cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code,&error))&&
            SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&result));
    };
    if(!compile(CasterBoundsShader,sizeof(CasterBoundsShader)-1,"SpatialBounds",measure)||
       !compile(CasterCullShader,sizeof(CasterCullShader)-1,"Cull",cull))return false;
    D3D11_BUFFER_DESC d{};d.ByteWidth=16+BatchFaces*96;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&d,nullptr,&constants));
}
void CasterShadowCuller::NextFrame(uint64_t generation){
    for(auto it=entries.begin();it!=entries.end();){
        if(generation>it->second.used+3)it=entries.erase(it);else ++it;
    }
}
bool CasterShadowCuller::Prepare(ID3D11DeviceContext* ctx,const CasterVertexStream& stream,
                               const Matrix* matrices,UINT faces,uint64_t generation){
    if(!ctx||!constants||!measure||!cull||!matrices||!faces||faces>BatchFaces||!stream.buffer||!stream.count||stream.stride<12||
       stream.count>UINT_MAX/stream.stride||(!stream.gpuCount&&stream.count%3))return false;
    for(UINT f=0;f<faces;++f)for(float value:matrices[f])if(!std::isfinite(value))return false;
    D3D11_BUFFER_DESC source{};stream.buffer->GetDesc(&source);
    if(uint64_t(stream.count)*stream.stride>source.ByteWidth||stream.count>UINT_MAX/(BatchFaces*4))return false;
    const UINT blocks=(stream.count+1535)/1536;
    auto& entry=entries[stream.buffer];entry.used=generation;
    if(!entry.sourceView){
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_TYPELESS;
        s.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;s.BufferEx.NumElements=source.ByteWidth/4;
        s.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(device->CreateShaderResourceView(stream.buffer,&s,&entry.sourceView)))return false;
        entry.source=stream.buffer;
    }
    if(!entry.boundsView||entry.blocks<blocks){
        Entry next;next.source=entry.source;next.sourceView=entry.sourceView;next.used=generation;
        D3D11_BUFFER_DESC d{};d.ByteWidth=blocks*sizeof(CasterBounds);
        d.StructureByteStride=sizeof(CasterBounds);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(device->CreateBuffer(&d,nullptr,&next.bounds))||
           FAILED(device->CreateShaderResourceView(next.bounds.Get(),nullptr,&next.boundsView))||
           FAILED(device->CreateUnorderedAccessView(next.bounds.Get(),nullptr,&next.boundsOutput)))return false;
        next.blocks=blocks;entry=std::move(next);
    }
    entry.prepared=~0ull;
    if(!entry.indices||entry.capacity<stream.count){
        Buffer indices,arguments;Uav indexOutput,argumentOutput;
        D3D11_BUFFER_DESC d{};d.ByteWidth=stream.count*BatchFaces*4;
        d.BindFlags=D3D11_BIND_INDEX_BUFFER|D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=stream.count*BatchFaces;
        u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(device->CreateBuffer(&d,nullptr,&indices))||
           FAILED(device->CreateUnorderedAccessView(indices.Get(),&u,&indexOutput)))return false;
        d.ByteWidth=BatchFaces*20;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags|=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;u.Buffer.NumElements=BatchFaces*5;
        if(FAILED(device->CreateBuffer(&d,nullptr,&arguments))||
           FAILED(device->CreateUnorderedAccessView(arguments.Get(),&u,&argumentOutput)))return false;
        entry.indices=std::move(indices);entry.arguments=std::move(arguments);
        entry.indexOutput=std::move(indexOutput);entry.argumentOutput=std::move(argumentOutput);
        entry.capacity=stream.count;
    }
    struct Parameters{UINT count,stride,base,indirect;std::array<std::array<float,4>,BatchFaces*6> planes;};
    Parameters p{stream.count,stream.stride,0,stream.gpuCount?1u:0u,{}};
    for(UINT f=0;f<faces;++f){const auto frustum=ShadowFrustum(matrices[f]);
        std::copy(frustum.planes.begin(),frustum.planes.end(),p.planes.begin()+f*6);}
    static_assert(sizeof(p)==16+BatchFaces*96,"Caster cull constants");
    // Both immediate and deferred callers can have unrelated compute state live.
    ComPtr<ID3D11ComputeShader> previous;ID3D11ClassInstance* instances[256]{};UINT instanceCount=256;
    ctx->CSGetShader(&previous,instances,&instanceCount);
    ComPtr<ID3D11Buffer> previousConstants;ctx->CSGetConstantBuffers(0,1,&previousConstants);
    ID3D11ShaderResourceView* oldViews[2]{};ctx->CSGetShaderResources(0,2,oldViews);
    ID3D11UnorderedAccessView* oldOutputs[2]{};ctx->CSGetUnorderedAccessViews(0,2,oldOutputs);
    ComPtr<ID3D11Buffer> oldIndices;DXGI_FORMAT oldFormat{};UINT oldOffset=0;
    ctx->IAGetIndexBuffer(&oldIndices,&oldFormat,&oldOffset);ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);
    ID3D11ShaderResourceView* noViews[2]{};ID3D11UnorderedAccessView* noOutputs[2]{};
    ctx->CSSetShaderResources(0,2,noViews);ctx->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);
    ctx->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);
    auto cb=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    if(entry.generation!=generation||entry.count!=stream.count||entry.stride!=stream.stride||entry.countSource!=stream.gpuCount){
        ID3D11ShaderResourceView* views[]{entry.sourceView.Get(),stream.gpuCount};
        auto output=entry.boundsOutput.Get();ctx->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        ctx->CSSetShaderResources(0,2,views);ctx->CSSetShader(measure.Get(),nullptr,0);ctx->Dispatch(blocks,1,1);
        ctx->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);ctx->CSSetShaderResources(0,2,noViews);
        entry.generation=generation;entry.count=stream.count;entry.stride=stream.stride;entry.countSource=stream.gpuCount;
    }
    UINT args[BatchFaces*5]{};
    for(UINT f=0;f<faces;++f){args[f*5+1]=1;args[f*5+2]=f*stream.count;}
    ctx->UpdateSubresource(entry.arguments.Get(),0,nullptr,args,0,0);
    ID3D11ShaderResourceView* views[]{entry.boundsView.Get(),stream.gpuCount};
    ID3D11UnorderedAccessView* outputs[]{entry.indexOutput.Get(),entry.argumentOutput.Get()};
    ctx->CSSetShaderResources(0,2,views);ctx->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
    ctx->CSSetShader(cull.Get(),nullptr,0);ctx->Dispatch(blocks,faces,1);
    ctx->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);ctx->CSSetShaderResources(0,2,noViews);
    ctx->IASetIndexBuffer(oldIndices.Get(),oldFormat,oldOffset);
    std::copy_n(matrices,faces,entry.matrices.begin());entry.faceCount=faces;entry.prepared=generation;
    const UINT keep[2]{~0u,~0u};ctx->CSSetUnorderedAccessViews(0,2,oldOutputs,keep);
    ctx->CSSetShaderResources(0,2,oldViews);cb=previousConstants.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    ctx->CSSetShader(previous.Get(),instances,instanceCount);
    for(UINT i=0;i<instanceCount;++i)if(instances[i])instances[i]->Release();
    for(auto* v:oldViews)if(v)v->Release();for(auto* v:oldOutputs)if(v)v->Release();
    return true;
}
bool CasterShadowCuller::Draw(ID3D11DeviceContext* ctx,const CasterVertexStream& stream,
                            const Matrix& matrix,uint64_t generation){
    const auto found=entries.find(stream.buffer);if(!ctx||found==entries.end())return false;
    const auto& e=found->second;
    if(e.prepared!=generation||e.count!=stream.count||e.stride!=stream.stride||e.countSource!=stream.gpuCount)return false;
    UINT face=0;while(face<e.faceCount&&e.matrices[face]!=matrix)++face;
    if(face==e.faceCount)return false;
    ComPtr<ID3D11Buffer> previous;DXGI_FORMAT format;UINT offset=0;
    ctx->IAGetIndexBuffer(&previous,&format,&offset);
    ctx->IASetIndexBuffer(e.indices.Get(),DXGI_FORMAT_R32_UINT,0);
    ctx->DrawIndexedInstancedIndirect(e.arguments.Get(),face*20);
    ctx->IASetIndexBuffer(previous.Get(),format,offset);return true;
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
