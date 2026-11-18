#define NOMINMAX
#include "EnvironmentNativeMaterialDetail.h"
// Deriving the owned variants of CS2's lit material shaders, on worker threads.
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
using namespace NativeDetail;
namespace {
// Set while this thread creates one of our derived programs: that creation passes through the
// CreatePixelShader detour again and must not be derived from.
thread_local bool creatingCoverage=false;
bool NativeSmokeAbi(const NativeSunAbiInfo& info){return info.cb3Count==1990&&!info.lightSlot&&info.flags==31;}
// The lighting variants of one CS2 material program: atmosphere, dynamic irradiance, shared AO and
// lighting, sparse cluster, coverage and smoke. Runs on the derivation worker below.
void DeriveNativeMaterialShaders(ID3D11PixelShader* shader,const void* code,size_t bytes,const bool sunAbi,NativeSunAbiInfo info){
    auto shareLighting=[&](ID3D11Device* device,ID3D11PixelShader* original,const std::vector<uint8_t>& program){
        auto create=[&](const std::vector<uint8_t>& candidate,ComPtr<ID3D11PixelShader>& shared){
            std::vector<uint8_t> sharedCode;NativeSharedLightingInfo layout;
            if(!NativeSharedLightingProgram(candidate.data(),candidate.size(),sharedCode,layout))return false;
            creatingCoverage=true;const auto hr=device->CreatePixelShader(sharedCode.data(),sharedCode.size(),nullptr,&shared);creatingCoverage=false;
            if(FAILED(hr))return false;shared->SetPrivateData(SharedLightingInfoTag,sizeof(layout),&layout);return true;
        };
        ComPtr<ID3D11PixelShader> shared;
        if(create(program,shared))::Filmmaker::SetOwnedInterface(original,SharedLightingShaderTag,shared.Get());
        std::vector<uint8_t> sparseCode;
        if(NativeSparseClusterProgram(program.data(),program.size(),sparseCode)){
            shared.Reset();if(create(sparseCode,shared)){
                const UINT enabled=1;shared->SetPrivateData(SparseClusterInfoTag,sizeof(enabled),&enabled);
                ::Filmmaker::SetOwnedInterface(original,SparseClusterShaderTag,shared.Get());
            }
        }
    };
    auto shareAo=[&](ID3D11Device* device,ID3D11PixelShader* original,const std::vector<uint8_t>& program){
        std::vector<uint8_t> sharedCode;
        bool unchanged=false;
        if(!NativeSharedAoProgram(program.data(),program.size(),sharedCode,&unchanged)){
            if(unchanged){const UINT valid=1;original->SetPrivateData(UnchangedAoViewTag,sizeof(valid),&valid);}
            shareLighting(device,original,program);return;
        }
        ComPtr<ID3D11PixelShader> shared;creatingCoverage=true;
        const auto hr=device->CreatePixelShader(sharedCode.data(),sharedCode.size(),nullptr,&shared);creatingCoverage=false;
        if(SUCCEEDED(hr)){shareLighting(device,shared.Get(),sharedCode);::Filmmaker::SetOwnedInterface(original,SharedAoShaderTag,shared.Get());}
    };
    if(sunAbi||info.lightSlot){
        std::vector<uint8_t> atmosphereCode;
        bool ownedCascades=false;uint32_t screenShadowSlot=0;
        bool lampCube=false;
        const bool atmosphere=NativeAtmosphereProgram(code,bytes,atmosphereCode,true,sunAbi?&ownedCascades:nullptr,&screenShadowSlot,&lampCube);
        // One owned program that cannot read cube shadow records keeps every light on face records.
        if(atmosphere&&!lampCube&&!g_cubeShadowUnsupported.exchange(true))
            MvmDebugLog_LinefAlways("relight.cube.unsupported","hash=%016llx: lights stay six face records",info.programHash);
        if(atmosphere){
            ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> owned;
            creatingCoverage=true;const auto hr=device->CreatePixelShader(atmosphereCode.data(),atmosphereCode.size(),nullptr,&owned);creatingCoverage=false;
            if(SUCCEEDED(hr)){shareAo(device.Get(),owned.Get(),atmosphereCode);::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),NativeAtmosphereTag,owned.Get());info.ownedCascades=ownedCascades;info.screenShadowSlot=screenShadowSlot;}
            MvmDebugLog_LinefAlways("relight.atmosphere.shader","hash=%016llx valid=%d hr=%08x",info.programHash,SUCCEEDED(hr),unsigned(hr));
        }
        if((info.lightmapArrayMask&2)||info.probeSlot){
            std::vector<uint8_t> dynamicCode;Relight::IrradianceProgramInfo query;
            if(Relight::DynamicIrradianceProgram(atmosphere?atmosphereCode.data():code,
                atmosphere?atmosphereCode.size():bytes,dynamicCode,&query,info.probeSlot)){
                ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> dynamic;
                creatingCoverage=true;
                const auto hr=device->CreatePixelShader(dynamicCode.data(),dynamicCode.size(),nullptr,&dynamic);
                creatingCoverage=false;
                if(SUCCEEDED(hr)){
                    info.ownedCascades=ownedCascades;info.screenShadowSlot=screenShadowSlot;
                    shareAo(device.Get(),dynamic.Get(),dynamicCode);
                    dynamic->SetPrivateData(SunAbiInfoTag,sizeof(info),&info);
                    ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),DynamicIrradianceTag,dynamic.Get());
                }
                MvmDebugLog_LinefAlways("relight.irradiance.shader","hash=%016llx queries=%u evaluations=%u position=%u normal=%u valid=%d hr=%08x",
                    info.programHash,query.queries,query.evaluations,query.position,query.normal,SUCCEEDED(hr),unsigned(hr));
            }else MvmDebugLog_LinefAlways("relight.irradiance.shader","hash=%016llx unsupported=1 bakedFallback=0",info.programHash);
        }
        std::vector<uint8_t> coverageCode;
        if(NativeCoverageProgram(code,bytes,coverageCode)){
            ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> coverage;
            creatingCoverage=true;
            const auto hr=device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coverage);
            creatingCoverage=false;
            if(SUCCEEDED(hr)){
                ::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),CoverageShaderTag,coverage.Get());
                const UINT valid=1;shader->SetPrivateData(SunAbiTag,sizeof(valid),&valid);
            }
            if(NativeCoverageProgram(code,bytes,coverageCode,true)){
                coverage.Reset();creatingCoverage=true;
                const auto blackHr=device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coverage);
                creatingCoverage=false;
                if(SUCCEEDED(blackHr))::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),BlackCoverageShaderTag,coverage.Get());
            }
        }
    }
    shader->SetPrivateData(SunAbiInfoTag,sizeof(info),&info);
    if(NativeSmokeAbi(info)){
        // One compile serves every smoke program; D3DCompile per program cost up to hundreds of ms
        // each. Never released, so nothing calls into the compiler module during process exit.
        static std::once_flag compiled;static ID3DBlob* kernel=nullptr;static ID3DBlob* error=nullptr;
        std::call_once(compiled,[]{if(FAILED(CompileEnvironmentShader(NativeSmokeLightingShader,sizeof(NativeSmokeLightingShader)-1,
            "EnvironmentNativeSmoke",nullptr,"main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&kernel,&error))&&kernel){kernel->Release();kernel=nullptr;}});
        if(kernel){
            std::vector<uint8_t> patched;unsigned queries=0;
            if(NativeSmokeProgram(code,bytes,kernel->GetBufferPointer(),kernel->GetBufferSize(),patched,&queries)){
                ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> smoke;
                creatingCoverage=true;const auto hr=device->CreatePixelShader(patched.data(),patched.size(),nullptr,&smoke);creatingCoverage=false;
                if(SUCCEEDED(hr))::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),NativeSmokeTag,smoke.Get());
                MvmDebugLog_LinefAlways("environment.smoke.shader","hash=%016llx queries=%u hr=%08x",info.programHash,queries,unsigned(hr));
            }
        }else MvmDebugLog_LinefAlways("environment.smoke.shader","compile=%s",error?static_cast<const char*>(error->GetBufferPointer()):"failed");
    }
    // Development aid: keep the bytecode of every clustered-light program so it can be written out
    // and disassembled (mirv_relight dumpprograms). Capped, and only programs that carry the ABI.
    if(info.lightSlot){
        static std::mutex lock;std::lock_guard<std::mutex> guard(lock);
        static size_t total=0;
        if(programRecord().size()<8192&&total<512u*1024*1024&&!programRecord().count(info.programHash)){
            programRecord()[info.programHash]=std::vector<uint8_t>(static_cast<const uint8_t*>(code),static_cast<const uint8_t*>(code)+bytes);
            total+=bytes;
        }
    }
}

// CS2 creates its shaders on loader threads while the main menu or a map loads, and waits for them.
// Deriving the variants there (up to eight more programs per material, each compiled by the driver)
// cost those threads 19-50 s per load on a 9800X3D, against 3 s for CS2's own 5240 shaders
// (measured 2026-10-15). Player reports from that day show 70 s and 18 s frames at startup and
// sessions that ended at the main menu without a clean exit. The variants are built here instead,
// below normal priority. Until its turn a program carries no SunAbiInfoTag, so its draws take CS2's
// own path, exactly as an unsupported program's do.
//
// The driver's compile of the variants is nearly all the work: about 0.8 s a program when its
// shader cache has none of them (the first launch after the patched code changes), 1,650 programs
// on de_ancient. One worker took over 20 minutes to light the whole map (2026-10-18). Several
// workers compile at once, and a program a lit draw is waiting for jumps the queue
// (PrioritizeNativeMaterial), so what is on screen is lit first.
struct DerivationJob {
    ComPtr<ID3D11PixelShader> shader;
    std::vector<uint8_t> code;
    bool sunAbi=false;
    NativeSunAbiInfo info{};
    bool scaledColor=false; // only the scaled-colour variant (QueueScaledColorVariant)
};
// The scaled-colour variant of an unlit effect program (EnvironmentNativeScaledColorMath.h).
void DeriveScaledColor(ID3D11PixelShader* shader,const std::vector<uint8_t>& code){
    std::vector<uint8_t> scaled;
    const bool patched=NativeScaledColorProgram(code.data(),code.size(),scaled);
    HRESULT hr=E_FAIL;
    if(patched){
        ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<ID3D11PixelShader> variant;
        creatingCoverage=true;hr=device->CreatePixelShader(scaled.data(),scaled.size(),nullptr,&variant);creatingCoverage=false;
        if(SUCCEEDED(hr))::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),ScaledColorShaderTag,variant.Get());
    }
    MvmDebugLog_LinefAlways("environment.skybox.unlit.shader","bytes=%zu patched=%d hr=%08x",code.size(),patched?1:0,unsigned(hr));
}
struct DerivationQueue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<DerivationJob> jobs;
    bool started=false;
    std::atomic<unsigned long long> done{0};
    std::atomic<long long> ticks{0};
};
// Never destroyed: the workers are still parked on it while statics are torn down at exit.
DerivationQueue& Derivations(){static auto* queue=new DerivationQueue();return *queue;}
// Half the logical processors, at most eight: below normal priority they yield to the game.
unsigned DerivationWorkers(){return std::clamp(std::thread::hardware_concurrency()/2,1u,8u);}
void DerivationWorker(){
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    auto& queue=Derivations();
    for(;;){
        DerivationJob job;
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.wake.wait(lock,[&]{return !queue.jobs.empty();});
            job=std::move(queue.jobs.front());queue.jobs.pop_front();
        }
        // Our reference is the last one: CS2 released the program while it waited (a map unloaded).
        job.shader->AddRef();
        if(job.shader->Release()>1){
            LARGE_INTEGER t0,t1;QueryPerformanceCounter(&t0);
            RenderShaderCost_OwnWork(true);
            if(job.scaledColor)DeriveScaledColor(job.shader.Get(),job.code);
            else DeriveNativeMaterialShaders(job.shader.Get(),job.code.data(),job.code.size(),job.sunAbi,job.info);
            RenderShaderCost_OwnWork(false);
            QueryPerformanceCounter(&t1);queue.ticks.fetch_add(t1.QuadPart-t0.QuadPart,std::memory_order_relaxed);
        }
        queue.done.fetch_add(1,std::memory_order_relaxed);
    }
}
}
void NoteNativeMaterialPixelShader(ID3D11PixelShader* shader,const void* code,size_t bytes){
    if(!shader||creatingCoverage)return;NativeSunAbiInfo info{};
    if(NativePostUsesCompressedHdr(code,bytes)){const UINT encoded=1;shader->SetPrivateData(PostCompressionTag,sizeof(encoded),&encoded);nativePostCompressedHdr.store(true,std::memory_order_relaxed);}
    const bool sunAbi=HasNativeSunMaterialAbi(code,bytes,&info);
    if(!sunAbi&&!info.lightSlot&&!NativeSmokeAbi(info)){
        shader->SetPrivateData(SunAbiInfoTag,sizeof(info),&info);
        // The effect family (csgo_effects, sprite cards: a 36- or 39-row cb3, the skybox's and the main
        // view's, and no light slot) keeps its bytecode, on the program itself so it goes with it, for
        // the alpha-blended haze cards at night (QueueScaledColorVariant). Nothing is compiled until
        // such a card is drawn.
        if((info.cb3Count==36||info.cb3Count==39)&&bytes<=256*1024)shader->SetPrivateData(ScaledColorCodeTag,UINT(bytes),code);
        return;
    }
    DerivationJob job;
    job.shader=shader;job.sunAbi=sunAbi;job.info=info;
    job.code.assign(static_cast<const uint8_t*>(code),static_cast<const uint8_t*>(code)+bytes);
    auto& queue=Derivations();
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.jobs.push_back(std::move(job));
        if(!queue.started){queue.started=true;
            for(unsigned i=0;i<DerivationWorkers();++i)std::thread(DerivationWorker).detach();}
    }
    queue.wake.notify_one();
}
void PrioritizeNativeMaterial(ID3D11PixelShader* shader){
    UINT wanted=1,size=sizeof(wanted);
    if(!shader||SUCCEEDED(shader->GetPrivateData(DerivationWantedTag,&size,&wanted)))return;
    shader->SetPrivateData(DerivationWantedTag,sizeof(wanted),&wanted);
    auto& queue=Derivations();
    std::lock_guard<std::mutex> lock(queue.mutex);
    for(auto job=queue.jobs.begin();job!=queue.jobs.end();++job)if(job->shader.Get()==shader){
        if(job!=queue.jobs.begin()){auto moved=std::move(*job);queue.jobs.erase(job);queue.jobs.push_front(std::move(moved));}
        break;
    }
}
bool QueueScaledColorVariant(ID3D11PixelShader* shader){
    UINT wanted=1,size=sizeof(wanted);
    if(!shader)return false;
    if(SUCCEEDED(shader->GetPrivateData(ScaledColorWantedTag,&size,&wanted)))return true;
    UINT bytes=0;
    if(shader->GetPrivateData(ScaledColorCodeTag,&bytes,nullptr)!=S_OK||!bytes)return false;
    DerivationJob job;job.shader=shader;job.scaledColor=true;job.code.resize(bytes);
    if(FAILED(shader->GetPrivateData(ScaledColorCodeTag,&bytes,job.code.data())))return false;
    shader->SetPrivateData(ScaledColorWantedTag,sizeof(wanted),&wanted);
    auto& queue=Derivations();
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.jobs.push_front(std::move(job));
        if(!queue.started){queue.started=true;
            for(unsigned i=0;i<DerivationWorkers();++i)std::thread(DerivationWorker).detach();}
    }
    queue.wake.notify_one();
    return true;
}
NativeMaterialDerivation NativeMaterialDerivationStatus(){
    auto& queue=Derivations();
    NativeMaterialDerivation out;
    {std::lock_guard<std::mutex> lock(queue.mutex);out.queued=queue.jobs.size();}
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    out.done=queue.done.load(std::memory_order_relaxed);
    out.workMs=queue.ticks.load(std::memory_order_relaxed)*1000.0/double(frequency.QuadPart);
    return out;
}
unsigned DumpNativePrograms(const char* directory){
    unsigned written=0;
    for(const auto& entry:programRecord()){
        char path[512];sprintf_s(path,"%s/%016llx.dxbc",directory,static_cast<unsigned long long>(entry.first));
        FILE* file=nullptr;if(fopen_s(&file,path,"wb")||!file)continue;
        fwrite(entry.second.data(),1,entry.second.size(),file);fclose(file);++written;
    }
    return written;
}
} // namespace Filmmaker::Environment
