#define NOMINMAX
#include "EnvironmentParticleShadows.h"
#include "OwnedInterface.h"
#include "EnvironmentParticleShadowShaders.h"
#include "EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID CaptureTag={0x707e0cd2,0xd8c5,0x4cd4,{0x97,0x63,0xcd,0xc2,0xee,0x15,0x12,0x39}};
constexpr GUID SlotTag={0xe47b4628,0xdfe5,0x4bb4,{0xb7,0xe9,0xe7,0x58,0x76,0x57,0x14,0x42}};
constexpr UINT Capacity=128*1024,Stride=36,MaxBatches=256;
struct Batch {ComPtr<ID3D11ShaderResourceView> sheet;ComPtr<ID3D11SamplerState> samplers[2];ComPtr<ID3D11Buffer> materials[2];UINT materialIndex=0,first=0,count=0;};
struct Frame {ComPtr<ID3D11Buffer> vertices;std::array<Batch,MaxBatches> batches;UINT count=0,batchCount=0;};
struct State {
    ComPtr<ID3D11Device> device;
    Frame current,published;
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> transform,previousConstants;
    ComPtr<ID3D11Buffer> diagnostic;bool diagnosticPending=false;ULONGLONG diagnosticTime=0;
    bool active=false;
} state;
std::atomic<bool> enabled{true};
bool Compile(const std::string& source,const char* entry,const char* profile,ComPtr<ID3DBlob>& code){
    ComPtr<ID3DBlob> error;
    const auto hr=CompileEnvironmentShader(source.data(),source.size(),"ParticleShadow",nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
    if(FAILED(hr))MvmDebugLog_LinefAlways("environment.particle.shadow","compile failed: %s",error?static_cast<const char*>(error->GetBufferPointer()):"compiler unavailable");
    return SUCCEEDED(hr);
}
Frame& Available(){return state.current.count?state.current:state.published;}
}

