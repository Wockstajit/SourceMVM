#define NOMINMAX
#include "EnvironmentAnimatedCasters.h"
#include "OwnedInterface.h"
#include "EnvironmentParticleShadows.h"
#include "EnvironmentAnimatedCasterShaders.h"
#include "EnvironmentCasterBounds.h"
#include "EnvironmentCasterBuffer.h"
#include "EnvironmentIndirectCasterMath.h"
#include "EnvironmentIndirectCasterShaders.h"
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentShaderCoverageMath.h"
#include "../Debug/MvmDebugLog.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <set>
#include <vector>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID VertexAlphaTag={0x87db2d23,0x4fd3,0x4e35,{0x99,0x75,0x4f,0xb1,0x3f,0x37,0x28,0xb2}};
constexpr GUID CoverageCaptureTag={0x278a3378,0x517b,0x4c7b,{0x8b,0x97,0x94,0xe4,0x31,0x61,0x5f,0x63}};
constexpr GUID CoverageSemanticTag={0x00c8a69a,0xb339,0x4d9e,{0x89,0xa9,0x26,0x92,0x21,0x9e,0x6c,0x63}};
constexpr UINT CoverageCapacity=128*1024; // 2 MiB for native cable vertices plus their fade.
constexpr GUID ConstantAlphaTag={0x625cb01d,0x137f,0x4f23,{0x98,0x6d,0x22,0x4b,0x3e,0x14,0x85,0xc2}};
constexpr GUID PixelSignatureTag={0x26885323,0x0765,0x4437,{0x80,0x91,0x5a,0xfa,0x50,0xe9,0x69,0xee}};
constexpr GUID CutoutPixelTag={0x37530677,0x5946,0x48d8,{0xbd,0x94,0xed,0x57,0xab,0x5c,0x36,0xdd}};
constexpr GUID CutoutCaptureTag={0xac5c339b,0xb9eb,0x4f01,{0xa4,0x24,0xa3,0xf3,0x2b,0x23,0xa4,0x8d}};
constexpr UINT CutoutCapacity=512*1024;
struct CutoutBatch {
    ComPtr<ID3D11Buffer> constants[2];UINT constantBytes[2]{};
    ComPtr<ID3D11ShaderResourceView> opacity;
    ComPtr<ID3D11SamplerState> sampler;
    UINT first=0,count=0;
};
struct IndirectBatch {
    ComPtr<ID3D11Buffer> vertices,arguments,metadata;
    ComPtr<ID3D11ShaderResourceView> argumentView,metadataView;
    ComPtr<ID3D11UnorderedAccessView> metadataUav;
    CutoutBatch coverage;
    UINT capacity=0,stride=0,vertexBytes=0,argumentBytes=0;
    unsigned idleFrames=0;
    bool masked=false;
};
constexpr GUID OpaquePixelTag={0xe9b6654e,0x4a72,0x4d94,{0xa8,0xbb,0x3d,0x01,0x6a,0x2c,0xf5,0x70}};
constexpr GUID ViewTag={0xcf09e215,0x4724,0x4aae,{0x90,0xd3,0x77,0x68,0x29,0xa1,0xe9,0x3a}};
constexpr GUID ViewCaptureTag={0x54ffda73,0x19aa,0x4ff3,{0xbb,0x41,0x4c,0x53,0xaa,0x0d,0x43,0x09}};
constexpr GUID CaptureTag={0xf6cfd0c7,0x1f31,0x496f,{0x88,0x4e,0x78,0xa8,0x4c,0x5c,0xcf,0xaf}};
constexpr GUID SkinInputTag={0x1d4b7217,0x2f6a,0x4b0d,{0xad,0x11,0x58,0xec,0x72,0x89,0x31,0x22}};
constexpr GUID GlobalsScrubShaderTag={0x3e1c5f24,0x74fb,0x4a0a,{0x96,0xd0,0x24,0xc8,0x4e,0x91,0x0f,0x5b}};
constexpr GUID GlobalsScrubInfoTag={0x9b32ad57,0xc33c,0x4540,{0x82,0x7a,0x1c,0xf8,0x2e,0xa9,0x65,0xd4}};
constexpr UINT Capacity=2*1024*1024; // Capture limit; storage grows to the observed pose size.
// Preserve the handoff's bounded indirect foliage allocation. The GPU argument
// validator rejects oversized batches instead of reading incomplete SO data.
constexpr UINT IndirectCapacity=512*1024;
constexpr unsigned IndirectRetentionFrames=120;
constexpr size_t EvidenceSlots=32;
struct EvidenceSlot { std::string key; ULONGLONG last=0; };
struct GlobalsScrubInfo {
    UINT slot=~0u,bytes=0,zeroCount=0,zeroOffsets[4]{},oneOffset=~0u;
};
struct GlobalsScratch {
    ComPtr<ID3D11Buffer> clone,raw;
    ComPtr<ID3D11UnorderedAccessView> uav;
    UINT bytes=0;
};
struct State {
    CasterBoundsReader bounds;
    ComPtr<ID3D11ComputeShader> indirectCounter;
    ComPtr<ID3D11Buffer> indirectSettings;
    std::array<IndirectBatch,32> indirect{};
    UINT indirectCount=0;
    ComPtr<ID3D11Buffer> cutoutVertices;
    ComPtr<ID3D11VertexShader> cutoutVertex;
    ComPtr<ID3D11PixelShader> cutoutPixel;
    ComPtr<ID3D11InputLayout> cutoutLayout;
    std::array<CutoutBatch,128> cutouts{};
    UINT cutoutCount=0,cutoutBatchCount=0;
    ComPtr<ID3D11Buffer> vertices,weaponVertices,viewmodelVertices,sceneVertices,previousConstants;
    // The last COMPLETE frame's posed characters and world weapons. Shadow and bounce passes run
    // at the first owned world draw, which can precede this frame's character depth draws; they
    // then read these instead of an empty capture (one frame of latency, never missing players).
    ComPtr<ID3D11Buffer> publishedVertices,publishedWeaponVertices,publishedViewmodelVertices,publishedSceneVertices;
    ComPtr<ID3D11Device> device;
    UINT publishedCount=0,publishedWeaponCount=0,publishedViewmodelCount=0,publishedSceneCount=0;
    ComPtr<ID3D11Buffer> coverageVertices,coverageConstants;
    ComPtr<ID3D11VertexShader> coverageVertex;
    ComPtr<ID3D11PixelShader> coveragePixel;
    ComPtr<ID3D11InputLayout> coverageLayout;
    UINT coverageCount=0;
    ComPtr<ID3D11Buffer> readback;
    ComPtr<ID3D11Buffer> nativeConstantsReadback;
    UINT nativeConstantsBytes=0;
    std::array<GlobalsScratch,4> scrubScratch{};
    ComPtr<ID3D11Buffer> previousVsGlobals;
    UINT previousVsGlobalsSlot=~0u;
    bool globalsReplaced=false;
    ComPtr<ID3D11Query> done;
    bool pending=false;ULONGLONG probeTime=0;
    UINT count=0,weaponCount=0,viewmodelCount=0,sceneCount=0,batches=0,overflow=0,directOverflow=0,indirectOverflow=0;
    UINT attempts=0,world=0,tagged=0,character=0,color=0;
    float lastMin=0,lastMax=0;
    std::array<EvidenceSlot,EvidenceSlots> evidence{};
    UINT evidenceCount=0,evidenceCursor=0,evidenceWindowCount=0;
    ULONGLONG evidenceWindow=0;
    UINT sampleClass=0,pendingClass=0,activeOffset=0,pendingOffset=0;
    bool activeSample=false,activeNativeSample=false,pendingNativeSample=false,activeWeapon=false,activeScene=false;
    char activeShader[128]{},pendingShader[128]{};
    Matrix receiverWorldToClip{},pendingReceiverWorldToClip{};
    bool receiverProjectionValid=false,pendingReceiverProjectionValid=false;
    uint64_t generation=1;
    bool wanted=false, requested=false, active=false;
} state;

UINT AvailableSceneCount(){
    // A current masked or indirect scene draw replaces the previous opaque pose
    // too; mixing them can retain a stale solid blocker for a newly cut-out prop.
    if(state.sceneCount||state.coverageCount||state.cutoutCount||state.indirectCount)return state.sceneCount;
    return state.publishedSceneCount;
}

struct Clip4 { float x=0,y=0,z=0,w=0; };
Clip4 MultiplyClip(const Matrix &m, Vec3 p) {
    return {m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3],
            m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7],
            m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11],
            m[12]*p.x+m[13]*p.y+m[14]*p.z+m[15]};
}
bool FiniteMatrix(const Matrix &m) {
    for(float x:m)if(!std::isfinite(x))return false;
    return true;
}
float MatrixMaxAbsDelta(const Matrix &a,const Matrix &b) {
    float result=0;
    for(size_t i=0;i<a.size();++i)result=(std::max)(result,std::abs(a[i]-b[i]));
    return result;
}
float MatrixIdentityResidual(const Matrix &a,const Matrix &b) {
    const Matrix product=Multiply(a,b);float result=0;
    for(size_t i=0;i<product.size();++i){const float expected=i/4==i%4?1.f:0.f;result=(std::max)(result,std::abs(product[i]-expected));}
    return result;
}

bool EnsureNativeConstantsReadback(ID3D11Device *device,UINT bytes) {
    if(!device||bytes<128||bytes>64u*1024u||bytes%16)return false;
    if(state.nativeConstantsReadback&&state.nativeConstantsBytes==bytes)return true;
    if(state.pending)return false;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> coverageVertices,coverageConstants;
    ComPtr<ID3D11VertexShader> coverageVertex;
    ComPtr<ID3D11PixelShader> coveragePixel;
    ComPtr<ID3D11InputLayout> coverageLayout;
    UINT coverageCount=0;
    ComPtr<ID3D11Buffer> readback;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&readback)))return false;
    state.nativeConstantsReadback=readback;state.nativeConstantsBytes=bytes;return true;
}

