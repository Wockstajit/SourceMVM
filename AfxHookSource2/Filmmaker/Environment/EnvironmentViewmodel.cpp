#define NOMINMAX
#include "EnvironmentViewmodel.h"
#include "EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cmath>
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID ViewTag={0xd488e68a,0xca19,0x493b,{0x90,0x0c,0x8c,0x8b,0xdc,0x39,0x53,0x13}};
// Only Observe/Bind/Next touch snapshots, on the immediate render thread. Shader
// creation workers attach immutable reflection metadata to the shader itself.
ComPtr<ID3D11Buffer> projection;
ComPtr<ID3D11Texture2D> beforeDepth,sourceDepth;
ComPtr<ID3D11ShaderResourceView> beforeSrv;
bool wanted=false,captured=false;
struct Probe {
    ComPtr<ID3D11DeviceContext> commands;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> output,readback;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Query> done;
    bool pending=false;UINT samples=0;ULONGLONG last=0;float camera[3]{};
} probe;
}
void NoteViewmodelShader(ID3D11VertexShader *shader,const void *code,size_t bytes){
    const char *name="g_matProjectionToWorld",*first=static_cast<const char*>(code);
    if(!shader||!code||std::search(first,first+bytes,name,name+strlen(name))==first+bytes)return;
    static HMODULE library=EnvironmentShaderCompiler();
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    auto reflect=library?reinterpret_cast<Reflect>(GetProcAddress(library,"D3DReflect")):nullptr;
    ComPtr<ID3D11ShaderReflection> r;
    if(!reflect||FAILED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&r)))return;
    const char *buffer="PerViewConstantBuffer_t";
    auto cb=r->GetConstantBufferByName(buffer);D3D11_SHADER_INPUT_BIND_DESC binding{};
    if(!cb||FAILED(r->GetResourceBindingDescByName(buffer,&binding))||binding.Type!=D3D_SIT_CBUFFER)return;
    const char *fields[]={"g_matProjectionToWorld","g_vWorldToCameraOffset"};
    const UINT offsets[]={64,384},sizes[]={64,16};
    for(int i=0;i<2;++i){D3D11_SHADER_VARIABLE_DESC v{};
        if(FAILED(cb->GetVariableByName(fields[i])->GetDesc(&v))||v.StartOffset!=offsets[i]||v.Size!=sizes[i])return;}
    shader->SetPrivateData(ViewTag,sizeof(binding.BindPoint),&binding.BindPoint);
}
void ObserveViewmodel(ID3D11DeviceContext *ctx){
    if(!wanted||captured||!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    // Measured native viewmodel pass, not a distance heuristic on scene pixels.
    if(count!=1||vp.Width<256||vp.Height<256||vp.MinDepth!=0||std::abs(vp.MaxDepth-.1f)>1e-6)return;
    ComPtr<ID3D11VertexShader> shader;ctx->VSGetShader(&shader,nullptr,nullptr);
    UINT slot=~0u,size=sizeof(slot);
    if(!shader||FAILED(shader->GetPrivateData(ViewTag,&size,&slot))||slot>=14)return;
    ComPtr<ID3D11Buffer> native;ctx->VSGetConstantBuffers(slot,1,&native);
    ComPtr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);
    if(!native||!dsv)return;
    ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.Width!=UINT(vp.Width)||td.Height!=UINT(vp.Height)||td.ArraySize!=1||td.MipLevels!=1)return;
    DXGI_FORMAT view=td.Format==DXGI_FORMAT_R32_TYPELESS||td.Format==DXGI_FORMAT_D32_FLOAT?DXGI_FORMAT_R32_FLOAT:
        td.Format==DXGI_FORMAT_R24G8_TYPELESS||td.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:DXGI_FORMAT_UNKNOWN;
    if(view==DXGI_FORMAT_UNKNOWN)return;
    // A depth-only typed resource can be copied into its typeless family for SRV reads.
    td.Format=view==DXGI_FORMAT_R32_FLOAT?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;
    D3D11_BUFFER_DESC bd{};native->GetDesc(&bd);if(bd.ByteWidth<400||bd.ByteWidth>65536)return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    D3D11_TEXTURE2D_DESC previous{};if(beforeDepth)beforeDepth->GetDesc(&previous);
    if(!beforeDepth||td.Width!=previous.Width||td.Height!=previous.Height||td.SampleDesc.Count!=previous.SampleDesc.Count||td.SampleDesc.Quality!=previous.SampleDesc.Quality||td.Format!=previous.Format){
        beforeDepth.Reset();beforeSrv.Reset();td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_DEFAULT;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&beforeDepth)))return;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=view;
        sd.ViewDimension=td.SampleDesc.Count>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;
        if(td.SampleDesc.Count==1)sd.Texture2D.MipLevels=1;
        if(FAILED(device->CreateShaderResourceView(beforeDepth.Get(),&sd,&beforeSrv))){beforeDepth.Reset();return;}
    }
    D3D11_BUFFER_DESC old{};if(projection)projection->GetDesc(&old);
    if(!projection||old.ByteWidth!=bd.ByteWidth){
        projection.Reset();bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&bd,nullptr,&projection)))return;
    }
    ctx->CopyResource(beforeDepth.Get(),texture.Get());ctx->CopyResource(projection.Get(),native.Get());
    sourceDepth=texture;captured=true;
    if(MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>3000){last=now;
            MvmDebugLog_LinefAlways("environment.viewmodel","captured=1 size=%ux%u samples=%u depthMax=0.1 inverseOffset=64 cameraOffset=384 slot=%u",td.Width,td.Height,td.SampleDesc.Count,slot);}}
}
void NextViewmodelFrame(){captured=false;}
void ResetViewmodel(){wanted=captured=false;projection.Reset();beforeDepth.Reset();beforeSrv.Reset();sourceDepth.Reset();probe=Probe{};}
bool BindViewmodel(ID3D11DeviceContext *ctx,ID3D11Texture2D *depth){
    wanted=true;bool valid=captured&&depth==sourceDepth.Get();
    auto cb=valid?projection.Get():nullptr;auto srv=valid?beforeSrv.Get():nullptr;
    ctx->PSSetConstantBuffers(7,1,&cb);ctx->PSSetShaderResources(17,1,&srv);return valid;
}
// Sparse, asynchronous read-only diagnostics. A private command list restores all engine
// bindings; neither the main render target nor any native constants are modified.
void ProbeViewmodel(ID3D11DeviceContext *immediate,ID3D11Texture2D *depth,float x,float y,float z){
    if(!MvmDebugLog_Active()||!immediate)return;
    if(probe.pending){
        BOOL done=FALSE;if(immediate->GetData(probe.done.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||!done)return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(immediate->Map(probe.readback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))){
            auto data=static_cast<float*>(mapped.pData);unsigned hand=0,unchanged=0;float maxDistance=0,total=0;
            for(unsigned i=1;i<128*96;++i){auto p=data+4*i;if(p[3]==2)++unchanged;if(p[3]!=1)continue;++hand;
                float a=p[0]-data[0],b=p[1]-data[1],c=p[2]-data[2];float distance=std::sqrt(a*a+b*b+c*c);
                total+=distance;maxDistance=std::max(maxDistance,distance);}
            immediate->Unmap(probe.readback.Get(),0);
            MvmDebugLog_LinefAlways("environment.viewmodel.probe","grid=128x96 hand=%u nearUnchanged=%u anchoredAverageDistance=%.3f anchoredMaximumDistance=%.3f",hand,unchanged,hand?total/hand:0,maxDistance);
            probe.pending=false;
        }
        return;
    }
    auto now=GetTickCount64();if(now-probe.last<3000||!captured||depth!=sourceDepth.Get())return;probe.last=now;
    ComPtr<ID3D11Device> device;immediate->GetDevice(&device);D3D11_TEXTURE2D_DESC td{};depth->GetDesc(&td);
    if(!probe.shader||probe.samples!=td.SampleDesc.Count){
        probe=Probe{};probe.last=now;probe.samples=td.SampleDesc.Count;
        const char *code=R"(
            cbuffer Native:register(b0){column_major float4x4 inv:packoffset(c4);float4 offset:packoffset(c24);}
            #if MS
            Texture2DMS<float> current:register(t0),before:register(t1);
            #else
            Texture2D<float> current:register(t0),before:register(t1);
            #endif
            RWStructuredBuffer<float4> result:register(u0);
            [numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
                uint w,h;
                #if MS
                uint samples;current.GetDimensions(w,h,samples);
                #else
                current.GetDimensions(w,h);
                #endif
                uint2 p=min(uint2((id.xy+.5)*float2(w,h)/float2(128,96)),uint2(w-1,h-1));
                #if MS
                float a=current.Load(p,0),b=before.Load(p,0);
                #else
                float a=current.Load(int3(p,0)),b=before.Load(int3(p,0));
                #endif
                float4 world=mul(inv,float4((p+.5)/float2(w,h)*float2(2,-2)+float2(-1,1),a*10,1));
                if(id.x==0&&id.y==0){float4 eye=mul(inv,float4(0,0,1,0));result[0]=float4(eye.xyz/eye.w-offset.xyz,0);return;}
                result[id.y*128+id.x]=float4(world.xyz/world.w-offset.xyz,a>.100001?0:a<b-.000001?1:2);
            })";
        ComPtr<ID3DBlob> bytes,error;D3D_SHADER_MACRO macros[]={{"MS",td.SampleDesc.Count>1?"1":"0"},{nullptr,nullptr}};
        if(FAILED(CompileEnvironmentShader(code,strlen(code),"ViewmodelProbe",macros,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&bytes,&error))||
           FAILED(device->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&probe.shader))||
           FAILED(device->CreateDeferredContext(0,&probe.commands)))return;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=128*96*16;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
        if(FAILED(device->CreateBuffer(&bd,nullptr,&probe.output))||FAILED(device->CreateUnorderedAccessView(probe.output.Get(),nullptr,&probe.uav)))return;
        bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
        if(FAILED(device->CreateBuffer(&bd,nullptr,&probe.readback))||FAILED(device->CreateQuery(&q,&probe.done)))return;
    }
    if(!probe.commands||!probe.output||!probe.uav||!probe.readback||!probe.done)return;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};beforeSrv->GetDesc(&sd);ComPtr<ID3D11ShaderResourceView> source;
    if(FAILED(device->CreateShaderResourceView(depth,&sd,&source)))return;
    auto ctx=probe.commands.Get();ctx->ClearState();ctx->CSSetShader(probe.shader.Get(),nullptr,0);
    auto cb=projection.Get();ctx->CSSetConstantBuffers(0,1,&cb);ID3D11ShaderResourceView *views[]={source.Get(),beforeSrv.Get()};ctx->CSSetShaderResources(0,2,views);
    auto uav=probe.uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);ctx->Dispatch(16,12,1);ctx->CopyResource(probe.readback.Get(),probe.output.Get());ctx->End(probe.done.Get());
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return;immediate->ExecuteCommandList(list.Get(),TRUE);
    probe.pending=true;probe.camera[0]=x;probe.camera[1]=y;probe.camera[2]=z;
}
}