void NoteParticleShadowShader(ID3D11VertexShader* shader,const void* code,size_t bytes){
    if(!shader||!code)return;
    const char* first=static_cast<const char*>(code),*marker="SpritecardVertexControls_t";
    if(std::search(first,first+bytes,marker,marker+strlen(marker))==first+bytes)return;
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    auto library=EnvironmentShaderCompiler();auto reflect=library?reinterpret_cast<Reflect>(GetProcAddress(library,"D3DReflect")):nullptr;
    ComPtr<ID3D11ShaderReflection> r;D3D11_SHADER_DESC desc{};D3D11_SHADER_INPUT_BIND_DESC binding{};
    if(!reflect||FAILED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&r))||FAILED(r->GetDesc(&desc))||
       FAILED(r->GetResourceBindingDescByName("PerViewConstantBuffer_t",&binding))||binding.Type!=D3D_SIT_CBUFFER||binding.BindPoint>=14)return;
    D3D11_SHADER_VARIABLE_DESC inverse{};
    if(FAILED(r->GetConstantBufferByName("PerViewConstantBuffer_t")->GetVariableByName("g_matProjectionToWorld")->GetDesc(&inverse))||inverse.StartOffset!=64||inverse.Size!=64)return;
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> signature;
    for(UINT i=0;i<desc.OutputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC d{};if(FAILED(r->GetOutputParameterDesc(i,&d)))return;signature.push_back(d);}
    std::stable_sort(signature.begin(),signature.end(),[](const auto& a,const auto& b){return a.Register!=b.Register?a.Register<b.Register:(a.Mask&-int(a.Mask))<(b.Mask&-int(b.Mask));});
    std::string input="struct Input {";int position=-1,world=-1,uv=-1,alpha=-1,blend=-1;
    for(UINT i=0;i<signature.size();++i){const auto& d=signature[i];
        const UINT lane=d.Mask&-int(d.Mask),lanes=(d.Mask&1u)+((d.Mask>>1)&1u)+((d.Mask>>2)&1u)+((d.Mask>>3)&1u);
        if(!d.Mask||(d.Mask/lane+1)!=(1u<<lanes)||d.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32)return;
        if(d.SystemValueType==D3D_NAME_POSITION&&d.Mask==15)position=i;
        else if(d.SystemValueType!=D3D_NAME_UNDEFINED&&d.SystemValueType!=D3D_NAME_CLIP_DISTANCE)return;
        if(!strcmp(d.SemanticName,"TEXCOORD")&&d.SemanticIndex==0&&d.Mask==15)uv=i;
        if(!strcmp(d.SemanticName,"TEXCOORD")&&d.SemanticIndex==2&&d.Mask==7)world=i;
        if(!strcmp(d.SemanticName,"COLOR")&&d.SemanticIndex==0&&d.Mask==15)alpha=i;
        if(!strcmp(d.SemanticName,"TEXCOORD")&&d.SemanticIndex==4&&d.Mask==15)blend=i;
        input+="float"+std::to_string(lanes)+" v"+std::to_string(i)+":"+d.SemanticName+std::to_string(d.SemanticIndex)+";";
    }
    if(position<0||world<0||uv<0||alpha<0||blend<0)return;
    input+="};";
    const auto value=[](int i){return "input[k].v"+std::to_string(i);};
    // Spritecard adjusts clip depth to avoid near-camera clipping. Inverting
    // SV_Position would move the smoke caster; TEXCOORD2 is its native world pose.
    std::string source=input+
        "struct Output{float3 p:POSITION;float4 uv:TEXCOORD0;float2 fade:TEXCOORD1;};"
        "[maxvertexcount(3)]void Main(triangle Input input[3],inout TriangleStream<Output> output){"
        "[unroll]for(uint k=0;k<3;++k){Output o;o.p="+value(world)+";o.uv="+value(uv)+";o.fade=float2("+value(alpha)+".w,"+value(blend)+".x);output.Append(o);}output.RestartStrip();}";
    // Shader variants sharing the same output ABI share compilation, outside draw submission.
    // The cache keeps plain bytecode, not compiler blobs: a cached ID3DBlob read back as a garbage
    // object during startup shader creation (crash report 2026-10-13, AddRef on 0xffff...).
    static std::map<std::string,std::vector<char>> cache;
    static std::mutex cacheMutex;
    std::vector<char> compiled;
    {
        // Native shader creation runs concurrently on engine compile workers.
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found=cache.find(source);
        if(found!=cache.end())compiled=found->second;
        else {
            ComPtr<ID3DBlob> blob;
            if(cache.size()>=32||!Compile(source,"Main","gs_5_0",blob)||!blob||!blob->GetBufferSize())return;
            const char* bytecode=static_cast<const char*>(blob->GetBufferPointer());
            compiled.assign(bytecode,bytecode+blob->GetBufferSize());
            cache.emplace(source,compiled);
        }
    }
    ComPtr<ID3D11ShaderReflection> check;
    if(FAILED(reflect(compiled.data(),compiled.size(),__uuidof(ID3D11ShaderReflection),&check)))return;
    for(UINT i=0;i<signature.size();++i){D3D11_SIGNATURE_PARAMETER_DESC d{};
        if(FAILED(check->GetInputParameterDesc(i,&d))||d.Register!=signature[i].Register||d.Mask!=signature[i].Mask||
           d.SemanticIndex!=signature[i].SemanticIndex||strcmp(d.SemanticName,signature[i].SemanticName)){
            MvmDebugLog_LinefAlways("environment.particle.shadow.shader","signature mismatch at %u native=%s%u r%u mask%u capture=%s%u r%u mask%u",i,signature[i].SemanticName,signature[i].SemanticIndex,signature[i].Register,signature[i].Mask,d.SemanticName?d.SemanticName:"?",d.SemanticIndex,d.Register,d.Mask);return;}}
    ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11GeometryShader> gs;
    const D3D11_SO_DECLARATION_ENTRY entries[]={{0,"POSITION",0,0,3,0},{0,"TEXCOORD",0,0,4,0},{0,"TEXCOORD",1,0,2,0}};const UINT stride=Stride;
    const auto hr=device->CreateGeometryShaderWithStreamOutput(compiled.data(),compiled.size(),entries,3,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&gs);
    if(SUCCEEDED(hr)&&SUCCEEDED(::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CaptureTag,gs.Get())))shader->SetPrivateData(SlotTag,sizeof(binding.BindPoint),&binding.BindPoint);
    if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("environment.particle.shadow.shader","capture=%u hr=%08x outputs=%zu viewSlot=%u",unsigned(!!gs),unsigned(hr),signature.size(),binding.BindPoint);
}
void EndParticleShadows(){state=State{};}
void SetParticleShadowsEnabled(bool value){enabled.store(value);}
bool ParticleShadowsEnabled(){return enabled.load();}
void BeginParticleShadows(ID3D11Device* device){
    EndParticleShadows();if(!device)return;state.device=device;
    D3D11_BUFFER_DESC b{};b.ByteWidth=Capacity*Stride;b.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.current.vertices))||FAILED(device->CreateBuffer(&b,nullptr,&state.published.vertices))){MvmDebugLog_LinefAlways("environment.particle.shadow","vertex buffers failed");EndParticleShadows();return;}
    b.ByteWidth=64;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.MiscFlags=0;
    ComPtr<ID3DBlob> vs,ps;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.transform))||!Compile(ParticleShadowShader,"VS","vs_5_0",vs)||!Compile(ParticleShadowShader,"PS","ps_5_0",ps)||
       FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.vertex))||FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&state.pixel))){MvmDebugLog_LinefAlways("environment.particle.shadow","draw initialization failed");EndParticleShadows();return;}
    const D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,28,D3D11_INPUT_PER_VERTEX_DATA,0}};
    const auto layoutHr=device->CreateInputLayout(elements,3,vs->GetBufferPointer(),vs->GetBufferSize(),&state.layout);
    if(FAILED(layoutHr)){MvmDebugLog_LinefAlways("environment.particle.shadow","layout failed %08x",unsigned(layoutHr));EndParticleShadows();}
    if(!state.device)return;
    b.ByteWidth=512;b.BindFlags=0;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    device->CreateBuffer(&b,nullptr,&state.diagnostic);
    b.Usage=D3D11_USAGE_DEFAULT;b.CPUAccessFlags=0;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    for(UINT i=0;i<2;++i){b.ByteWidth=i?512:320;
        for(auto* frame:{&state.current,&state.published})for(auto& batch:frame->batches)
            if(FAILED(device->CreateBuffer(&b,nullptr,&batch.materials[i]))){MvmDebugLog_LinefAlways("environment.particle.shadow","material buffer failed");EndParticleShadows();return;}}
}
void NextParticleShadowFrame(){
    if(!state.device)return;
    // Swap complete frames, including texture ownership. Publishing an empty frame
    // clears finished smoke; an early world draw cannot reuse it indefinitely.
    std::swap(state.current,state.published);
    if(MvmDebugLog_Active()&&state.published.count){static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>250){last=now;MvmDebugLog_LinefAlways("environment.particle.shadow.frame","vertices=%u batches=%u published=1",state.published.count,state.published.batchCount);}}
    for(UINT i=0;i<state.current.batchCount;++i){state.current.batches[i].sheet.Reset();for(auto& sampler:state.current.batches[i].samplers)sampler.Reset();}
    state.current.count=state.current.batchCount=0;
}
bool BeginParticleShadowDraw(ID3D11DeviceContext* ctx,UINT vertices,UINT instances){
    if(!enabled.load()||!ctx||!state.current.vertices||state.active||!vertices||!instances||vertices%3||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    D3D11_PRIMITIVE_TOPOLOGY topology;ctx->IAGetPrimitiveTopology(&topology);if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return false;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);if(!vs)return false;
    UINT slot=~0u,bytes=sizeof(slot);if(FAILED(vs->GetPrivateData(SlotTag,&bytes,&slot))||slot>=14)return false;
    char name[128]{};bytes=sizeof(name)-1;
    if(FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,name))||strcmp(name,"spritecard.vfx_vs"))return false;
    D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);
    if(MvmDebugLog_Active()){static std::map<UINT,ULONGLONG> seen;const UINT key=UINT(vp.MaxDepth*10000)+UINT(vp.Width)*10000;const auto now=GetTickCount64();if(now-seen[key]>1000){seen[key]=now;MvmDebugLog_LinefAlways("environment.particle.shadow.viewport","viewport=%.0fx%.0f depth=%.4f..%.4f",vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth);}}
    // Both main-world and first-person sprite passes retain a world-space pose.
    if(n!=1||vp.Width<128||vp.Height<128||vp.MinDepth!=0||
       (std::abs(vp.MaxDepth-.95f)>1e-6f&&std::abs(vp.MaxDepth-.1f)>1e-6f))return false;
    ComPtr<ID3D11BlendState> blend;float factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);const auto& target=bd.RenderTarget[0];
    const bool opacityBlend=target.BlendEnable&&target.BlendOp==D3D11_BLEND_OP_ADD&&target.DestBlend==D3D11_BLEND_INV_SRC_ALPHA&&
        (target.SrcBlend==D3D11_BLEND_SRC_ALPHA||target.SrcBlend==D3D11_BLEND_ONE);
    if(MvmDebugLog_Active()){
        static std::map<UINT,ULONGLONG> last;auto now=GetTickCount64();const UINT key=UINT(target.SrcBlend)*100+UINT(target.DestBlend);if(now-last[key]>=1000){last[key]=now;
            MvmDebugLog_LinefAlways("environment.particle.shadow.draw","vertices=%u instances=%u viewport=%.0fx%.0f blend=%u src=%u dst=%u opacity=%u batches=%u",vertices,instances,vp.Width,vp.Height,unsigned(target.BlendEnable),unsigned(target.SrcBlend),unsigned(target.DestBlend),unsigned(opacityBlend),state.current.batchCount);}}
    if(!opacityBlend||bd.AlphaToCoverageEnable||uint64_t(vertices)*instances>Capacity-state.current.count||state.current.batchCount>=MaxBatches)return false;
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);if(predicate)return false;
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);if(gs||hs||ds)return false;
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool occupied=false;for(auto* p:so)if(p){occupied=true;p->Release();}if(occupied)return false;
    ComPtr<ID3D11Buffer> view,material;ctx->VSGetConstantBuffers(slot,1,&view);ctx->PSGetConstantBuffers(4,1,&material);
    D3D11_BUFFER_DESC vd{},md{};if(view)view->GetDesc(&vd);if(material)material->GetDesc(&md);
    if(MvmDebugLog_Active()){static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>1000){last=now;MvmDebugLog_LinefAlways("environment.particle.shadow.buffers","view=%u material=%u",vd.ByteWidth,md.ByteWidth);}}
    if(!view||vd.ByteWidth<128||!material||(md.ByteWidth!=320&&md.ByteWidth!=512))return false;
    if(MvmDebugLog_Active()&&state.diagnostic){
        if(state.diagnosticPending){D3D11_MAPPED_SUBRESOURCE mapped{};if(SUCCEEDED(ctx->Map(state.diagnostic.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))){
            const auto* values=static_cast<const float*>(mapped.pData);const auto* words=static_cast<const UINT*>(mapped.pData);
            MvmDebugLog_LinefAlways("environment.particle.shadow.material","flags=%08x,%08x,%08x blend=%.4f alphaScale=%.4f alphaBias=%.4f textureScale=%.4f ramps=%.4f,%.4f motion=%.0f lighting=%.4f,%.4f",words[76],words[77],words[78],values[7],values[19],values[32],values[33],values[2],values[3],values[14],values[0],values[1]);
            ctx->Unmap(state.diagnostic.Get(),0);state.diagnosticPending=false;}}
        const auto now=GetTickCount64();if(!state.diagnosticPending&&now-state.diagnosticTime>1000){D3D11_BOX box{0,0,0,md.ByteWidth,1,1};ctx->CopySubresourceRegion(state.diagnostic.Get(),0,0,0,0,material.Get(),0,&box);state.diagnosticTime=now;state.diagnosticPending=true;}
    }
    auto& batch=state.current.batches[state.current.batchCount];
    ctx->PSGetShaderResources(8,1,&batch.sheet);for(UINT i=0;i<2;++i)ctx->PSGetSamplers(i,1,&batch.samplers[i]);
    D3D11_SHADER_RESOURCE_VIEW_DESC texture{};if(batch.sheet)batch.sheet->GetDesc(&texture);
    if(!batch.sheet||!batch.samplers[0]||texture.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)return false;
    batch.materialIndex=md.ByteWidth==512?1:0;if(!batch.materials[batch.materialIndex])return false;
    bytes=sizeof(ID3D11GeometryShader*);if(FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(vs),CaptureTag,&bytes,(void**)gs.GetAddressOf()))||!gs)return false;
    ctx->CopyResource(batch.materials[batch.materialIndex].Get(),material.Get());batch.first=state.current.count;batch.count=vertices*instances;
    ctx->GSGetConstantBuffers(0,1,&state.previousConstants);auto buffer=view.Get();ctx->GSSetConstantBuffers(0,1,&buffer);ctx->GSSetShader(gs.Get(),nullptr,0);
    buffer=state.current.vertices.Get();UINT offset=batch.first*Stride;ctx->SOSetTargets(1,&buffer,&offset);
    state.current.count+=batch.count;++state.current.batchCount;state.active=true;return true;
}
bool EndParticleShadowDraw(ID3D11DeviceContext* ctx){
    if(!state.active)return false;
    ID3D11Buffer* none=nullptr;UINT offset=0;ctx->SOSetTargets(1,&none,&offset);ctx->GSSetShader(nullptr,nullptr,0);
    auto cb=state.previousConstants.Get();ctx->GSSetConstantBuffers(0,1,&cb);state.previousConstants.Reset();state.active=false;return true;
}
CasterVertexStream ParticleShadowStream(){auto& f=Available();return {f.vertices.Get(),enabled.load()?f.count:0,Stride};}
void DrawParticleShadows(ID3D11DeviceContext* ctx,const Matrix& worldToShadow){
    auto& f=Available();if(!enabled.load()||!ctx||!f.count||!state.vertex||!state.pixel)return;
    ComPtr<ID3D11ShaderResourceView> oldSheet;ComPtr<ID3D11SamplerState> oldSamplers[2];ComPtr<ID3D11Buffer> oldMaterial;
    ctx->PSGetShaderResources(8,1,&oldSheet);for(UINT i=0;i<2;++i)ctx->PSGetSamplers(i,1,&oldSamplers[i]);ctx->PSGetConstantBuffers(4,1,&oldMaterial);
    ctx->IASetInputLayout(state.layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);
    auto vb=f.vertices.Get();UINT stride=Stride,offset=0;ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    ctx->VSSetShader(state.vertex.Get(),nullptr,0);ctx->PSSetShader(state.pixel.Get(),nullptr,0);
    auto cb=state.transform.Get();ctx->UpdateSubresource(cb,0,nullptr,worldToShadow.data(),0,0);ctx->VSSetConstantBuffers(3,1,&cb);
    for(UINT i=0;i<f.batchCount;++i){auto& b=f.batches[i];auto sheet=b.sheet.Get();ID3D11SamplerState* samplers[2]={b.samplers[0].Get(),b.samplers[1].Get()};auto material=b.materials[b.materialIndex].Get();
        ctx->PSSetShaderResources(8,1,&sheet);ctx->PSSetSamplers(0,2,samplers);ctx->PSSetConstantBuffers(4,1,&material);ctx->Draw(b.count,b.first);}
    auto sheet=oldSheet.Get();ID3D11SamplerState* samplers[2]={oldSamplers[0].Get(),oldSamplers[1].Get()};cb=oldMaterial.Get();ctx->PSSetShaderResources(8,1,&sheet);ctx->PSSetSamplers(0,2,samplers);ctx->PSSetConstantBuffers(4,1,&cb);
}
}
