#pragma once
#include "RelightNativeClusterMath.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

namespace Filmmaker::Relight {
// Called by CreateComputeShader; shader creation may run on an engine worker.
void NoteNativeClusterShader(ID3D11ComputeShader* shader,const void* code,size_t bytes);
unsigned DumpNativeClusterPrograms(const std::string& directory);
class NativeClusterWork {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11ComputeShader> depthKernel,tileKernel,prepare,finalize;
    Ptr<ID3D11Buffer> items,planes,parameters;
    Ptr<ID3D11ShaderResourceView> itemSrv,planeSrv;
    Ptr<ID3D11UnorderedAccessView> itemUav,planeUav;
    uint32_t capacity=0;NativeClusterParams prior{},pending{};bool hasParams=false;
public:
    bool Ensure(ID3D11Device* device,uint32_t count);
    void Record(ID3D11DeviceContext* work,const ClusterGrid& grid,ID3D11Buffer* clusterParams,
        ID3D11ShaderResourceView* authored,ID3D11ShaderResourceView* extents,
        ID3D11UnorderedAccessView* masks,ID3D11Buffer* gatedArguments);
    // Publish upload reuse only after the command list is actually executed.
    void Commit(){prior=pending;hasParams=true;}
};
}
