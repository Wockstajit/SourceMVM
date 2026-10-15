#pragma once
#include "EnvironmentShaderCompiler.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
namespace Filmmaker::Environment {
// Compare the exact bits read by the rebase and cluster shaders. No CPU readback,
// hashes or engine buffer identities participate in deciding whether to reuse.
inline constexpr char LightingViewGateShader[]=R"HLSL(
cbuffer Gate:register(b0){uint4 groups;
#ifdef SOURCE_MVM_NATIVE_CLUSTERS
    uint4 nativeDimensions;
#endif
}
cbuffer NativeView:register(b4){float4 view[36];}
cbuffer NativeViewport:register(b1){float4 viewport[21];}
RWStructuredBuffer<uint4> previous:register(u0);
RWByteAddressBuffer arguments:register(u1);
[numthreads(1,1,1)]void Main(uint3 id:SV_DispatchThreadID){
    bool changed=groups.w!=0||any(previous[0]!=asuint(view[2]))||
        any(previous[5]!=asuint(view[35]))||any(previous[6]!=asuint(viewport[20]));
    changed=changed||any(previous[1]!=asuint(view[12]))||any(previous[2]!=asuint(view[13]))||any(previous[3]!=asuint(view[14]))||any(previous[4]!=asuint(view[15]));
    if(changed){
        previous[0]=asuint(view[2]);previous[5]=asuint(view[35]);
        previous[1]=asuint(view[12]);previous[2]=asuint(view[13]);previous[3]=asuint(view[14]);previous[4]=asuint(view[15]);
        previous[6]=asuint(viewport[20]);
    }
    uint4 counts=previous[7];counts.x+=changed?1:0;counts.y+=changed?0:1;counts.z+=groups.w!=0?1:0;
    previous[7]=counts;
    arguments.Store3(0,uint3(changed?groups.x:0,1,1));
    arguments.Store3(12,uint3(changed?groups.y:0,1,1));
    arguments.Store3(24,uint3(changed?groups.z:0,1,1));
#ifdef SOURCE_MVM_NATIVE_CLUSTERS
    arguments.Store3(36,uint3(changed?nativeDimensions.x:0,nativeDimensions.y,1));
    arguments.Store3(48,uint3(changed?nativeDimensions.z:0,nativeDimensions.w,nativeDimensions.y));
#endif
}
)HLSL";
class LightingViewGate {
    using Buffer=Microsoft::WRL::ComPtr<ID3D11Buffer>;
    using Uav=Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Buffer inputs,previous,arguments;Uav previousUav,argumentsUav;
    std::vector<uint8_t> committed,pending;std::array<UINT,3> lastGroups{},pendingGroups{};
    bool valid=false,nativeMode=false;
    std::array<UINT,4> lastNative{},pendingNative{};
    bool Ensure(ID3D11Device* device,bool wantedNative){
        if(nativeMode!=wantedNative){shader.Reset();inputs.Reset();arguments.Reset();argumentsUav.Reset();valid=false;nativeMode=wantedNative;}
        if(!shader){Microsoft::WRL::ComPtr<ID3DBlob> code,error;
            // The optimized FXC program drops required snapshot stores; the tiny
            // gate uses the verified unoptimized program. Buffer tests cover all rows.
            const D3D_SHADER_MACRO macros[]={{"SOURCE_MVM_NATIVE_CLUSTERS","1"},{nullptr,nullptr}};
            if(FAILED(CompileEnvironmentShader(LightingViewGateShader,sizeof(LightingViewGateShader)-1,"LightingViewGate",nativeMode?macros:nullptr,
                "Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_SKIP_OPTIMIZATION,&code,&error))||
                FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;
        }
        if(!inputs){D3D11_BUFFER_DESC d{};d.ByteWidth=nativeMode?32:16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&d,nullptr,&inputs)))return false;}
        if(!previous){std::array<UINT,32> zero{};D3D11_SUBRESOURCE_DATA initial{zero.data()};
            D3D11_BUFFER_DESC d{};d.ByteWidth=128;d.StructureByteStride=16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            if(FAILED(device->CreateBuffer(&d,&initial,&previous)))return false;
            const char name[]="SourceMvmLightingViewGate";previous->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(name)-1,name);valid=false;
        }
        if(!previousUav&&FAILED(device->CreateUnorderedAccessView(previous.Get(),nullptr,&previousUav)))return false;
        if(!arguments){D3D11_BUFFER_DESC d{};d.ByteWidth=nativeMode?60:36;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
            d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
            if(FAILED(device->CreateBuffer(&d,nullptr,&arguments)))return false;
            const char name[]="SourceMvmLightingViewDispatchArgs";arguments->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(name)-1,name);valid=false;
        }
        if(!argumentsUav){D3D11_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements=nativeMode?15:9;d.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
            if(FAILED(device->CreateUnorderedAccessView(arguments.Get(),&d,&argumentsUav)))return false;}
        return true;
    }
public:
    void Invalidate(){valid=false;}
    ID3D11Buffer* Arguments()const{return arguments.Get();}
    ID3D11Buffer* Counters()const{return previous.Get();}
    bool Prepare(ID3D11Device* device,ID3D11DeviceContext* work,ID3D11Buffer* view,ID3D11Buffer* viewport,
        const std::vector<uint8_t>& cpuKey,const std::array<UINT,3>& groups,bool authoredChanged,const std::array<UINT,4>& nativeDimensions={}){
        D3D11_BUFFER_DESC v{},p{};if(view)view->GetDesc(&v);if(viewport)viewport->GetDesc(&p);
        const bool wantedNative=nativeDimensions[1]!=0;
        if(v.ByteWidth<36*16||p.ByteWidth<21*16||!Ensure(device,wantedNative)){Invalidate();return false;}
        pending=cpuKey;pendingGroups=groups;pendingNative=nativeDimensions;
        const UINT values[]{groups[0],groups[1],groups[2],UINT(authoredChanged||!valid||committed!=pending||lastGroups!=groups||lastNative!=pendingNative),
            nativeDimensions[0],nativeDimensions[1],nativeDimensions[2],nativeDimensions[3]};
        work->UpdateSubresource(inputs.Get(),0,nullptr,values,0,0);
        auto b=inputs.Get();work->CSSetConstantBuffers(0,1,&b);work->CSSetConstantBuffers(4,1,&view);work->CSSetConstantBuffers(1,1,&viewport);
        ID3D11UnorderedAccessView* uavs[]{previousUav.Get(),argumentsUav.Get()};work->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        work->CSSetShader(shader.Get(),nullptr,0);work->Dispatch(1,1,1);
        ID3D11UnorderedAccessView* empty[2]{};work->CSSetUnorderedAccessViews(0,2,empty,nullptr);return true;
    }
    // Publish only after the caller executes the recorded command list. Failed
    // recording and diagnostic bypasses invalidate the gate before it is reused.
    void Commit(){committed=std::move(pending);lastGroups=pendingGroups;lastNative=pendingNative;valid=true;}
};
}