void LogAnimatedCasterProjection(const char *shader,UINT offset,UINT casterClass,const float *vertices,
    const void *nativeBytes,UINT nativeByteWidth) {
    if(!MvmDebugLog_Active())return;
    Matrix nativeProjectionToWorld{};bool nativeValid=nativeBytes&&nativeByteWidth>=128;
    if(nativeValid){
        // NativeView declares this matrix column_major. D3D11 stores columns
        // contiguously, while Environment::Matrix is row-major.
        const auto values=static_cast<const float*>(nativeBytes);
        for(UINT row=0;row<4;++row)for(UINT column=0;column<4;++column)
            nativeProjectionToWorld[row*4+column]=values[16+column*4+row];
        nativeValid=FiniteMatrix(nativeProjectionToWorld);
    }
    Matrix nativeWorldToClip{},receiverInverse{};
    const bool nativeInverseValid=nativeValid&&Inverse(nativeProjectionToWorld,nativeWorldToClip);
    const auto &receiver=state.pendingReceiverWorldToClip;
    const bool receiverValid=state.pendingReceiverProjectionValid&&FiniteMatrix(receiver)&&
        Inverse(receiver,receiverInverse);
    float nativeResidual=0,receiverResidual=0,forwardDelta=0,inverseDelta=0,ndcDelta=0;
    float nativeDepth[3]{},receiverDepth[3]{};bool depthValid=true;
    if(nativeInverseValid)nativeResidual=MatrixIdentityResidual(nativeWorldToClip,nativeProjectionToWorld);
    if(receiverValid)receiverResidual=MatrixIdentityResidual(receiver,receiverInverse);
    if(nativeInverseValid&&receiverValid){
        forwardDelta=MatrixMaxAbsDelta(nativeWorldToClip,receiver);
        inverseDelta=MatrixMaxAbsDelta(nativeProjectionToWorld,receiverInverse);
    }
    for(UINT i=0;i<3;++i){
        Vec3 p{vertices[i*3],vertices[i*3+1],vertices[i*3+2]};
        const Clip4 n=nativeInverseValid?MultiplyClip(nativeWorldToClip,p):Clip4{};
        const Clip4 r=receiverValid?MultiplyClip(receiver,p):Clip4{};
        if(nativeInverseValid&&std::abs(n.w)>1e-8f)nativeDepth[i]=n.z/n.w;else depthValid=false;
        if(receiverValid&&std::abs(r.w)>1e-8f)receiverDepth[i]=r.z/r.w;else depthValid=false;
        if(nativeInverseValid&&receiverValid&&std::abs(n.w)>1e-8f&&std::abs(r.w)>1e-8f){
            ndcDelta=(std::max)(ndcDelta,std::abs(n.x/n.w-r.x/r.w));
            ndcDelta=(std::max)(ndcDelta,std::abs(n.y/n.w-r.y/r.w));
            ndcDelta=(std::max)(ndcDelta,std::abs(n.z/n.w-r.z/r.w));
        }
    }
    MvmDebugLog_LinefAlways("environment.caster.projection",
        "shader=%s offset=%u class=%s nativeMatrix=%u receiverMatrix=%u nativeInvResidual=%.6g receiverInvResidual=%.6g forwardDelta=%.6g inverseDelta=%.6g ndcDelta=%.6g depthNative=%.6g,%.6g,%.6g depthReceiver=%.6g,%.6g,%.6g depthValid=%u",
        shader&&*shader?shader:"<unnamed>",offset,casterClass==2?"scene":casterClass==0?"character":"weapon",nativeInverseValid?1:0,
        receiverValid?1:0,nativeResidual,receiverResidual,forwardDelta,inverseDelta,ndcDelta,
        nativeDepth[0],nativeDepth[1],nativeDepth[2],receiverDepth[0],receiverDepth[1],receiverDepth[2],depthValid?1:0);
}

void LogAnimatedCasterDrawEvidence(const char* name,bool hasTag,const char* reason,bool accepted,
    UINT indices,UINT instances,D3D11_PRIMITIVE_TOPOLOGY topology,bool hasRtv,bool hasDsv,
    UINT depthWidth,UINT depthHeight,float minDepth,float maxDepth) {
    if(!MvmDebugLog_Active())return;
    const ULONGLONG now=GetTickCount64();
    if(!state.evidenceWindow||now-state.evidenceWindow>=3000){state.evidenceWindow=now;state.evidenceWindowCount=0;}
    std::string key=name&&*name?name:"<unnamed>";
    key.push_back('|');key+=reason;
    EvidenceSlot* slot=nullptr;
    for(UINT i=0;i<state.evidenceCount;++i)if(state.evidence[i].key==key){slot=&state.evidence[i];break;}
    if(!slot){
        if(state.evidenceCount<EvidenceSlots)slot=&state.evidence[state.evidenceCount++];
        else slot=&state.evidence[state.evidenceCursor++%EvidenceSlots];
        slot->key=key;slot->last=0;
    }
    if(slot->last&&now-slot->last<5000)return;
    if(state.evidenceWindowCount>=16)return;
    slot->last=now;++state.evidenceWindowCount;
    MvmDebugLog_LinefAlways("environment.caster.draw",
        "shader=%s reflectionTag=%u accepted=%u reason=%s indices=%u instances=%u topology=%u rtv=%u dsv=%u depth=%ux%u viewportDepth=%.5f..%.5f",
        name&&*name?name:"<unnamed>",hasTag?1:0,accepted?1:0,reason,indices,instances,
        static_cast<UINT>(topology),hasRtv?1:0,hasDsv?1:0,depthWidth,depthHeight,minDepth,maxDepth);
}

void CopyCasterShaderName(char (&destination)[128],const char* source) {
    destination[0]=0;
    if(source)std::strncpy(destination,source,sizeof(destination)-1);
    destination[sizeof(destination)-1]=0;
}

bool CachedCasterShader(const std::string& source,const char* name,const char* profile,std::vector<unsigned char>& bytes){
    static std::mutex mutex;static std::map<std::string,std::vector<unsigned char>> cache;
    const std::string key=std::string(name)+"|"+profile+"|"+source;
    std::lock_guard<std::mutex> lock(mutex);auto found=cache.find(key);
    if(found!=cache.end()){bytes=found->second;return true;}
    if(cache.size()>=4096)return false; // one entry per shader variant and capture kind: the game has hundreds of variants, and the old cap of 256 left the last-created ones (some character skin variants) without a capture shader
    ComPtr<ID3DBlob> code,error;
    if(FAILED(CompileEnvironmentShader(source.data(),source.size(),name,nullptr,"Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||!code)return false;
    const auto* first=static_cast<const unsigned char*>(code->GetBufferPointer());
    bytes.assign(first,first+code->GetBufferSize());cache.emplace(key,bytes);return true;
}

bool AttachGlobalsScrubber(ID3D11Device* device,ID3D11VertexShader* shader,
    ID3D11ShaderReflection* reflection,const GlobalsScrubInfo& info) {
    if(!device||!shader||!reflection||!info.bytes||(!info.zeroCount&&info.oneOffset==~0u))return false;
    std::string source="RWByteAddressBuffer data:register(u0);\n[numthreads(1,1,1)] void Main(uint3 id:SV_DispatchThreadID){";
    for(UINT i=0;i<info.zeroCount;++i)source+="data.Store("+std::to_string(info.zeroOffsets[i])+",0);";
    if(info.oneOffset!=~0u)source+="data.Store("+std::to_string(info.oneOffset)+",1);";
    source+="}";
    std::vector<unsigned char> code;
    if(!CachedCasterShader(source,"AnimatedCasterGlobalsScrub","cs_5_0",code))return false;
    ComPtr<ID3D11ComputeShader> scrub;
    if(FAILED(device->CreateComputeShader(code.data(),code.size(),nullptr,&scrub)))return false;
    if(FAILED(::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),GlobalsScrubShaderTag,scrub.Get())))return false;
    return SUCCEEDED(shader->SetPrivateData(GlobalsScrubInfoTag,sizeof(info),&info));
}

GlobalsScratch* EnsureGlobalsScratch(ID3D11Device* device,UINT bytes) {
    if(!device||bytes<16||bytes%16||bytes>64u*1024u)return nullptr;
    for(auto& scratch:state.scrubScratch)
        if(scratch.bytes==bytes&&scratch.clone&&scratch.raw&&scratch.uav)return &scratch;
    GlobalsScratch* target=nullptr;
    for(auto& scratch:state.scrubScratch)if(!scratch.bytes){target=&scratch;break;}
    if(!target)return nullptr;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&target->clone)))return nullptr;
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&target->raw))){target->clone.Reset();return nullptr;}
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};uavDesc.Format=DXGI_FORMAT_R32_TYPELESS;
    uavDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uavDesc.Buffer.NumElements=bytes/4;
    uavDesc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
    if(FAILED(device->CreateUnorderedAccessView(target->raw.Get(),&uavDesc,&target->uav))){
        target->clone.Reset();target->raw.Reset();return nullptr;
    }
    target->bytes=bytes;return target;
}
}
void NoteAnimatedCasterPixelShader(ID3D11PixelShader* shader,const void* code,size_t bytes) {
    if(!shader||!code)return;
    if(bytes>=32&&!std::memcmp(code,"DXBC",4))
        shader->SetPrivateData(PixelSignatureTag,16,static_cast<const unsigned char*>(code)+4);
    if(NativeDepthCutoutCoverage(code,bytes)){
        auto library=EnvironmentShaderCompiler();
        using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
        auto reflect=library?reinterpret_cast<Reflect>(GetProcAddress(library,"D3DReflect")):nullptr;
        ComPtr<ID3D11ShaderReflection> r;D3D11_SHADER_DESC desc{};D3D11_SIGNATURE_PARAMETER_DESC uv{};
        if(reflect&&SUCCEEDED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&r))&&
           SUCCEEDED(r->GetDesc(&desc))&&desc.InputParameters==1&&
           SUCCEEDED(r->GetInputParameterDesc(0,&uv))&&uv.Register==0&&uv.Mask==3&&
           uv.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32&&uv.SystemValueType==D3D_NAME_UNDEFINED&&
           !strcmp(uv.SemanticName,"TEXCOORD")&&uv.SemanticIndex==0){
            const UINT valid=1;shader->SetPrivateData(CutoutPixelTag,sizeof(valid),&valid);
        }
    }
    bool alphaOne=false;UINT alphaInput=~0u,reason=0;
    UINT opaque=OpaquePixelShaderCoverage(code,bytes,&reason,&alphaOne,&alphaInput)?1u:0u;
    if(opaque&&alphaInput!=~0u){
        auto library=EnvironmentShaderCompiler();
        using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
        auto reflect=library?reinterpret_cast<Reflect>(GetProcAddress(library,"D3DReflect")):nullptr;
        ComPtr<ID3D11ShaderReflection> reflection;D3D11_SHADER_DESC desc{};
        if(reflect&&SUCCEEDED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&reflection))&&SUCCEEDED(reflection->GetDesc(&desc))){
            for(UINT i=0;i<desc.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC d{};
                if(SUCCEEDED(reflection->GetInputParameterDesc(i,&d))&&d.Register==alphaInput&&
                   d.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32&&(d.Mask&8)&&d.SystemValueType==D3D_NAME_UNDEFINED&&
                   !strcmp(d.SemanticName,"TEXCOORD")){
                    shader->SetPrivateData(VertexAlphaTag,sizeof(d.SemanticIndex),&d.SemanticIndex);break;}}
        }
    }
    if(!opaque)opaque=reason+1;
    if(reason==1&&MvmDebugLog_Active()){
        static std::atomic<unsigned> reported{0};
        if(reported.fetch_add(1)<8){UINT head[8]{};std::memcpy(head,code,(std::min)(bytes,sizeof(head)));
            MvmDebugLog_LinefAlways("environment.caster.pixel","size=%zu header=%08x declared=%u chunks=%u",bytes,head[0],head[6],head[7]);}
    }
    shader->SetPrivateData(OpaquePixelTag,sizeof(opaque),&opaque);
    const UINT alpha=alphaOne?1u:0u;shader->SetPrivateData(ConstantAlphaTag,sizeof(alpha),&alpha);
}

