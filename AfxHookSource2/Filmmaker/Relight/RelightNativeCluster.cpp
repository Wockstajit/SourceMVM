#define NOMINMAX
#include "RelightNativeCluster.h"
#include "RelightClusterShaders.h"
#include "RelightNativeClusterShaders.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace Filmmaker::Relight {
namespace {
using Microsoft::WRL::ComPtr;
struct Kernels {std::mutex lock;ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> depth,tile;std::vector<uint8_t> depthCode,tileCode;};
Kernels& Cache(){static Kernels* cache=new Kernels;return *cache;}
// The two engine programs share CullParams_t. Require the names, offsets,
// resource bindings and thread groups observed in the captured native programs.
unsigned Classify(const void* code,size_t bytes){
    ComPtr<ID3D11ShaderReflection> reflection;
    const auto library=Environment::EnvironmentShaderCompiler();
    const auto reflect=library?reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(library,"D3DReflect")):nullptr;
    if(!reflect||FAILED(reflect(code,bytes,IID_PPV_ARGS(&reflection))))return 0;
    auto block=reflection->GetConstantBufferByName("CullParams_t");D3D11_SHADER_BUFFER_DESC bd{};
    if(!block||FAILED(block->GetDesc(&bd))||bd.Size!=160)return 0;
    const std::pair<const char*,UINT> fields[]={{"g_nTiles",0},{"g_nTilesX",4},{"g_nTilesY",8},
        {"g_vTileToCenterScale",16},{"g_vTileToCenterOffset",24},{"g_nDepthBins",32},
        {"g_fDepthBinWidth",36},{"g_fBinEpsilon",40},{"g_fNearPlane",44},
        {"g_nFirstMaskForBatch",48},{"g_TileBatches",64},{"g_BinBatches",112}};
    for(const auto& field:fields){D3D11_SHADER_VARIABLE_DESC d{};
        if(FAILED(block->GetVariableByName(field.first)->GetDesc(&d))||d.StartOffset!=field.second)return 0;}
    auto binding=[&](const char* name,D3D_SHADER_INPUT_TYPE type,UINT slot){D3D11_SHADER_INPUT_BIND_DESC d{};
        return SUCCEEDED(reflection->GetResourceBindingDescByName(name,&d))&&d.Type==type&&d.BindPoint==slot&&d.BindCount==1;};
    if(!binding("CullParams_t",D3D_SIT_CBUFFER,0)||!binding("g_OutputCullBits",D3D_SIT_UAV_RWSTRUCTURED,0))return 0;
    UINT x=0,y=0,z=0;reflection->GetThreadGroupSize(&x,&y,&z);
    if(x==32&&y==1&&z==1&&binding("g_CullItems",D3D_SIT_STRUCTURED,0)&&NativeClusterDeclarations(code,bytes,1))return 1;
    if(x==8&&y==4&&z==1&&binding("g_CullItems",D3D_SIT_STRUCTURED,1)&&binding("g_CullPlanes",D3D_SIT_STRUCTURED,0)&&NativeClusterDeclarations(code,bytes,2))return 2;
    return 0;
}
}
void NoteNativeClusterShader(ID3D11ComputeShader* shader,const void* code,size_t bytes){
    if(!shader||!code||bytes<32||bytes>1024*1024)return;
    // Avoid reflecting unrelated skinning, smoke and post-processing programs.
    static constexpr char name[]="CullParams_t";
    const auto* data=static_cast<const uint8_t*>(code);
    if(std::search(data,data+bytes,name,name+sizeof(name)-1)==data+bytes)return;
    const auto kind=Classify(code,bytes);if(!kind)return;
    ComPtr<ID3D11Device> device;shader->GetDevice(&device);auto& cache=Cache();std::lock_guard<std::mutex> lock(cache.lock);
    if(cache.device.Get()!=device.Get()){cache.device=device;cache.depth.Reset();cache.tile.Reset();cache.depthCode.clear();cache.tileCode.clear();}
    (kind==1?cache.depth:cache.tile)=shader;
    auto& stored=kind==1?cache.depthCode:cache.tileCode;stored.assign(data,data+bytes);
}
unsigned DumpNativeClusterPrograms(const std::string& directory){
    auto& cache=Cache();std::lock_guard<std::mutex> lock(cache.lock);unsigned written=0;
    for(unsigned kind=0;kind<2;++kind){const auto& code=kind?cache.tileCode:cache.depthCode;if(code.empty())continue;
        std::ofstream file(std::filesystem::u8path(directory)/(kind?"native-cluster-tile.dxbc":"native-cluster-depth.dxbc"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()),code.size());if(file)++written;}
    return written;
}
bool NativeClusterWork::Ensure(ID3D11Device* device,uint32_t count){
    {auto& cache=Cache();std::lock_guard<std::mutex> lock(cache.lock);
        if(cache.device.Get()!=device||!cache.depth||!cache.tile)return false;
        depthKernel=cache.depth;tileKernel=cache.tile;}
    if(!prepare||!finalize){const std::string source=std::string(ClusterShader)+NativeClusterAdapter;
        for(unsigned pass=0;pass<2;++pass){auto& shader=pass?finalize:prepare;if(shader)continue;
            ComPtr<ID3DBlob> code,error;
            if(FAILED(Environment::CompileEnvironmentShader(source.data(),source.size(),"RelightNativeClusterAdapter",nullptr,
                pass?"NativeFinalize":"NativePrepare","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||
                FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;}}
    if(!parameters){D3D11_BUFFER_DESC d{};d.ByteWidth=160;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&parameters)))return false;}
    const uint32_t padded=((count+31)/32)*32;
    if(capacity!=padded){items.Reset();planes.Reset();itemSrv.Reset();planeSrv.Reset();itemUav.Reset();planeUav.Reset();capacity=0;
        auto buffer=[&](UINT stride,UINT n,ComPtr<ID3D11Buffer>& b,ComPtr<ID3D11ShaderResourceView>& srv,ComPtr<ID3D11UnorderedAccessView>& uav){
            D3D11_BUFFER_DESC d{};d.ByteWidth=stride*n;d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            return SUCCEEDED(device->CreateBuffer(&d,nullptr,&b))&&SUCCEEDED(device->CreateShaderResourceView(b.Get(),nullptr,&srv))&&
                SUCCEEDED(device->CreateUnorderedAccessView(b.Get(),nullptr,&uav));};
        if(!buffer(80,padded,items,itemSrv,itemUav)||!buffer(16,padded*4,planes,planeSrv,planeUav))return false;
        capacity=padded;const char itemName[]="SourceMvmNativeCullItems",planeName[]="SourceMvmNativeCullPlanes";
        items->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(itemName)-1,itemName);
        planes->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(planeName)-1,planeName);}
    return true;
}
void NativeClusterWork::Record(ID3D11DeviceContext* work,const ClusterGrid& grid,ID3D11Buffer* clusterParams,
    ID3D11ShaderResourceView* authored,ID3D11ShaderResourceView* extents,ID3D11UnorderedAccessView* masks,ID3D11Buffer* gatedArguments){
    auto dispatch=[&](unsigned slot,UINT x,UINT y=1,UINT z=1){if(gatedArguments)work->DispatchIndirect(gatedArguments,slot*12);else work->Dispatch(x,y,z);};
    work->CSSetConstantBuffers(0,1,&clusterParams);work->CSSetShaderResources(1,1,&extents);
    ID3D11UnorderedAccessView* outputs[]{itemUav.Get(),planeUav.Get()};work->CSSetUnorderedAccessViews(3,2,outputs,nullptr);
    work->CSSetShader(prepare.Get(),nullptr,0);dispatch(1,(grid.lights.count+63)/64);
    ID3D11UnorderedAccessView* empty[2]{};work->CSSetUnorderedAccessViews(3,2,empty,nullptr);
    const auto p=MakeNativeClusterParams(grid);
    pending=p;
    if(!hasParams||std::memcmp(&p,&prior,sizeof(p)))work->UpdateSubresource(parameters.Get(),0,nullptr,&p,0,0);
    auto cb=parameters.Get();work->CSSetConstantBuffers(0,1,&cb);
    auto item=itemSrv.Get();work->CSSetShaderResources(0,1,&item);work->CSSetUnorderedAccessViews(0,1,&masks,nullptr);
    work->CSSetShader(depthKernel.Get(),nullptr,0);dispatch(3,(grid.sliceCount+31)/32,grid.lights.words);
    ID3D11ShaderResourceView* inputs[]{planeSrv.Get(),itemSrv.Get()};work->CSSetShaderResources(0,2,inputs);
    work->CSSetShader(tileKernel.Get(),nullptr,0);dispatch(4,(grid.tilesX+7)/8,(grid.tilesY+3)/4,grid.lights.words);
    work->CSSetUnorderedAccessViews(0,1,empty,nullptr);
    work->CSSetConstantBuffers(0,1,&clusterParams);work->CSSetShaderResources(0,1,&authored);work->CSSetShaderResources(1,1,&extents);
    work->CSSetUnorderedAccessViews(1,1,&masks,nullptr);work->CSSetShader(finalize.Get(),nullptr,0);
    dispatch(2,(grid.TileCount()+grid.sliceCount+1+63)/64);
}
}
