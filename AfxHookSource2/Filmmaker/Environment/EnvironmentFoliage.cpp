#define NOMINMAX
#include "EnvironmentFoliage.h"
#include "EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <cmath>
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID InputsTag={0xbd60a6a1,0x10a2,0x4ca2,{0xba,0xaf,0x98,0xc8,0x1b,0xd5,0x26,0x36}};
constexpr GUID CpuWindTag={0x552a885a,0xa9d2,0x40ca,{0x93,0x40,0x27,0x99,0x84,0xbd,0x6a,0x1e}};
struct CpuWind {float values[8]{};const void* mapped=nullptr;bool valid=false;};
bool ConstantBuffer(ID3D11Resource* resource) {
    if(!resource)return false;D3D11_RESOURCE_DIMENSION type;resource->GetType(&type);
    if(type!=D3D11_RESOURCE_DIMENSION_BUFFER)return false;
    D3D11_BUFFER_DESC d{};static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    return (d.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&d.ByteWidth>=304&&d.ByteWidth<=65536;
}
bool CpuSnapshot(ID3D11Resource* resource,CpuWind& data) {
    UINT bytes=sizeof(data);return resource&&SUCCEEDED(resource->GetPrivateData(CpuWindTag,&bytes,&data))&&bytes==sizeof(data);
}
struct Slots { UINT time=~0u,wind=~0u,noise=~0u; };
std::atomic<bool> requested{false},nextRequested{false},complete{false};
std::mutex guard;
ComPtr<ID3D11Buffer> clockBuffer,windBuffer;
Vec3 cachedWind{};float cachedStrength=0;bool cachedWindValid=false;
ComPtr<ID3D11ShaderResourceView> noise;
bool gotTime=false,gotWind=false;
unsigned long long generation=0;
std::atomic<unsigned> registered{0},draws{0},tagged{0},deferred{0};
bool Has(const void *code,size_t bytes,const char *word) {
    const char *first=static_cast<const char*>(code);
    return std::search(first,first+bytes,word,word+strlen(word))!=first+bytes;
}
bool CopyBuffer(ID3D11DeviceContext *ctx,UINT slot,UINT minimum,ComPtr<ID3D11Buffer>& target) {
    if(slot>=D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)return false;
    ComPtr<ID3D11Buffer> source;ctx->VSGetConstantBuffers(slot,1,&source);
    if(!source)return false;
    D3D11_BUFFER_DESC d{};source->GetDesc(&d);
    if(d.ByteWidth<minimum||d.ByteWidth>65536)return false;
    D3D11_BUFFER_DESC old{};if(target)target->GetDesc(&old);
    if(!target||old.ByteWidth!=d.ByteWidth){
        target.Reset();d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=d.StructureByteStride=0;
        d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateBuffer(&d,nullptr,&target)))return false;
    }
    // GPU-to-GPU snapshot at the native draw: no stall/readback, no engine CB writes.
    ctx->CopyResource(target.Get(),source.Get());
    CpuWind cpu;
    if(CpuSnapshot(source.Get(),cpu)&&cpu.valid&&!cpu.mapped){
        cpu.mapped=nullptr;target->SetPrivateData(CpuWindTag,sizeof(cpu),&cpu);
    }else target->SetPrivateData(CpuWindTag,0,nullptr);
    return true;
}
}
void NoteFoliageConstantMap(ID3D11Resource* resource,D3D11_MAP type,const void* data) {
    if(!data||type==D3D11_MAP_READ||!ConstantBuffer(resource))return;
    CpuWind cpu;cpu.mapped=data;resource->SetPrivateData(CpuWindTag,sizeof(cpu),&cpu);
}
void NoteFoliageConstantUnmap(ID3D11Resource* resource) {
    CpuWind cpu;if(!CpuSnapshot(resource,cpu)||!cpu.mapped)return;
    std::memcpy(cpu.values,static_cast<const char*>(cpu.mapped)+272,sizeof(cpu.values));
    cpu.mapped=nullptr;cpu.valid=true;resource->SetPrivateData(CpuWindTag,sizeof(cpu),&cpu);
}
void NoteFoliageConstantUpdate(ID3D11Resource* resource,const D3D11_BOX* box,const void* data) {
    if(!data||!ConstantBuffer(resource))return;
    CpuWind cpu;
    const UINT first=box?box->left:0,last=box?box->right:304;
    // A partial upload cannot establish all eight current values. Fail open for
    // culling rather than retain a previous frame's potentially smaller bound.
    if(first<=272&&last>=304){
        std::memcpy(cpu.values,static_cast<const char*>(data)+(272-first),sizeof(cpu.values));cpu.valid=true;
    }
    resource->SetPrivateData(CpuWindTag,sizeof(cpu),&cpu);
}
void InvalidateFoliageConstantCopy(ID3D11Resource* resource) {
    CpuWind cpu;if(CpuSnapshot(resource,cpu))resource->SetPrivateData(CpuWindTag,0,nullptr);
}
void NoteFoliageShader(ID3D11VertexShader *shader,const void *code,size_t bytes) {
    if(!shader||!code||!Has(code,bytes,"g_flGameTime"))return;
    static HMODULE compiler=EnvironmentShaderCompiler();
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    auto reflect=compiler?reinterpret_cast<Reflect>(GetProcAddress(compiler,"D3DReflect")):nullptr;
    ComPtr<ID3D11ShaderReflection> reflection;
    if(!reflect||FAILED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&reflection)))return;
    auto cb=[&](const char *buffer,const char *variable,UINT offset,UINT bytes)->UINT {
        D3D11_SHADER_INPUT_BIND_DESC binding{};D3D11_SHADER_VARIABLE_DESC value{};
        auto constant=reflection->GetConstantBufferByName(buffer);
        if(!constant||FAILED(constant->GetVariableByName(variable)->GetDesc(&value))||
           value.StartOffset!=offset||value.Size!=bytes||
           FAILED(reflection->GetResourceBindingDescByName(buffer,&binding))||binding.Type!=D3D_SIT_CBUFFER)return ~0u;
        return binding.BindPoint;
    };
    Slots slots;
    slots.time=cb("PerViewConstantBuffer_t","g_flGameTime",300,4);
    slots.wind=cb("PerViewConstantBufferCsgo_t","g_vWindDirection",272,16);
    // Names/layout, not a guessed shader pointer or slot from an unrelated material.
    if(slots.wind!=cb("PerViewConstantBufferCsgo_t","g_vWindStrengthFreqMulHighStrength",288,16))slots.wind=~0u;
    D3D11_SHADER_INPUT_BIND_DESC resource{};
    if(Has(code,bytes,"g_flSwayAmount")&&Has(code,bytes,"g_flFlutterAmount")&&
       SUCCEEDED(reflection->GetResourceBindingDescByName("g_tNoiseMap",&resource))&&
       resource.Type==D3D_SIT_TEXTURE&&resource.Dimension==D3D_SRV_DIMENSION_TEXTURE3D)slots.noise=resource.BindPoint;
    if(slots.time!=~0u){shader->SetPrivateData(InputsTag,sizeof(slots),&slots);++registered;}
}
void ObserveFoliageInputs(ID3D11DeviceContext *ctx) {
    if(!requested.load(std::memory_order_relaxed)||complete.load(std::memory_order_relaxed)||!ctx)return;
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++deferred;return;}++draws;
    ComPtr<ID3D11VertexShader> shader;ctx->VSGetShader(&shader,nullptr,nullptr);
    Slots slots;UINT size=sizeof(slots);
    if(!shader||FAILED(shader->GetPrivateData(InputsTag,&size,&slots))||size!=sizeof(slots))return;
    ++tagged;
    std::lock_guard<std::mutex> lock(guard);
    if(!gotTime&&CopyBuffer(ctx,slots.time,304,clockBuffer)){gotTime=true;++generation;}
    if(!gotWind&&CopyBuffer(ctx,slots.wind,304,windBuffer))gotWind=true;
    if(!noise&&slots.noise<128){
        ComPtr<ID3D11ShaderResourceView> candidate;ctx->VSGetShaderResources(slots.noise,1,&candidate);
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};if(candidate)candidate->GetDesc(&d);
        if(candidate&&d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE3D){
            noise=candidate;
            MvmDebugLog_LinefAlways("environment.foliage","native inputs verified timeOffset=300 windOffset=272 strengthOffset=288 noise3D=1 noiseSlot=%u",slots.noise);
        }
    }
    complete.store(gotTime&&gotWind&&noise,std::memory_order_relaxed);
}
void NextFoliageFrame(){
    const bool needed=nextRequested.exchange(false,std::memory_order_relaxed);
    requested.store(needed,std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(guard);gotTime=gotWind=false;complete=false;
}
void RequestFoliageInputs(bool value){
    // Several shadow tiers request inputs in one frame. An inactive later tier
    // must not cancel an enabled earlier tier or freeze its animation clock.
    if(value){requested=true;nextRequested=true;}
}
void ResetFoliageInputs(){
    requested=nextRequested=false;std::lock_guard<std::mutex> lock(guard);
    clockBuffer.Reset();windBuffer.Reset();noise.Reset();gotTime=gotWind=false;complete=false;generation=0;
    cachedWindValid=false;
}
bool BindFoliageInputs(ID3D11DeviceContext *ctx,unsigned long long &serial){
    nextRequested=true; // An active consumer also needs a fresh snapshot next frame.
    std::lock_guard<std::mutex> lock(guard);
    if(MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>=1000){last=now;
            MvmDebugLog_LinefAlways("environment.foliage.inputs","registered=%u draws=%u deferred=%u tagged=%u time=%d wind=%d noise=%d",registered.load(),draws.load(),deferred.load(),tagged.load(),gotTime,gotWind,bool(noise));}
    }
    if(!gotTime||!gotWind||!windBuffer||!noise)return false;
    auto clock=clockBuffer.Get(),wind=windBuffer.Get();auto texture=noise.Get();
    ctx->VSSetConstantBuffers(5,1,&clock);ctx->VSSetConstantBuffers(6,1,&wind);
    ctx->VSSetShaderResources(12,1,&texture);serial=generation;return true;
}
bool ReadFoliageWind(ID3D11DeviceContext* ctx,Vec3& wind,float& strength){
    if(!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    std::lock_guard<std::mutex> lock(guard);
    if(!gotTime||!gotWind||!windBuffer||!noise)return false;
    {
        cachedWindValid=false;
        D3D11_SHADER_RESOURCE_VIEW_DESC noiseDesc{};noise->GetDesc(&noiseDesc);
        const bool normalized=noiseDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||noiseDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
            noiseDesc.Format==DXGI_FORMAT_R16G16B16A16_UNORM||noiseDesc.Format==DXGI_FORMAT_R8G8_UNORM||
            noiseDesc.Format==DXGI_FORMAT_BC1_UNORM||noiseDesc.Format==DXGI_FORMAT_BC3_UNORM||
            noiseDesc.Format==DXGI_FORMAT_BC7_UNORM||noiseDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;
        if(!normalized)return false;
        CpuWind cpu;
        if(CpuSnapshot(windBuffer.Get(),cpu)&&cpu.valid&&!cpu.mapped){
            cachedWind={cpu.values[0],cpu.values[1],cpu.values[2]};cachedStrength=cpu.values[4];
            cachedWindValid=std::isfinite(cachedWind.x)&&std::isfinite(cachedWind.y)&&std::isfinite(cachedWind.z)&&std::isfinite(cachedStrength);
        }
        if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
            MvmDebugLog_LinefAlways("environment.foliage.wind","valid=%u noiseFormat=%u wind=(%.4f %.4f %.4f) strength=%.4f cpuUpload=1 argumentReadback=0",unsigned(cachedWindValid),unsigned(noiseDesc.Format),cachedWind.x,cachedWind.y,cachedWind.z,cachedStrength);}}
    }
    wind=cachedWind;strength=cachedStrength;return cachedWindValid;
}
}