void NoteAnimatedCasterShader(ID3D11VertexShader* shader,const void* code,size_t bytes) {
    NoteParticleShadowShader(shader,code,bytes);
    if(!shader||!code)return;
    const char* first=static_cast<const char*>(code),*field="g_matProjectionToWorld";
    if(std::search(first,first+bytes,field,field+strlen(field))==first+bytes)return;
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    static auto library=EnvironmentShaderCompiler();
    auto reflect=library?reinterpret_cast<Reflect>(GetProcAddress(library,"D3DReflect")):nullptr;
    ComPtr<ID3D11ShaderReflection> r;
    if(!reflect||FAILED(reflect(code,bytes,__uuidof(ID3D11ShaderReflection),&r)))return;
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    if(FAILED(r->GetResourceBindingDescByName("PerViewConstantBuffer_t",&binding))||
       binding.Type!=D3D_SIT_CBUFFER||binding.BindPoint>=14)return;
    auto cb=r->GetConstantBufferByName("PerViewConstantBuffer_t");
    const char* fields[]={"g_matProjectionToWorld","g_vWorldToCameraOffset"};
    const UINT offsets[]={64,384},sizes[]={64,16};
    for(int i=0;i<2;++i){D3D11_SHADER_VARIABLE_DESC d{};
        if(FAILED(cb->GetVariableByName(fields[i])->GetDesc(&d))||d.StartOffset!=offsets[i]||d.Size!=sizes[i])return;}
    // Weapon and character shaders apply first-person clamps/distortion from $Globals
    // before writing SV_Position. Keep this metadata reflection-driven: never assume cb0
    // or a fixed layout for a shader variant, and never modify the native buffer in place.
    GlobalsScrubInfo scrubInfo{};D3D11_SHADER_INPUT_BIND_DESC globalsBinding{};
    auto globals=r->GetConstantBufferByName("$Globals");D3D11_SHADER_BUFFER_DESC globalsDesc{};
    if(globals&&SUCCEEDED(r->GetResourceBindingDescByName("$Globals",&globalsBinding))&&
       globalsBinding.Type==D3D_SIT_CBUFFER&&globalsBinding.BindPoint<14&&
       SUCCEEDED(globals->GetDesc(&globalsDesc))&&globalsDesc.Size%16==0){
        scrubInfo.slot=globalsBinding.BindPoint;scrubInfo.bytes=globalsDesc.Size;
        auto addField=[&](const char* name,bool zero,bool one){
            auto variable=globals->GetVariableByName(name);D3D11_SHADER_VARIABLE_DESC d{};
            if(!variable||FAILED(variable->GetDesc(&d))||d.Size!=4||d.StartOffset+4>scrubInfo.bytes||d.StartOffset%4)return;
            if(zero&&scrubInfo.zeroCount<4)scrubInfo.zeroOffsets[scrubInfo.zeroCount++]=d.StartOffset;
            if(one)scrubInfo.oneOffset=d.StartOffset;
        };
        addField("g_flPlayerOcclusionAmount",true,false);
        addField("g_flWeaponOcclusionAmount",true,false);
        addField("g_flFirstpersonLegsFade",true,false);
        addField("g_flZBias",true,false);
        addField("g_bDisableFirstpersonLegsDistortion",false,true);
        if(!scrubInfo.zeroCount&&scrubInfo.oneOffset==~0u)scrubInfo.bytes=0;
    }
    D3D11_SHADER_DESC desc{};if(FAILED(r->GetDesc(&desc)))return;
    UINT skinInput=0;
    for(UINT i=0;i<desc.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC d{};
        if(SUCCEEDED(r->GetInputParameterDesc(i,&d))&&!strcmp(d.SemanticName,"BLENDINDICES")&&d.Mask)skinInput=1;}
    shader->SetPrivateData(SkinInputTag,sizeof(skinInput),&skinInput);
    bool position=false;std::string input="struct Input {\n";
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> signature;
    for(UINT i=0;i<desc.OutputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC d{};
        if(FAILED(r->GetOutputParameterDesc(i,&d)))return;signature.push_back(d);}
    std::stable_sort(signature.begin(),signature.end(),[](const auto& a,const auto& b){return a.Register!=b.Register?a.Register<b.Register:(a.Mask&-int(a.Mask))<(b.Mask&-int(b.Mask));});
    for(UINT i=0;i<signature.size();++i){const auto& d=signature[i];
        // Reproduce the entire native output signature. CS2's character/weapon
        // SV_Position is register 8, not register 0. A one-field GS mislinks it.
        const UINT lane=d.Mask&-int(d.Mask),lanes=(d.Mask&1u)+((d.Mask>>1)&1u)+((d.Mask>>2)&1u)+((d.Mask>>3)&1u);
        // Several semantics may share one register (a float3 with a float in .w): the compiler packs a GS input the same way, and the post-compile check below fails closed if it does not.
        if(!d.Mask||(d.Mask/lane+1)!=(1u<<lanes)||(i&&d.Register<signature[i-1].Register))return;
        const char* type=d.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32?"float":d.ComponentType==D3D_REGISTER_COMPONENT_UINT32?"uint":d.ComponentType==D3D_REGISTER_COMPONENT_SINT32?"int":nullptr;
        if(!type)return;
        const bool p=d.SystemValueType==D3D_NAME_POSITION;
        if(p){if(d.Mask!=15||d.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32)return;position=true;}
        else if(d.SystemValueType!=D3D_NAME_UNDEFINED)return;
        input+=std::string(type)+std::to_string(lanes)+" "+(p?"position":"unused"+std::to_string(i))+":"+d.SemanticName+std::to_string(d.SemanticIndex)+";\n";
    }
    if(!position)return;input+="};";
    std::string source=AnimatedCasterCaptureShader;
    const std::string original="struct Input {float4 position:SV_Position;};";
    source.replace(source.find(original),original.size(),input);
    std::vector<unsigned char> compiled;
    if(!CachedCasterShader(source,"AnimatedCasterCapture","gs_5_0",compiled))return;
    ComPtr<ID3D11ShaderReflection> check;
    if(FAILED(reflect(compiled.data(),compiled.size(),__uuidof(ID3D11ShaderReflection),&check)))return;
    // Fail closed if the compiler repacks any register/component.
    for(UINT i=0;i<signature.size();++i){D3D11_SIGNATURE_PARAMETER_DESC d{};
        if(FAILED(check->GetInputParameterDesc(i,&d))||d.Register!=signature[i].Register||d.Mask!=signature[i].Mask||
           d.ComponentType!=signature[i].ComponentType||d.SemanticIndex!=signature[i].SemanticIndex||strcmp(d.SemanticName,signature[i].SemanticName))return;}
    ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11GeometryShader> capture;
    const D3D11_SO_DECLARATION_ENTRY entry{0,"POSITION",0,0,3,0};const UINT stride=12;
    if(FAILED(device->CreateGeometryShaderWithStreamOutput(compiled.data(),compiled.size(),&entry,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&capture)))return;
    if(scrubInfo.bytes&&!AttachGlobalsScrubber(device.Get(),shader,r.Get(),scrubInfo))return;
    if(SUCCEEDED(::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CaptureTag,capture.Get())))shader->SetPrivateData(ViewTag,sizeof(binding.BindPoint),&binding.BindPoint);
    // Depth-prepass UVs are TEXCOORD0. Retain their post-VS values beside the
    // actual posed world positions; never reconstruct UVs from screen depth.
    for(UINT i=0;i<signature.size();++i){const auto& d=signature[i];
        if(strcmp(d.SemanticName,"TEXCOORD")||d.SemanticIndex!=0||(d.Mask&3)!=3||d.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32)continue;
        std::string cutoutSource=source;
        const std::string output="struct Output {float3 position:POSITION;};",positionCode="v.position=p.xyz/p.w;";
        cutoutSource.replace(cutoutSource.find(output),output.size(),"struct Output {float3 position:POSITION;float2 uv:TEXCOORD0;};");
        cutoutSource.replace(cutoutSource.find(positionCode),positionCode.size(),positionCode+"v.uv=input[i].unused"+std::to_string(i)+".xy;");
        std::vector<unsigned char> cutoutCode;
        if(!CachedCasterShader(cutoutSource,"DepthCutoutCapture","gs_5_0",cutoutCode))break;
        ComPtr<ID3D11ShaderReflection> check;
        bool linked=SUCCEEDED(reflect(cutoutCode.data(),cutoutCode.size(),__uuidof(ID3D11ShaderReflection),&check));
        for(UINT j=0;linked&&j<signature.size();++j){D3D11_SIGNATURE_PARAMETER_DESC actual{};
            linked=SUCCEEDED(check->GetInputParameterDesc(j,&actual))&&actual.Register==signature[j].Register&&actual.Mask==signature[j].Mask&&
                actual.ComponentType==signature[j].ComponentType&&actual.SemanticIndex==signature[j].SemanticIndex&&!strcmp(actual.SemanticName,signature[j].SemanticName);}
        const D3D11_SO_DECLARATION_ENTRY entries[]={{0,"POSITION",0,0,3,0},{0,"TEXCOORD",0,0,2,0}};
        const UINT cutoutStride=20;ComPtr<ID3D11GeometryShader> cutoutCapture;
        if(linked&&SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(cutoutCode.data(),cutoutCode.size(),entries,2,&cutoutStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&cutoutCapture)))
            ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CutoutCaptureTag,cutoutCapture.Get());
        break;
    }
    // Native cable color/fade is TEXCOORD4. Match this reflected semantic in
    // both stages, and carry its actual posed vertex value through SO. The PS
    // gate below additionally proves that this is the complete opacity formula.
    for(UINT i=0;i<signature.size();++i){const auto& d=signature[i];
        if(strcmp(d.SemanticName,"TEXCOORD")||d.SemanticIndex!=4||d.Mask!=15||d.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32)continue;
        std::string coverageSource=source;
        const std::string output="struct Output {float3 position:POSITION;};";
        coverageSource.replace(coverageSource.find(output),output.size(),"struct Output {float3 position:POSITION;float alpha:ALPHA;};");
        const std::string positionCode="v.position=p.xyz/p.w;";
        coverageSource.replace(coverageSource.find(positionCode),positionCode.size(),positionCode+"v.alpha=input[i].unused"+std::to_string(i)+".w;");
        std::vector<unsigned char> coverageCode;
        if(!CachedCasterShader(coverageSource,"CableCoverageCapture","gs_5_0",coverageCode))break;
        ComPtr<ID3D11ShaderReflection> check;
        bool linked=SUCCEEDED(reflect(coverageCode.data(),coverageCode.size(),__uuidof(ID3D11ShaderReflection),&check));
        for(UINT j=0;linked&&j<signature.size();++j){D3D11_SIGNATURE_PARAMETER_DESC actual{};
            linked=SUCCEEDED(check->GetInputParameterDesc(j,&actual))&&actual.Register==signature[j].Register&&actual.Mask==signature[j].Mask&&
                actual.ComponentType==signature[j].ComponentType&&actual.SemanticIndex==signature[j].SemanticIndex&&!strcmp(actual.SemanticName,signature[j].SemanticName);}
        const D3D11_SO_DECLARATION_ENTRY entries[]={{0,"POSITION",0,0,3,0},{0,"ALPHA",0,0,1,0}};
        const UINT coverageStride=16;ComPtr<ID3D11GeometryShader> coverageCapture;
        if(linked&&SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(coverageCode.data(),coverageCode.size(),entries,2,&coverageStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&coverageCapture))&&
           SUCCEEDED(::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CoverageCaptureTag,coverageCapture.Get())))
            shader->SetPrivateData(CoverageSemanticTag,sizeof(d.SemanticIndex),&d.SemanticIndex);
        break;
    }
    // First-person geometry must use the exact rendered VS output, including its
    // projection adjustments. Store positions relative to that projection's eye,
    // matching ReceiverPosition; the shadow draw anchors them at the real camera.
    const std::string worldPosition="v.position=p.xyz/p.w;";
    source.replace(source.find(worldPosition),worldPosition.size(),
        "float4 eye=mul(projectionToWorld,float4(0,0,1,0));v.position=p.xyz/p.w-eye.xyz/eye.w;");
    std::vector<unsigned char> viewCode;
    if(!CachedCasterShader(source,"ViewmodelCasterCapture","gs_5_0",viewCode))return;
    ComPtr<ID3D11GeometryShader> viewCapture;
    if(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(viewCode.data(),viewCode.size(),&entry,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&viewCapture)))
        ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),ViewCaptureTag,viewCapture.Get());
}
void EndAnimatedCasters(){EndParticleShadows();state=State{};}
void BeginAnimatedCasters(ID3D11Device* device){
    EndAnimatedCasters();if(!device)return;
    state.device=device;BeginParticleShadows(device);
    const UINT stride=12;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=CasterBufferCapacity(1,Capacity)*stride;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&bd,nullptr,&state.vertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.weaponVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.publishedVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.publishedWeaponVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.publishedViewmodelVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.publishedSceneVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.viewmodelVertices))||
       FAILED(device->CreateBuffer(&bd,nullptr,&state.sceneVertices))){EndAnimatedCasters();return;}
    bd.ByteWidth=CasterBufferCapacity(1,CoverageCapacity)*16;
    if(FAILED(device->CreateBuffer(&bd,nullptr,&state.coverageVertices))){EndAnimatedCasters();return;}
    bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.MiscFlags=0;
    if(FAILED(device->CreateBuffer(&bd,nullptr,&state.coverageConstants))){EndAnimatedCasters();return;}
    ComPtr<ID3DBlob> coverageVs,coveragePs,error;
    const auto coverageBytes=std::strlen(AnimatedCasterCoverageShader);
    if(FAILED(CompileEnvironmentShader(AnimatedCasterCoverageShader,coverageBytes,"CableCoverage",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&coverageVs,&error))||
       FAILED(CompileEnvironmentShader(AnimatedCasterCoverageShader,coverageBytes,"CableCoverage",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&coveragePs,&error))||
       FAILED(device->CreateVertexShader(coverageVs->GetBufferPointer(),coverageVs->GetBufferSize(),nullptr,&state.coverageVertex))||
       FAILED(device->CreatePixelShader(coveragePs->GetBufferPointer(),coveragePs->GetBufferSize(),nullptr,&state.coveragePixel))){EndAnimatedCasters();return;}
    const D3D11_INPUT_ELEMENT_DESC coverageElements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"ALPHA",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if(FAILED(device->CreateInputLayout(coverageElements,2,coverageVs->GetBufferPointer(),coverageVs->GetBufferSize(),&state.coverageLayout))){EndAnimatedCasters();return;}
    bd.ByteWidth=CasterBufferCapacity(1,CutoutCapacity)*20;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&bd,nullptr,&state.cutoutVertices))){EndAnimatedCasters();return;}
    ComPtr<ID3DBlob> cutoutVs,cutoutPs;
    const auto cutoutBytes=std::strlen(AnimatedCasterCutoutShader);
    if(FAILED(CompileEnvironmentShader(AnimatedCasterCutoutShader,cutoutBytes,"DepthCutout",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutVs,&error))||
       FAILED(CompileEnvironmentShader(AnimatedCasterCutoutShader,cutoutBytes,"DepthCutout",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutPs,&error))||
       FAILED(device->CreateVertexShader(cutoutVs->GetBufferPointer(),cutoutVs->GetBufferSize(),nullptr,&state.cutoutVertex))||
       FAILED(device->CreatePixelShader(cutoutPs->GetBufferPointer(),cutoutPs->GetBufferSize(),nullptr,&state.cutoutPixel))){EndAnimatedCasters();return;}
    const D3D11_INPUT_ELEMENT_DESC cutoutElements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if(FAILED(device->CreateInputLayout(cutoutElements,2,cutoutVs->GetBufferPointer(),cutoutVs->GetBufferSize(),&state.cutoutLayout))){EndAnimatedCasters();return;}
    bd.ByteWidth=36;bd.BindFlags=0;bd.MiscFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateBuffer(&bd,nullptr,&state.readback))){EndAnimatedCasters();return;}
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    if(FAILED(device->CreateQuery(&query,&state.done))){EndAnimatedCasters();return;}
    // Initialization failure retains the existing conservative draw path.
    state.bounds.Initialize(device);
    ComPtr<ID3DBlob> indirectCode;
    if(SUCCEEDED(CompileEnvironmentShader(IndirectCasterArgumentsShader,sizeof(IndirectCasterArgumentsShader)-1,
        "EnvironmentIndirectCasterArguments",nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&indirectCode,&error))){
        device->CreateComputeShader(indirectCode->GetBufferPointer(),indirectCode->GetBufferSize(),nullptr,&state.indirectCounter);
        D3D11_BUFFER_DESC settings{};settings.ByteWidth=16;settings.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&settings,nullptr,&state.indirectSettings);
    }
}
void RequestAnimatedCasters(bool enabled){
    // Native materials, editor shadows and proof rendering share this capture.
    // An inactive later tier must not erase another consumer's next-frame request.
    if(enabled)state.wanted=state.requested=true;
}
void NoteAnimatedCasterReceiverProjection(const Matrix &worldToClip){
    if(!MvmDebugLog_Active()||!FiniteMatrix(worldToClip))return;
    state.receiverWorldToClip=worldToClip;state.receiverProjectionValid=true;
}
void NextAnimatedCasterFrame(){
    NextParticleShadowFrame();
    if(state.wanted&&MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>3000){last=now;
            MvmDebugLog_LinefAlways("environment.caster","posedVertices=%u characterVertices=%u weaponVertices=%u batches=%u overflow=%u generation=%llu coverage=engine-submitted-world-depth-range-meshes viewmodelVertices=%u viewmodelPolicy=separate-native-pose sceneVertices=%u cableCoverageVertices=%u indirectBatches=%u overflowDirect=%u overflowIndirect=%u",
                state.count+state.weaponCount,state.count,state.weaponCount,state.batches,state.overflow,state.generation,state.viewmodelCount,state.sceneCount,state.coverageCount,state.indirectCount,state.directOverflow,state.indirectOverflow);
            uint64_t poseBytes=0,coverageBytes=0;
            for(auto* buffer:{state.vertices.Get(),state.weaponVertices.Get(),state.viewmodelVertices.Get(),state.sceneVertices.Get(),
                             state.publishedVertices.Get(),state.publishedWeaponVertices.Get(),state.publishedViewmodelVertices.Get(),state.publishedSceneVertices.Get()}){
                D3D11_BUFFER_DESC desc{};if(buffer)buffer->GetDesc(&desc);poseBytes+=desc.ByteWidth;
            }
            for(auto* buffer:{state.coverageVertices.Get(),state.cutoutVertices.Get()}){
                D3D11_BUFFER_DESC desc{};if(buffer)buffer->GetDesc(&desc);coverageBytes+=desc.ByteWidth;
            }
            MvmDebugLog_LinefAlways("environment.caster.storage","poseBytes=%llu coverageBytes=%llu boundedGrowth=1",poseBytes,coverageBytes);
            MvmDebugLog_LinefAlways("environment.caster.cutout","vertices=%u batches=%u",state.cutoutCount,state.cutoutBatchCount);}}
    if(state.wanted&&MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>3000){last=now;
            MvmDebugLog_LinefAlways("environment.caster.gates","attempts=%u world=%u tagged=%u character=%u color=%u lastDepth=%.5f..%.5f",state.attempts,state.world,state.tagged,state.character,state.color,state.lastMin,state.lastMax);}}
    // Publish this frame's completed actor capture before the counters restart.
    // An overflowed frame still holds valid posed triangles, only not all of them.
    if(state.device&&state.publishedVertices&&state.publishedWeaponVertices){
        ComPtr<ID3D11DeviceContext> immediate;state.device->GetImmediateContext(&immediate);
        auto publish=[&](ID3D11Buffer* from,ID3D11Buffer* to,UINT count,UINT& published){
            if(count){const D3D11_BOX box{0,0,0,count*12,1,1};immediate->CopySubresourceRegion(to,0,0,0,0,from,0,&box);}
            published=count;};
        publish(state.vertices.Get(),state.publishedVertices.Get(),state.count,state.publishedCount);
        publish(state.weaponVertices.Get(),state.publishedWeaponVertices.Get(),state.weaponCount,state.publishedWeaponCount);
        // The first-person pose too: its shadow pass runs before this frame's viewmodel draws.
        if(state.publishedViewmodelVertices)publish(state.viewmodelVertices.Get(),state.publishedViewmodelVertices.Get(),state.viewmodelCount,state.publishedViewmodelCount);
        // Animated props can be submitted after the first owned world draw too.
        // Keep their completed pose available to that early local-light shadow pass.
        if(state.publishedSceneVertices)publish(state.sceneVertices.Get(),state.publishedSceneVertices.Get(),state.sceneCount,state.publishedSceneCount);
    }
    state.count=state.weaponCount=state.viewmodelCount=state.sceneCount=state.coverageCount=state.batches=state.overflow=state.directOverflow=state.indirectOverflow=0;++state.generation;
    for(UINT i=0;i<state.cutoutBatchCount;++i){state.cutouts[i].opacity.Reset();state.cutouts[i].sampler.Reset();}
    state.cutoutCount=state.cutoutBatchCount=0;
    for(UINT i=0;i<state.indirect.size();++i){auto& batch=state.indirect[i];
        if(i<state.indirectCount){batch.coverage.opacity.Reset();batch.coverage.sampler.Reset();batch.idleFrames=0;}
        else if(batch.vertices&&++batch.idleFrames>=IndirectRetentionFrames)batch=IndirectBatch{};
    }
    state.indirectCount=0;
    state.attempts=state.world=state.tagged=state.character=state.color=0;
    state.receiverProjectionValid=false;
    state.wanted=state.requested;state.requested=false;
}
bool BeginAnimatedCasterDraw(ID3D11DeviceContext* ctx,UINT indices,UINT instances){
    if(!state.wanted||state.active||!state.vertices||!ctx||!indices||!instances||indices%3||
       ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    if(BeginParticleShadowDraw(ctx,indices,instances))return true;
    ++state.attempts;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    state.lastMin=vp.MinDepth;state.lastMax=vp.MaxDepth;
    // Native primary world depth-range pass only. The .95 range alone does not
    // establish third-person versus first-person pose ownership.
    const bool viewmodel=std::abs(vp.MaxDepth-.1f)<1e-6f;
    if(count!=1||vp.Width<256||vp.Height<256||vp.MinDepth!=0||(!viewmodel&&std::abs(vp.MaxDepth-.95f)>1e-6f)){
        // Which other passes draw skinned characters (engine shadow cascades would carry the
        // players the main view culls). Once per shader and viewport shape.
        if(MvmDebugLog_Active()){
            ComPtr<ID3D11VertexShader> other;ctx->VSGetShader(&other,nullptr,nullptr);char shader[128]{};UINT bytes=sizeof(shader)-1;
            if(other&&SUCCEEDED(other->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,shader))&&(strstr(shader,"character")||strstr(shader,"depth_only"))){
                static std::set<std::string> seen;char key[256];
                sprintf_s(key,"%s %gx%g %g..%g n=%u",shader,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth,count);
                if(seen.size()<64&&seen.insert(key).second)MvmDebugLog_LinefAlways("environment.caster.otherpass","%s indices=%u instances=%u",key,indices,instances);
            }
        }
        return false;
    }
    ++state.world;
    const bool debug=MvmDebugLog_Active();
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC td{};
    bool depthInfo=false;
    if(debug){
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->OMGetRenderTargets(1,&rtv,&dsv);
        if(dsv){ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);
            if(SUCCEEDED(resource.As(&texture))){texture->GetDesc(&td);depthInfo=true;}}
    }
    const auto evidence=[&](const char* name,bool hasTag,const char* reason,bool accepted){
        LogAnimatedCasterDrawEvidence(name,hasTag,reason,accepted,indices,instances,topology,
            !!rtv,!!dsv,depthInfo?td.Width:0,depthInfo?td.Height:0,vp.MinDepth,vp.MaxDepth);};
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
    if(!vs){evidence("<no-vertex-shader>",false,"missing-vertex-shader",false);return false;}
    UINT slot=~0u,size=sizeof(slot);
    const bool hasTag=SUCCEEDED(vs->GetPrivateData(ViewTag,&size,&slot))&&size==sizeof(slot);
    if(!hasTag&&!debug)return false;
    char name[128]{};size=sizeof(name)-1;
    const bool hasName=SUCCEEDED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name))&&name[0];
    if(!hasTag){evidence(hasName?name:"<unnamed>",false,"missing-reflection-tag",false);return false;}
    if(slot>=14){evidence(hasName?name:"<unnamed>",true,"reflection-bindpoint-invalid",false);return false;}
    ++state.tagged;
    if(!hasName){evidence("<unnamed>",true,"missing-debug-name",false);return false;}
    const bool depth=!strcmp(name,"csgo_depth_only.vfx_vs");
    UINT skinInput=0,skinBytes=sizeof(skinInput);ComPtr<ID3D11PixelShader> depthPixel;
    if(depth)ctx->PSGetShader(&depthPixel,nullptr,nullptr);
    const bool depthPose=depth&&!depthPixel&&SUCCEEDED(vs->GetPrivateData(SkinInputTag,&skinBytes,&skinInput))&&skinBytes==sizeof(skinInput)&&skinInput==1;
    const bool cutout=depth&&!depthPose;
    const bool scene=cutout||depthPose||!strcmp(name,"csgo_vertexlitgeneric.vfx_vs")||!strcmp(name,"csgo_complex.vfx_vs")||!strcmp(name,"cables.vfx_vs")||!strcmp(name,"csgo_simple.vfx_vs");
    if((viewmodel&&scene)||(!scene&&strcmp(name,"csgo_character.vfx_vs")&&strcmp(name,"csgo_weapon.vfx_vs"))){
        evidence(name,true,"shader-class-not-allowlisted",false);return false;
    }
    bool sceneOpaqueAlpha=false,vertexCoverage=false;
    if(cutout){
        ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
        UINT valid=0,tagSize=sizeof(valid);
        if(!ps||FAILED(ps->GetPrivateData(CutoutPixelTag,&tagSize,&valid))||tagSize!=sizeof(valid)||valid!=1){
            evidence(name,true,"depth-cutout-opacity-unverified",false);return false;
        }
        ComPtr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;ctx->OMGetDepthStencilState(&depthState,&stencilRef);
        if(depthState){D3D11_DEPTH_STENCIL_DESC desc{};depthState->GetDesc(&desc);
            if(!desc.DepthEnable||desc.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL||desc.DepthFunc==D3D11_COMPARISON_EQUAL){
                evidence(name,true,"not-a-depth-writing-prepass",false);return false;}}
    }
    if(depthPose){
        ComPtr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;ctx->OMGetDepthStencilState(&depthState,&stencilRef);
        if(depthState){D3D11_DEPTH_STENCIL_DESC desc{};depthState->GetDesc(&desc);
            if(!desc.DepthEnable||desc.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL||desc.DepthFunc==D3D11_COMPARISON_EQUAL){
                evidence(name,true,"skinned-depth-not-writing",false);return false;}}
    }
    if(scene&&!cutout&&!depthPose){
        ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
        UINT opaque=0,opaqueSize=sizeof(opaque);
        if(!ps||FAILED(ps->GetPrivateData(OpaquePixelTag,&opaqueSize,&opaque))||opaqueSize!=sizeof(opaque)||opaque!=1){
            char reason[80];std::snprintf(reason,sizeof(reason),"scene-pixel-coverage-rejected-%u",opaque);evidence(name,true,reason,false);return false;
        }
        UINT alpha=0,alphaSize=sizeof(alpha);
        sceneOpaqueAlpha=SUCCEEDED(ps->GetPrivateData(ConstantAlphaTag,&alphaSize,&alpha))&&alphaSize==sizeof(alpha)&&alpha==1;
        UINT semantic=~0u,semanticSize=sizeof(semantic),vsSemantic=~0u,vsSemanticSize=sizeof(vsSemantic);
        vertexCoverage=!strcmp(name,"cables.vfx_vs")&&
            SUCCEEDED(ps->GetPrivateData(VertexAlphaTag,&semanticSize,&semantic))&&semanticSize==sizeof(semantic)&&
            SUCCEEDED(vs->GetPrivateData(CoverageSemanticTag,&vsSemanticSize,&vsSemantic))&&vsSemanticSize==sizeof(vsSemantic)&&semantic==vsSemantic;
    }
    ++state.character;
    if(!debug)ctx->OMGetRenderTargets(1,&rtv,&dsv);
    if((!rtv&&!cutout&&!depthPose)||!dsv){evidence(name,true,"missing-rtv-or-depth",false);return false;}
    ++state.color;
    if(!depthInfo){ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);
        if(FAILED(resource.As(&texture))){evidence(name,true,"depth-resource-unavailable",false);return false;}
        texture->GetDesc(&td);depthInfo=true;}
    if(td.Width!=UINT(vp.Width)||td.Height!=UINT(vp.Height)||td.ArraySize!=1){
        evidence(name,true,"depth-size-mismatch",false);return false;
    }
    if(!debug)ctx->IAGetPrimitiveTopology(&topology);
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST){evidence(name,true,"topology-not-triangle-list",false);return false;}
    // A native GS, tessellation, SO or alpha-tested/blended body needs its own verified path.
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds){evidence(name,true,"native-shader-stage-present",false);return false;}
    ID3D11Buffer* targets[4]{};ctx->SOGetTargets(4,targets);bool occupied=false;
    for(auto* b:targets)if(b){occupied=true;b->Release();}
    if(occupied){evidence(name,true,"stream-output-already-bound",false);return false;}
    ComPtr<ID3D11BlendState> blend;float factors[4]{};UINT mask=0;ctx->OMGetBlendState(&blend,factors,&mask);
    if(blend){D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);const auto &target=bd.RenderTarget[0];
        // Complex cutouts can report alpha=1 in their color PS because a prior
        // depth pass already applied the texture mask. Color opacity is not a
        // proof of whole-mesh coverage for these alpha-to-coverage materials.
        if(bd.AlphaToCoverageEnable&&!strcmp(name,"csgo_complex.vfx_vs"))sceneOpaqueAlpha=false;
        const bool identityBlend=!target.BlendEnable||(target.BlendOp==D3D11_BLEND_OP_ADD&&
            ((target.SrcBlend==D3D11_BLEND_ONE&&target.DestBlend==D3D11_BLEND_ZERO)||
             (target.SrcBlend==D3D11_BLEND_SRC_ALPHA&&target.DestBlend==D3D11_BLEND_INV_SRC_ALPHA)));
        vertexCoverage=vertexCoverage&&(bd.AlphaToCoverageEnable||
            (target.BlendEnable&&target.BlendOp==D3D11_BLEND_OP_ADD&&target.SrcBlend==D3D11_BLEND_SRC_ALPHA&&target.DestBlend==D3D11_BLEND_INV_SRC_ALPHA));
        // Constant alpha=1 makes ordinary alpha blending and alpha-to-coverage opaque.
        // Additive/multiplicative draws and unknown/variable alpha remain excluded.
        const bool fullMask=(mask&((uint64_t(1)<<td.SampleDesc.Count)-1))==((uint64_t(1)<<td.SampleDesc.Count)-1);
        const bool provedOpaque=sceneOpaqueAlpha&&identityBlend&&fullMask;
        // Vertex fade is represented with coverage in the shadow map. Only the
        // ordinary native alpha blend is compatible; additive light stays out.
        const bool covered=vertexCoverage&&identityBlend&&fullMask;
        const bool cutoutCoverage=cutout&&!target.BlendEnable&&fullMask;
        if((bd.AlphaToCoverageEnable||target.BlendEnable)&&!provedOpaque&&!covered&&!cutoutCoverage){
            UINT signature[4]{},signatureSize=sizeof(signature);
            if(debug){ComPtr<ID3D11PixelShader> pixel;ctx->PSGetShader(&pixel,nullptr,nullptr);
                if(pixel)pixel->GetPrivateData(PixelSignatureTag,&signatureSize,signature);}
            char reason[160];std::snprintf(reason,sizeof(reason),"alpha-or-blend-enabled-a2c%u-blend%u-src%u-dst%u-ps%08x%08x%08x%08x",unsigned(bd.AlphaToCoverageEnable),unsigned(bd.RenderTarget[0].BlendEnable),unsigned(bd.RenderTarget[0].SrcBlend),unsigned(bd.RenderTarget[0].DestBlend),signature[0],signature[1],signature[2],signature[3]);
            evidence(name,true,reason,false);return false;}}
    else vertexCoverage=false;
    const bool weapon=!strcmp(name,"csgo_weapon.vfx_vs");
    const uint64_t vertices=uint64_t(indices)*instances;
    const UINT classCount=cutout?state.cutoutCount:vertexCoverage?state.coverageCount:viewmodel?state.viewmodelCount:scene?state.sceneCount:weapon?state.weaponCount:state.count;
    if(vertices>(cutout?CutoutCapacity:vertexCoverage?CoverageCapacity:Capacity)-classCount||
       (cutout&&state.cutoutBatchCount>=state.cutouts.size())){++state.overflow;++state.directOverflow;
        evidence(name,true,"capture-capacity-exceeded",false);return false;}
    auto& storage=cutout?state.cutoutVertices:vertexCoverage?state.coverageVertices:viewmodel?state.viewmodelVertices:
        scene?state.sceneVertices:weapon?state.weaponVertices:state.vertices;
    const UINT storageStride=cutout?20:vertexCoverage?16:12;
    const UINT storageLimit=cutout?CutoutCapacity:vertexCoverage?CoverageCapacity:Capacity;
    const UINT required=classCount+UINT(vertices);
    bool storageReady=EnsureCasterBuffer(state.device.Get(),ctx,storage,required,classCount,storageStride,storageLimit);
    if(storageReady&&!cutout&&!vertexCoverage){
        auto& published=viewmodel?state.publishedViewmodelVertices:scene?state.publishedSceneVertices:
            weapon?state.publishedWeaponVertices:state.publishedVertices;
        const UINT previousCount=viewmodel?state.publishedViewmodelCount:scene?state.publishedSceneCount:
            weapon?state.publishedWeaponCount:state.publishedCount;
        // Publication happens before next-frame shadows. Reserve its destination
        // before accepting this draw, retaining the previous complete pose on growth.
        storageReady=EnsureCasterBuffer(state.device.Get(),ctx,published,(std::max)(required,previousCount),previousCount,12,Capacity);
    }
    if(!storageReady){++state.overflow;++state.directOverflow;evidence(name,true,"capture-storage-unavailable",false);return false;}
    ComPtr<ID3D11Buffer> cb;ctx->VSGetConstantBuffers(slot,1,&cb);if(!cb){evidence(name,true,"per-view-constant-buffer-missing",false);return false;}
    D3D11_BUFFER_DESC bd{};cb->GetDesc(&bd);if(bd.ByteWidth<400){evidence(name,true,"per-view-constant-buffer-too-small",false);return false;}
    // Replay the native VS with a private $Globals clone that removes only the
    // reflected first-person occlusion/clamp controls. This leaves engine memory
    // untouched and preserves every other material/pose value.
    GlobalsScrubInfo scrubInfo{};UINT scrubInfoSize=sizeof(scrubInfo);ComPtr<ID3D11ComputeShader> scrubShader;
    UINT scrubShaderSize=sizeof(ID3D11ComputeShader*);
    const bool hasScrubInfo=SUCCEEDED(vs->GetPrivateData(GlobalsScrubInfoTag,&scrubInfoSize,&scrubInfo))&&
        scrubInfoSize==sizeof(scrubInfo)&&scrubInfo.slot<14&&scrubInfo.bytes&&
        SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(vs),GlobalsScrubShaderTag,&scrubShaderSize,(void**)scrubShader.GetAddressOf()))&&scrubShader;
    if(hasScrubInfo&&!viewmodel&&(!scene||depthPose)){
        bool scrubReady=false;
        ComPtr<ID3D11Buffer> globals;ctx->VSGetConstantBuffers(scrubInfo.slot,1,&globals);D3D11_BUFFER_DESC globalsDesc{};
        if(globals){globals->GetDesc(&globalsDesc);ComPtr<ID3D11Device> device;globals->GetDevice(&device);
            GlobalsScratch* scratch=nullptr;
            if(globalsDesc.ByteWidth>=scrubInfo.bytes&&globalsDesc.ByteWidth%16==0)
                scratch=EnsureGlobalsScratch(device.Get(),globalsDesc.ByteWidth);
            if(scratch){
                ctx->CopyResource(scratch->raw.Get(),globals.Get());
                ComPtr<ID3D11ComputeShader> previousCs;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount=256;
                ctx->CSGetShader(previousCs.GetAddressOf(),previousInstances,&previousInstanceCount);
                ComPtr<ID3D11UnorderedAccessView> previousUav;ctx->CSGetUnorderedAccessViews(0,1,previousUav.GetAddressOf());
                auto scrubUav=scratch->uav.Get();UINT initialCount=~0u;
                ctx->CSSetShader(scrubShader.Get(),nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,&scrubUav,&initialCount);ctx->Dispatch(1,1,1);
                ID3D11UnorderedAccessView* nullUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&nullUav,&initialCount);
                auto restoreUav=previousUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&restoreUav,&initialCount);
                ctx->CSSetShader(previousCs.Get(),previousInstances,previousInstanceCount);
                for(UINT i=0;i<previousInstanceCount;++i)if(previousInstances[i])previousInstances[i]->Release();
                ctx->CopyResource(scratch->clone.Get(),scratch->raw.Get());
                auto scrubbed=scratch->clone.Get();ctx->VSSetConstantBuffers(scrubInfo.slot,1,&scrubbed);
                state.previousVsGlobals=globals;state.previousVsGlobalsSlot=scrubInfo.slot;state.globalsReplaced=true;scrubReady=true;
            }
        }
        if(!scrubReady){evidence(name,true,"globals-scrub-unavailable",false);return false;}
    }
    ComPtr<ID3D11GeometryShader> capture;size=sizeof(ID3D11GeometryShader*);
    if(FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(vs),cutout?CutoutCaptureTag:vertexCoverage?CoverageCaptureTag:viewmodel?ViewCaptureTag:CaptureTag,&size,(void**)capture.GetAddressOf()))||!capture){
        if(state.globalsReplaced){auto original=state.previousVsGlobals.Get();ctx->VSSetConstantBuffers(state.previousVsGlobalsSlot,1,&original);state.previousVsGlobals.Reset();state.previousVsGlobalsSlot=~0u;state.globalsReplaced=false;}
        evidence(name,true,"capture-geometry-shader-missing",false);return false;}
    if(cutout){
        auto& batch=state.cutouts[state.cutoutBatchCount];
        ComPtr<ID3D11ShaderResourceView> opacity;ComPtr<ID3D11SamplerState> sampler;
        ctx->PSGetShaderResources(12,1,&opacity);ctx->PSGetSamplers(0,1,&sampler);
        D3D11_SHADER_RESOURCE_VIEW_DESC opacityDesc{};if(opacity)opacity->GetDesc(&opacityDesc);
        if(!opacity||!sampler||opacityDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D){
            evidence(name,true,"cutout-texture-or-sampler-unavailable",false);return false;}
        for(UINT i=0;i<2;++i){ComPtr<ID3D11Buffer> original;ctx->PSGetConstantBuffers(i,1,&original);
            D3D11_BUFFER_DESC desc{};if(original)original->GetDesc(&desc);
            if(!original||desc.ByteWidth<32||desc.ByteWidth>65536||desc.ByteWidth%16){
                evidence(name,true,"cutout-constants-unavailable",false);return false;}
            if(!batch.constants[i]||batch.constantBytes[i]!=desc.ByteWidth){
                ComPtr<ID3D11Device> device;original->GetDevice(&device);
                D3D11_BUFFER_DESC clone{};clone.ByteWidth=desc.ByteWidth;clone.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                ComPtr<ID3D11Buffer> buffer;if(FAILED(device->CreateBuffer(&clone,nullptr,&buffer))){
                    evidence(name,true,"cutout-constant-clone-failed",false);return false;}
                batch.constants[i]=buffer;batch.constantBytes[i]=desc.ByteWidth;
            }
            ctx->CopyResource(batch.constants[i].Get(),original.Get());
        }
        batch.opacity=opacity;batch.sampler=sampler;batch.first=classCount;batch.count=UINT(vertices);
        ++state.cutoutBatchCount;
    }
    evidence(name,true,depthPose?"accepted-skinned-depth-prepass":cutout?"accepted-native-depth-cutout":vertexCoverage?"accepted-vertex-alpha-coverage":viewmodel?"accepted-viewmodel":state.globalsReplaced?"accepted-globals-scrubbed":"accepted",true);
    ctx->GSGetConstantBuffers(0,1,&state.previousConstants);
    auto buffer=cb.Get();ctx->GSSetConstantBuffers(0,1,&buffer);
    ctx->GSSetShader(capture.Get(),nullptr,0);
    buffer=cutout?state.cutoutVertices.Get():vertexCoverage?state.coverageVertices.Get():viewmodel?state.viewmodelVertices.Get():scene?state.sceneVertices.Get():weapon?state.weaponVertices.Get():state.vertices.Get();UINT offset=classCount*(cutout?20:vertexCoverage?16:12);ctx->SOSetTargets(1,&buffer,&offset);
    state.activeOffset=offset;
    state.activeWeapon=weapon;
    state.activeScene=scene&&!cutout&&!vertexCoverage;
    state.activeSample=debug&&!viewmodel&&!state.pending&&GetTickCount64()-state.probeTime>=3000&&
        ((state.sampleClass==0&&!strcmp(name,"csgo_character.vfx_vs"))||
         (state.sampleClass==1&&!strcmp(name,"csgo_weapon.vfx_vs"))||
         (state.sampleClass==2&&state.activeScene));
    state.activeNativeSample=false;
    if(state.activeSample){
        D3D11_BUFFER_DESC nativeDesc{};cb->GetDesc(&nativeDesc);ComPtr<ID3D11Device> device;
        cb->GetDevice(&device);
        if(EnsureNativeConstantsReadback(device.Get(),nativeDesc.ByteWidth)){
            // Queue this beside the sampled SO copy; pending metadata keeps
            // the matrix associated with this exact shader and SO offset.
            ctx->CopyResource(state.nativeConstantsReadback.Get(),cb.Get());
            state.activeNativeSample=true;
        }
    }
    if(state.activeSample)CopyCasterShaderName(state.activeShader,name);
    state.active=true;(cutout?state.cutoutCount:vertexCoverage?state.coverageCount:viewmodel?state.viewmodelCount:scene?state.sceneCount:weapon?state.weaponCount:state.count)+=UINT(vertices);++state.batches;return true;
}
bool BeginAnimatedCasterIndirectDraw(ID3D11DeviceContext* ctx,ID3D11Buffer* arguments,
                                    UINT offset,UINT stride,UINT count,bool indexed){
    if(!state.wanted||state.active||!state.vertices||!ctx||!arguments||
       ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
    if(!vs)return false;
    UINT skin=0,bytes=sizeof(skin);char name[128]{};UINT nameBytes=sizeof(name)-1;
    if(FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&nameBytes,name))||strcmp(name,"csgo_depth_only.vfx_vs"))return false;
    const bool tagged=SUCCEEDED(vs->GetPrivateData(SkinInputTag,&bytes,&skin))&&bytes==sizeof(skin);
    ComPtr<ID3D11PixelShader> pixel;ctx->PSGetShader(&pixel,nullptr,nullptr);
    UINT cutout=0,cutoutBytes=sizeof(cutout);
    const bool masked=pixel&&SUCCEEDED(pixel->GetPrivateData(CutoutPixelTag,&cutoutBytes,&cutout))&&cutoutBytes==sizeof(cutout)&&cutout==1;
    if(MvmDebugLog_Active()){
        static std::array<ID3D11VertexShader*,16> reported{};static UINT reportedCount=0;
        if(reportedCount<reported.size()&&std::find(reported.begin(),reported.begin()+reportedCount,vs.Get())==reported.begin()+reportedCount){
            reported[reportedCount++]=vs.Get();UINT slot=~0u,slotBytes=sizeof(slot);const bool view=SUCCEEDED(vs->GetPrivateData(ViewTag,&slotBytes,&slot));
            MvmDebugLog_LinefAlways("environment.caster.indirect.candidate","skinTagged=%u skinned=%u pixel=%u maskedVerified=%u perView=%u perViewSlot=%u indexed=%u draws=%u",unsigned(tagged),skin,unsigned(!!pixel),unsigned(masked),unsigned(view),slot,unsigned(indexed),count);
        }
    }
    if(!tagged||skin!=1)return false;
    if(pixel&&!masked)return false; // Unknown opacity must never become an opaque caster.
    D3D11_VIEWPORT viewport{};UINT viewports=1;ctx->RSGetViewports(&viewports,&viewport);
    if(viewports!=1||viewport.Width<256||viewport.Height<256||viewport.MinDepth!=0||std::abs(viewport.MaxDepth-.95f)>1e-6f)return false;
    D3D11_BUFFER_DESC desc{};arguments->GetDesc(&desc);
    const UINT span=IndirectCasterSpan(desc.ByteWidth,offset,stride,count,indexed);
    if(!span||!(desc.MiscFlags&D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS))return false;
    if(!state.indirectCounter||!state.indirectSettings||state.indirectCount>=state.indirect.size()){
        ++state.overflow;++state.indirectOverflow;return false;}
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
    if(predicate)return false; // Argument counts alone cannot prove predicated SO coverage.
    auto& batch=state.indirect[state.indirectCount];ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    const UINT vertexStride=masked?20:12,capacity=(masked?CutoutCapacity:IndirectCapacity)/3*3;
    // Stream-output stride belongs to the current capture shader, not the raw
    // buffer. Keep a sufficiently large allocation when this slot switches
    // between opaque positions (12 bytes) and cutout positions/UVs (20 bytes).
    const UINT vertexBytes=capacity*vertexStride;
    if(!batch.vertices||batch.vertexBytes<vertexBytes){
        D3D11_BUFFER_DESC d{};d.ByteWidth=vertexBytes;
        d.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ComPtr<ID3D11Buffer> result;if(FAILED(device->CreateBuffer(&d,nullptr,&result)))return false;
        MvmDebugLog_LinefAlways("environment.caster.indirect.allocate",
            "slot=%u oldBytes=%u bytes=%u stride=%u capacity=%u",
            state.indirectCount,batch.vertexBytes,vertexBytes,vertexStride,capacity);
        batch.vertices=result;batch.vertexBytes=vertexBytes;
    }
    batch.stride=vertexStride;batch.capacity=capacity;
    auto rawView=[&](ID3D11Buffer* buffer,UINT bytes,ComPtr<ID3D11ShaderResourceView>& result){
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements=bytes/4;view.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        return SUCCEEDED(device->CreateShaderResourceView(buffer,&view,&result));
    };
    if(!batch.arguments||batch.argumentBytes<span){
        D3D11_BUFFER_DESC d{};d.ByteWidth=span;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ComPtr<ID3D11Buffer> result;ComPtr<ID3D11ShaderResourceView> view;
        if(FAILED(device->CreateBuffer(&d,nullptr,&result))||!rawView(result.Get(),span,view))return false;
        batch.arguments=result;batch.argumentView=view;batch.argumentBytes=span;
    }
    if(!batch.metadata){
        D3D11_BUFFER_DESC d{};d.ByteWidth=32;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        ComPtr<ID3D11Buffer> result;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_TYPELESS;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements=8;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(device->CreateBuffer(&d,nullptr,&result))||!rawView(result.Get(),32,view)||
           FAILED(device->CreateUnorderedAccessView(result.Get(),&ud,&uav)))return false;
        batch.metadata=result;batch.metadataView=view;batch.metadataUav=uav;
    }
    if(masked){
        for(UINT i=0;i<2;++i){ComPtr<ID3D11Buffer> original;ctx->PSGetConstantBuffers(i,1,&original);
            D3D11_BUFFER_DESC d{};if(original)original->GetDesc(&d);
            if(!original||d.ByteWidth<32||d.ByteWidth>65536||d.ByteWidth%16)return false;
            if(!batch.coverage.constants[i]||batch.coverage.constantBytes[i]!=d.ByteWidth){
                D3D11_BUFFER_DESC clone{};clone.ByteWidth=d.ByteWidth;clone.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                ComPtr<ID3D11Buffer> result;if(FAILED(device->CreateBuffer(&clone,nullptr,&result)))return false;
                batch.coverage.constants[i]=result;batch.coverage.constantBytes[i]=d.ByteWidth;
            }
            ctx->CopyResource(batch.coverage.constants[i].Get(),original.Get());
        }
        ctx->PSGetShaderResources(12,1,&batch.coverage.opacity);ctx->PSGetSamplers(0,1,&batch.coverage.sampler);
    }
    // Counted on the immediate context itself, inside the engine's draw: replaying a deferred
    // command list here crashed ReShade's d3d11 wrapper (d3d11.dll+0x168d0c, RenoDX DLSS users,
    // 2026-10-13). Only the four compute slots the counter uses are saved and put back.
    ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* previousInstances[256]{};UINT previousInstanceCount=256;
    ctx->CSGetShader(previousShader.GetAddressOf(),previousInstances,&previousInstanceCount);
    ID3D11Buffer* previousConstants=nullptr;ctx->CSGetConstantBuffers(0,1,&previousConstants);
    ID3D11ShaderResourceView* previousView=nullptr;ctx->CSGetShaderResources(0,1,&previousView);
    ID3D11UnorderedAccessView* previousUav=nullptr;ctx->CSGetUnorderedAccessViews(0,1,&previousUav);
    const D3D11_BOX box{offset,0,0,offset+span,1,1};
    ctx->CopySubresourceRegion(batch.arguments.Get(),0,0,0,0,arguments,0,&box);
    const UINT settings[]{count,stride,capacity,indexed?1u:0u};ctx->UpdateSubresource(state.indirectSettings.Get(),0,nullptr,settings,0,0);
    ctx->CSSetShader(state.indirectCounter.Get(),nullptr,0);auto cb=state.indirectSettings.Get();ctx->CSSetConstantBuffers(0,1,&cb);
    auto srv=batch.argumentView.Get();ctx->CSSetShaderResources(0,1,&srv);auto uav=batch.metadataUav.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    ctx->Dispatch(1,1,1);
    UINT keepCounter=~0u;ctx->CSSetUnorderedAccessViews(0,1,&previousUav,&keepCounter);
    ctx->CSSetShaderResources(0,1,&previousView);ctx->CSSetConstantBuffers(0,1,&previousConstants);
    ctx->CSSetShader(previousShader.Get(),previousInstances,previousInstanceCount);
    for(UINT i=0;i<previousInstanceCount;++i)if(previousInstances[i])previousInstances[i]->Release();
    if(previousConstants)previousConstants->Release();if(previousView)previousView->Release();if(previousUav)previousUav->Release();
    // Reuse the reflection, depth-writing, coverage and native-state guards.
    // The placeholder is removed; this batch has its own GPU-counted SO buffer.
    const bool accepted=BeginAnimatedCasterDraw(ctx,3,1);
    if(accepted){
        if(masked){state.cutoutCount-=3;--state.cutoutBatchCount;}else state.sceneCount-=3;
        auto vertices=batch.vertices.Get();UINT start=0;ctx->SOSetTargets(1,&vertices,&start);
        batch.masked=masked;++state.indirectCount;
    }
    if(MvmDebugLog_Active()){
        static ULONGLONG last=0;const auto now=GetTickCount64();
        if(now-last>=3000){last=now;MvmDebugLog_LinefAlways("environment.caster.indirect",
            "accepted=%u indexed=%u draws=%u argumentBytes=%u gpuCount=1 argumentReadback=0 batches=%u",unsigned(accepted),unsigned(indexed),count,span,state.indirectCount);}
    }
    return accepted;
}
void EndAnimatedCasterDraw(ID3D11DeviceContext* ctx){
    if(EndParticleShadowDraw(ctx))return;
    if(!state.active)return;
    ctx->SOSetTargets(0,nullptr,nullptr);ctx->GSSetShader(nullptr,nullptr,0);
    auto buffer=state.previousConstants.Get();ctx->GSSetConstantBuffers(0,1,&buffer);
    state.previousConstants.Reset();state.active=false;
    if(state.globalsReplaced){
        auto original=state.previousVsGlobals.Get();ctx->VSSetConstantBuffers(state.previousVsGlobalsSlot,1,&original);
        state.previousVsGlobals.Reset();state.previousVsGlobalsSlot=~0u;state.globalsReplaced=false;
    }
    if(MvmDebugLog_Active()&&state.readback&&state.done){
        if(state.pending){BOOL ready=FALSE;
            if(ctx->GetData(state.done.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&ready){
                D3D11_MAPPED_SUBRESOURCE mapped{},nativeMapped{};
                const bool nativeReady=!state.pendingNativeSample||
                    SUCCEEDED(ctx->Map(state.nativeConstantsReadback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&nativeMapped));
                const bool vertexReady=SUCCEEDED(ctx->Map(state.readback.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped));
                if(vertexReady&&nativeReady){
                    auto p=static_cast<const float*>(mapped.pData);
                    MvmDebugLog_LinefAlways("environment.caster.vertices","shader=%s offset=%u class=%s a=(%.3f %.3f %.3f) b=(%.3f %.3f %.3f) c=(%.3f %.3f %.3f)",
                        state.pendingShader,state.pendingOffset,state.pendingClass==2?"scene":state.pendingClass==0?"character":"weapon",
                        p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8]);
                    LogAnimatedCasterProjection(state.pendingShader,state.pendingOffset,state.pendingClass,p,
                        state.pendingNativeSample?nativeMapped.pData:nullptr,
                        state.pendingNativeSample?state.nativeConstantsBytes:0);
                    ctx->Unmap(state.readback.Get(),0);state.pending=false;
                    if(state.pendingNativeSample)ctx->Unmap(state.nativeConstantsReadback.Get(),0);
                    state.pendingNativeSample=false;
                }else{
                    if(vertexReady)ctx->Unmap(state.readback.Get(),0);
                    if(state.pendingNativeSample&&nativeReady)ctx->Unmap(state.nativeConstantsReadback.Get(),0);
                }
            }
        }else if(state.activeSample){
            const UINT left=state.activeOffset;D3D11_BOX box{left,0,0,left+36,1,1};
            ctx->CopySubresourceRegion(state.readback.Get(),0,0,0,0,
                state.activeScene?state.sceneVertices.Get():state.activeWeapon?state.weaponVertices.Get():state.vertices.Get(),0,&box);ctx->End(state.done.Get());
            state.pending=true;state.pendingNativeSample=state.activeNativeSample;
            state.pendingReceiverWorldToClip=state.receiverWorldToClip;
            state.pendingReceiverProjectionValid=state.receiverProjectionValid;
            state.pendingOffset=left;state.pendingClass=state.sampleClass;
            CopyCasterShaderName(state.pendingShader,state.activeShader);state.sampleClass=(state.sampleClass+1)%3;
            state.probeTime=GetTickCount64();state.activeSample=false;
        }
    }
    state.activeSample=false;
}
uint64_t AnimatedCasterGeneration(){return state.generation;}
bool ReadAnimatedCasterBounds(ID3D11DeviceContext* context,std::vector<CasterBounds>& result,bool waitForGpu){
    if(state.overflow){result.clear();return false;}
    std::vector<CasterVertexStream> streams{
        {state.count?state.vertices.Get():state.publishedVertices.Get(),state.count?state.count:state.publishedCount,12},
        {state.weaponCount?state.weaponVertices.Get():state.publishedWeaponVertices.Get(),state.weaponCount?state.weaponCount:state.publishedWeaponCount,12},
        {state.sceneCount?state.sceneVertices.Get():state.publishedSceneVertices.Get(),AvailableSceneCount(),12},
        {state.coverageVertices.Get(),state.coverageCount,16},{state.cutoutVertices.Get(),state.cutoutCount,20}};
    for(UINT i=0;i<state.indirectCount;++i){const auto& batch=state.indirect[i];
        streams.push_back({batch.vertices.Get(),batch.capacity,batch.stride,batch.metadataView.Get()});}
    streams.push_back(ParticleShadowStream());
    return state.bounds.Read(context,streams.data(),UINT(streams.size()),result,waitForGpu);
}
bool HasAnimatedCasterGeometry(){return AnimatedCasterVertexCount()!=0||state.indirectCount!=0;}
UINT AnimatedCasterVertexCount(AnimatedCasterSubset subset){
    if(subset==AnimatedCasterSubset::Viewmodel)return state.viewmodelCount?state.viewmodelCount:state.publishedViewmodelCount;
    const UINT characters=state.count?state.count:state.publishedCount,weapons=state.weaponCount?state.weaponCount:state.publishedWeaponCount;
    if(subset==AnimatedCasterSubset::Characters)return characters;
    if(subset==AnimatedCasterSubset::Weapons)return weapons;
    if(subset==AnimatedCasterSubset::Actors)return characters+weapons;
    const UINT scene=AvailableSceneCount();
    const UINT particles=ParticleShadowStream().count;
    if(subset==AnimatedCasterSubset::Scene)return scene+state.coverageCount+state.cutoutCount+particles;
    return characters+weapons+scene+state.coverageCount+state.cutoutCount+particles;
}
void DrawAnimatedCoverageCasters(ID3D11DeviceContext* ctx,const Matrix& worldToShadow){
    if(!ctx)return;
    DrawParticleShadows(ctx,worldToShadow);
    bool indirectMasked=false;for(UINT i=0;i<state.indirectCount;++i)indirectMasked|=state.indirect[i].masked;
    if((state.cutoutCount||indirectMasked)&&state.cutoutVertices&&state.cutoutVertex&&state.cutoutPixel){
        // Restore resource bindings here; the enclosing shadow renderer already
        // owns/restores shader, layout, vertex-buffer and transform bindings.
        ComPtr<ID3D11ShaderResourceView> oldTexture;ComPtr<ID3D11SamplerState> oldSampler;
        ComPtr<ID3D11Buffer> oldConstants[2];ctx->PSGetShaderResources(12,1,&oldTexture);ctx->PSGetSamplers(0,1,&oldSampler);
        for(UINT i=0;i<2;++i)ctx->PSGetConstantBuffers(i,1,&oldConstants[i]);
        ctx->IASetInputLayout(state.cutoutLayout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);
        auto vertices=state.cutoutVertices.Get();UINT stride=20,offset=0;ctx->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
        ctx->VSSetShader(state.cutoutVertex.Get(),nullptr,0);ctx->PSSetShader(state.cutoutPixel.Get(),nullptr,0);
        auto matrix=state.coverageConstants.Get();ctx->UpdateSubresource(matrix,0,nullptr,worldToShadow.data(),0,0);ctx->VSSetConstantBuffers(3,1,&matrix);
        for(UINT i=0;i<state.cutoutBatchCount;++i){const auto& b=state.cutouts[i];
            auto texture=b.opacity.Get();auto sampler=b.sampler.Get();ctx->PSSetShaderResources(12,1,&texture);ctx->PSSetSamplers(0,1,&sampler);
            ID3D11Buffer* constants[]={b.constants[0].Get(),b.constants[1].Get()};ctx->PSSetConstantBuffers(0,2,constants);ctx->Draw(b.count,b.first);
        }
        for(UINT i=0;i<state.indirectCount;++i){const auto& batch=state.indirect[i];if(!batch.masked)continue;
            const auto& b=batch.coverage;auto texture=b.opacity.Get();auto sampler=b.sampler.Get();
            ctx->PSSetShaderResources(12,1,&texture);ctx->PSSetSamplers(0,1,&sampler);
            ID3D11Buffer* constants[]={b.constants[0].Get(),b.constants[1].Get()};ctx->PSSetConstantBuffers(0,2,constants);
            auto buffer=batch.vertices.Get();ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
            ctx->DrawInstancedIndirect(batch.metadata.Get(),0);
        }
        auto texture=oldTexture.Get();auto sampler=oldSampler.Get();ctx->PSSetShaderResources(12,1,&texture);ctx->PSSetSamplers(0,1,&sampler);
        ID3D11Buffer* constants[]={oldConstants[0].Get(),oldConstants[1].Get()};ctx->PSSetConstantBuffers(0,2,constants);
    }
    if(!state.coverageCount||!state.coverageVertices||!state.coverageVertex||!state.coveragePixel)return;
    ctx->IASetInputLayout(state.coverageLayout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);
    auto vertices=state.coverageVertices.Get();UINT stride=16,offset=0;ctx->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    ctx->VSSetShader(state.coverageVertex.Get(),nullptr,0);ctx->PSSetShader(state.coveragePixel.Get(),nullptr,0);
    auto constants=state.coverageConstants.Get();ctx->UpdateSubresource(constants,0,nullptr,worldToShadow.data(),0,0);
    ctx->VSSetConstantBuffers(3,1,&constants);ctx->Draw(state.coverageCount,0);
}
void DrawAnimatedCasters(ID3D11DeviceContext* ctx,AnimatedCasterSubset subset){
    if(!ctx)return;
    UINT stride=12,offset=0;ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);
    if(subset==AnimatedCasterSubset::Viewmodel){
        const UINT count=state.viewmodelCount?state.viewmodelCount:state.publishedViewmodelCount;
        auto buffer=state.viewmodelCount?state.viewmodelVertices.Get():state.publishedViewmodelVertices.Get();
        if(count&&buffer){ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);ctx->Draw(count,0);}return;
    }
    // This frame's capture when it already exists, otherwise the last complete frame's.
    const UINT characters=state.count?state.count:state.publishedCount,weapons=state.weaponCount?state.weaponCount:state.publishedWeaponCount;
    ID3D11Buffer* characterBuffer=state.count?state.vertices.Get():state.publishedVertices.Get();
    ID3D11Buffer* weaponBuffer=state.weaponCount?state.weaponVertices.Get():state.publishedWeaponVertices.Get();
    if((subset==AnimatedCasterSubset::All||subset==AnimatedCasterSubset::Characters||subset==AnimatedCasterSubset::Actors)&&characters&&characterBuffer){
        ctx->IASetVertexBuffers(0,1,&characterBuffer,&stride,&offset);ctx->Draw(characters,0);
    }
    if((subset==AnimatedCasterSubset::All||subset==AnimatedCasterSubset::Weapons||subset==AnimatedCasterSubset::Actors)&&weapons&&weaponBuffer){
        ctx->IASetVertexBuffers(0,1,&weaponBuffer,&stride,&offset);ctx->Draw(weapons,0);
    }
    const UINT scene=AvailableSceneCount();
    if((subset==AnimatedCasterSubset::All||subset==AnimatedCasterSubset::Scene)&&scene){
        auto buffer=state.sceneCount?state.sceneVertices.Get():state.publishedSceneVertices.Get();
        ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);ctx->Draw(scene,0);
    }
    if(subset==AnimatedCasterSubset::All||subset==AnimatedCasterSubset::Scene)
        for(UINT i=0;i<state.indirectCount;++i){const auto& batch=state.indirect[i];if(batch.masked)continue;
            auto buffer=batch.vertices.Get();ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);ctx->DrawInstancedIndirect(batch.metadata.Get(),0);}
}
}
