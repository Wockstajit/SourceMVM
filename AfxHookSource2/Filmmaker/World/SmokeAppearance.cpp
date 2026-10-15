#define NOMINMAX
#include "SmokeAppearance.h"
#include "../Environment/OwnedInterface.h"
#include "SmokeVolumeMath.h"
#include "../Environment/EnvironmentScene.h"
#include <future>
#include <unordered_map>
#include <chrono>
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdlib>
#include <cerrno>

namespace Filmmaker::SmokeAppearance {
using Microsoft::WRL::ComPtr;
namespace {
const GUID VolumeDataTag={0xd133fe41,0x4873,0x4172,{0xa8,0xca,0x39,0x45,0x2e,0xa1,0x27,0x72}};
const GUID CoverageStateTag={0xd133fe44,0x4873,0x4172,{0xa8,0xca,0x39,0x45,0x2e,0xa1,0x27,0x72}};
const GUID AppearanceTag={0x86ac19f3,0x1657,0x43be,{0xb9,0xd4,0x51,0xac,0x18,0x09,0x63,0x5e}};
const GUID DiagnosticTag={0xa23f0770,0x4573,0x4b31,{0xa4,0xc1,0x22,0x04,0x11,0xc8,0x70,0x22}};
const GUID LoggedTag={0xa23f0772,0x4573,0x4b31,{0xa4,0xc1,0x22,0x04,0x11,0xc8,0x70,0x22}};
std::mutex mutex;Parameters parameters;std::atomic<bool> active{false};
std::atomic<unsigned> pixels{0},computes{0};std::atomic<unsigned long long> draws{0},dispatches{0},revision{1};
std::atomic<unsigned long long> frameSerial{1};
thread_local bool creating=false;
struct GrowthResult {
    std::shared_ptr<const Environment::OcclusionMesh> mesh;
    std::shared_ptr<Environment::SurfaceRayGrid> grid;
    std::vector<GrowthVolume> volumes;
    std::vector<WarpCell> atlas;
    double seconds=0;
};
struct Cache {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11Buffer> buffer;
    unsigned long long revision=0,growthFrame=0,settingsFrame=0,frameRevision=0;unsigned collisionMask=0;
    State frameSettings;
    ComPtr<ID3D11DepthStencilState> smokeDepth,smokeDepthSource;
    ComPtr<ID3D11ShaderResourceView> warp;ComPtr<ID3D11SamplerState> warpSampler;
    std::future<GrowthResult> pending;GrowthResult built;
    std::shared_ptr<const Environment::OcclusionMesh> wantedMesh;
    std::vector<GrowthVolume> wanted;
};
struct Mapping {void* data=nullptr;UINT bytes=0;};
thread_local std::unordered_map<ID3D11Resource*,Mapping> mapped;
UINT VolumeBufferBytes(ID3D11Resource* resource){
    if(!resource)return 0;D3D11_RESOURCE_DIMENSION type;resource->GetType(&type);if(type!=D3D11_RESOURCE_DIMENSION_BUFFER)return 0;
    D3D11_BUFFER_DESC d{};static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    return (d.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&(d.ByteWidth==1024||d.ByteWidth==2496)?d.ByteWidth:0;
}
thread_local Cache cache;
unsigned PrepareGrowth(ID3D11DeviceContext* ctx,ID3D11Device* device,ID3D11DeviceChild* shader,bool compute,const Parameters& p){
    // Publish once at the frame boundary so opacity and colour cannot use different atlases.
    const auto frame=frameSerial.load(std::memory_order_relaxed);const bool publish=cache.growthFrame!=frame;cache.growthFrame=frame;
    if(publish&&cache.pending.valid()&&cache.pending.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        try{auto completed=cache.pending.get();
            if(!completed.atlas.empty()){
                D3D11_TEXTURE3D_DESC d{};d.Width=WarpWidth;d.Height=d.Depth=WarpSide;d.MipLevels=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
                d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{completed.atlas.data(),WarpWidth*sizeof(WarpCell),WarpWidth*WarpSide*sizeof(WarpCell)};
                ComPtr<ID3D11Texture3D> texture;ComPtr<ID3D11ShaderResourceView> view;
                HRESULT hr=device->CreateTexture3D(&d,&initial,&texture);
                if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(texture.Get(),nullptr,&view);
                MvmDebugLog_LinefAlways("smoke.volume.growth","ready volumes=%zu seconds=%.3f hr=%08x",completed.volumes.size(),completed.seconds,unsigned(hr));
                if(SUCCEEDED(hr)){completed.atlas.clear();completed.atlas.shrink_to_fit();cache.warp=std::move(view);cache.built=std::move(completed);}
            }
        }catch(const std::exception& e){MvmDebugLog_LinefAlways("smoke.volume.growth","failed=%s",e.what());}
    }
    if(p.spread<=1.f)return false;
    auto mesh=Environment::CollisionGeometrySnapshot();
    if(!mesh){cache.wantedMesh.reset();cache.wanted.clear();return false;}
    if(cache.wantedMesh!=mesh){cache.wanted.clear();cache.wantedMesh=mesh;}
    ProgramInfo info{};UINT bytes=sizeof(info);
    if(shader&&SUCCEEDED(shader->GetPrivateData(DiagnosticTag,&bytes,&info))&&info.samples&&info.table<14){
        ComPtr<ID3D11Buffer> volume;if(compute)ctx->CSGetConstantBuffers(info.table,1,&volume);else ctx->PSGetConstantBuffers(info.table,1,&volume);
        std::array<float,624> data{};UINT length=sizeof(data);
        if(volume&&SUCCEEDED(volume->GetPrivateData(VolumeDataTag,&length,data.data()))&&(length==1024||length==2496)){
            std::vector<GrowthVolume> wanted;
            for(unsigned i=0;i<16;++i){const auto low=&data[i*4],high=&data[(i+16)*4],center=&data[(i+32)*4];const float slotValue=data[(i+48)*4+2];
                if(!(high[0]>low[0]&&high[1]>low[1]&&high[2]>low[2])||!std::isfinite(slotValue)||slotValue<0||slotValue>=16)continue;
                Vec3 origin{center[0],center[1],center[2]};if(!Environment::Finite(origin))continue;
                wanted.push_back({origin,p.spread,unsigned(slotValue)});
            }
            // Visibility/order changes must not evict other grenades from the growth cache.
            MergeGrowthVolumes(cache.wanted,wanted);
        }
    }
    if(cache.wanted.empty())return 0;
    for(auto& volume:cache.wanted)volume.spread=p.spread;
    // Reuse each completed grenade independently. A new/camera-culled smoke cannot
    // drop every other slot to native scale while its collision field is prepared.
    const unsigned readyMask=cache.warp&&cache.built.mesh==mesh?GrowthReadyMask(cache.built.volumes,cache.wanted):0;
    if(readyMask!=GrowthWantedMask(cache.wanted)&&!cache.pending.valid()){
        const auto volumes=cache.wanted;auto grid=cache.built.mesh==mesh?cache.built.grid:nullptr;
        cache.pending=std::async(std::launch::async,[mesh,volumes,grid](){
            const auto start=std::chrono::steady_clock::now();GrowthResult result;result.mesh=mesh;result.volumes=volumes;result.grid=grid;
            if(!result.grid){result.grid=std::make_shared<Environment::SurfaceRayGrid>();if(!Environment::BuildSurfaceRayGrid(*mesh,*result.grid))return result;}
            IdentityWarp(result.atlas);for(const auto& volume:volumes)StoreWarp(result.atlas,volume.slot,GrowVolume(*mesh,*result.grid,volume));
            result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();return result;
        });
        MvmDebugLog_LinefAlways("smoke.volume.growth","queued volumes=%zu spread=%.3f",volumes.size(),p.spread);
        for(const auto& v:volumes)MvmDebugLog_LinefAlways("smoke.volume.source","slot=%u center=%.2f,%.2f,%.2f",v.slot,v.center.x,v.center.y,v.center.z);
    }
    if(readyMask&&!cache.warpSampler){D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&d,&cache.warpSampler);}
    return cache.warpSampler?readyMask:0;
}

template<class Shader> void Note(Shader* shader,const void* code,size_t bytes,bool compute) {
    if(!shader||creating)return;
    std::vector<uint8_t> patched;unsigned reads=0;ProgramInfo info;
    const bool supported=Program(code,bytes,patched,&reads,&info);
    if(info.smoke){shader->SetPrivateData(DiagnosticTag,sizeof(info),&info);}
    if(!supported)return;
    ComPtr<ID3D11Device> device;shader->GetDevice(&device);ComPtr<Shader> replacement;
    creating=true;
    HRESULT hr;
    if constexpr(std::is_same_v<Shader,ID3D11PixelShader>)hr=device->CreatePixelShader(patched.data(),patched.size(),nullptr,&replacement);
    else if constexpr(std::is_same_v<Shader,ID3D11VertexShader>)hr=device->CreateVertexShader(patched.data(),patched.size(),nullptr,&replacement);
    else hr=device->CreateComputeShader(patched.data(),patched.size(),nullptr,&replacement);
    creating=false;
    if(SUCCEEDED(hr)){::Filmmaker::SetOwnedInterface(::Filmmaker::RawOwner(shader),AppearanceTag,replacement.Get());if(compute)++computes;else ++pixels;}
    MvmDebugLog_LinefAlways("smoke.appearance.shader","stage=%s reads=%u geometry=%u samples=%u hr=%08x bytes=%zu",compute?"CS":std::is_same_v<Shader,ID3D11VertexShader>?"VS":"PS",reads,info.geometry,info.samples,unsigned(hr),bytes);
}
}
void NoteMap(ID3D11Resource* resource,D3D11_MAP mode,void* data){
    if(!data||(mode!=D3D11_MAP_WRITE_DISCARD&&mode!=D3D11_MAP_WRITE&&mode!=D3D11_MAP_WRITE_NO_OVERWRITE))return;
    const UINT bytes=VolumeBufferBytes(resource);if(bytes)mapped[resource]={data,bytes};
}
void NoteUnmap(ID3D11Resource* resource){auto item=mapped.find(resource);if(item==mapped.end())return;
    resource->SetPrivateData(VolumeDataTag,item->second.bytes,item->second.data);mapped.erase(item);
}
void NoteUpdate(ID3D11Resource* resource,const D3D11_BOX* box,const void* data){
    if(!data)return;const UINT bytes=VolumeBufferBytes(resource);
    if(!bytes)return;
    if(!box){resource->SetPrivateData(VolumeDataTag,bytes,data);return;}
    std::array<unsigned char,2496> captured{};UINT size=bytes;
    if(box->left<box->right&&box->right<=bytes&&SUCCEEDED(resource->GetPrivateData(VolumeDataTag,&size,captured.data()))&&size==bytes){
        std::memcpy(captured.data()+box->left,data,box->right-box->left);
        resource->SetPrivateData(VolumeDataTag,bytes,captured.data());
    }else resource->SetPrivateData(VolumeDataTag,0,nullptr);
}
void NoteCopy(ID3D11Resource* destination,ID3D11Resource* source,UINT offset,const D3D11_BOX* box){
    const UINT bytes=VolumeBufferBytes(destination),sourceBytes=VolumeBufferBytes(source);if(!bytes)return;
    std::array<unsigned char,2496> from{},to{};UINT size=sourceBytes;
    const UINT left=box?box->left:0,right=box?box->right:sourceBytes;
    if(!sourceBytes||left>=right||right>sourceBytes||offset>bytes||right-left>bytes-offset
        ||FAILED(source->GetPrivateData(VolumeDataTag,&size,from.data()))||size!=sourceBytes){destination->SetPrivateData(VolumeDataTag,0,nullptr);return;}
    if(offset||right-left!=bytes){size=bytes;
        if(FAILED(destination->GetPrivateData(VolumeDataTag,&size,to.data()))||size!=bytes){destination->SetPrivateData(VolumeDataTag,0,nullptr);return;}}
    std::memcpy(to.data()+offset,from.data()+left,right-left);destination->SetPrivateData(VolumeDataTag,bytes,to.data());
}
State Snapshot(){State result;{std::lock_guard<std::mutex> lock(mutex);result.enabled=active.load();result.parameters=parameters;}
    result.pixelPrograms=pixels.load();result.computePrograms=computes.load();result.draws=draws.load();result.dispatches=dispatches.load();return result;}
void Configure(bool enabled,const Parameters& p){std::lock_guard<std::mutex> lock(mutex);
    Parameters clean;for(const auto& c:Controls)Set(clean,c,p.*(c.member));parameters=clean;active.store(enabled);++revision;}
void Reset(){Configure(false,Parameters{});}
void NextFrame(){frameSerial.fetch_add(1,std::memory_order_relaxed);}
bool CreatingShader(){return creating;}
void NotePixelShader(ID3D11PixelShader* shader,const void* code,size_t bytes){Note(shader,code,bytes,false);}
void NoteComputeShader(ID3D11ComputeShader* shader,const void* code,size_t bytes){Note(shader,code,bytes,true);}
void NoteVertexShader(ID3D11VertexShader* shader,const void* code,size_t bytes){Note(shader,code,bytes,false);}
void AppendStateJson(std::string& json){const auto s=Snapshot();std::ostringstream o;
    o<<",\"smokeAppearance\":"<<(s.enabled?"true":"false");
    for(const auto& c:Controls)o<<",\"smoke_"<<c.name<<"\":"<<s.parameters.*(c.member);
    o<<",\"smoke_voxel_size\":"<<VoxelSize(s.parameters)<<",\"smoke_voxel_count\":"<<VoxelCount(s.parameters);
    o<<",\"smokeAppearanceReady\":"<<((s.pixelPrograms+s.computePrograms)>0?"true":"false");json+=o.str();}
void RunCommand(int argc,advancedfx::ICommandArgs* args){const char* key=argc>4?args->ArgV(4):"";auto s=Snapshot();
    if(!_stricmp(key,"reset")){Reset();advancedfx::Message("Smoke appearance restored to native.\n");return;}
    if(!_stricmp(key,"on")||!_stricmp(key,"off")||!_stricmp(key,"toggle")){
        Configure(!_stricmp(key,"toggle")?!s.enabled:!_stricmp(key,"on"),s.parameters);return;}
    if(!_stricmp(key,"voxel_size")){
        if(argc!=6&&(argc!=7||_stricmp(args->ArgV(6),"quiet"))){advancedfx::Warning("Usage: mirv_filmmaker world smoke appearance voxel_size <world units> [quiet]\n");return;}
        char* end=nullptr;errno=0;const float value=std::strtof(args->ArgV(5),&end);
        if(errno||end==args->ArgV(5)||*end||!SetVoxelSize(s.parameters,value)){advancedfx::Warning("Voxel size requires a positive finite number.\n");return;}
        Configure(true,s.parameters);return;
    }
    for(const auto& c:Controls)if(!_stricmp(key,c.name)){
        if(argc!=6&&(argc!=7||_stricmp(args->ArgV(6),"quiet"))){advancedfx::Warning("Usage: mirv_filmmaker world smoke appearance %s <%.2f..%.2f> [quiet]\n",c.name,c.minimum,c.maximum);return;}
        char* end=nullptr;errno=0;const float value=std::strtof(args->ArgV(5),&end);
        if(errno||end==args->ArgV(5)||*end||!Set(s.parameters,c,value)){advancedfx::Warning("Smoke appearance requires a finite number.\n");return;}
        Configure(true,s.parameters);return;
    }
    advancedfx::Message("Smoke appearance: %s; ready PS=%u CS=%u; applied draws=%llu dispatches=%llu\n",s.enabled?"on":"off",s.pixelPrograms,s.computePrograms,s.draws,s.dispatches);
    for(const auto& c:Controls)advancedfx::Message("  %s = %.3f [%.2f..%.2f]\n",c.name,s.parameters.*(c.member),c.minimum,c.maximum);
    advancedfx::Message("  voxel_size = %.3f world units; rendered grid = %u cells\n",VoxelSize(s.parameters),VoxelCount(s.parameters));
    advancedfx::Message("Usage: mirv_filmmaker world smoke appearance on|off|toggle|reset|<parameter> <value>\n");
}
Scope::Scope(ID3D11DeviceContext* ctx,bool enabled,bool compute):computeStage(compute){
    if(!enabled||!active.load(std::memory_order_relaxed)||!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    ComPtr<ID3D11DeviceChild> shader;UINT count=0;
    if(compute){ID3D11ComputeShader* p=nullptr;ctx->CSGetShader(&p,nullptr,&count);shader.Attach(p);}
    else{ID3D11PixelShader* p=nullptr;ctx->PSGetShader(&p,nullptr,&count);shader.Attach(p);}
    if(count)return;
    if(shader&&MvmDebugLog_Active()){
        ProgramInfo info;UINT size=sizeof(info),logged=0,loggedBytes=sizeof(logged);
        shader->GetPrivateData(LoggedTag,&loggedBytes,&logged);
        if(!logged&&SUCCEEDED(shader->GetPrivateData(DiagnosticTag,&size,&info))){
            logged=1;shader->SetPrivateData(LoggedTag,sizeof(logged),&logged);
            for(unsigned slot=1;slot<=15;++slot){ComPtr<ID3D11ShaderResourceView> view;if(compute)ctx->CSGetShaderResources(slot,1,&view);else ctx->PSGetShaderResources(slot,1,&view);
                if(!view)continue;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};view->GetDesc(&sd);if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE3D)continue;
                ComPtr<ID3D11Resource> resource;view->GetResource(&resource);D3D11_TEXTURE3D_DESC td{};static_cast<ID3D11Texture3D*>(resource.Get())->GetDesc(&td);
                MvmDebugLog_LinefAlways("smoke.volume.texture","shader=%p slot=%u dimensions=%ux%ux%u format=%u",shader.Get(),slot,td.Width,td.Height,td.Depth,unsigned(sd.Format));
            }
            MvmDebugLog_LinefAlways("smoke.appearance.binding","stage=%s shader=%p controls=%x reads=%u table=%u samples=%u geometry=%u",compute?"CS":"PS",shader.Get(),info.controls,info.reads,info.table,info.samples,info.geometry);
        }
    }
    ComPtr<ID3D11DeviceChild> replacement;UINT bytes=sizeof(ID3D11DeviceChild*);
    if(shader)::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(shader),AppearanceTag,&bytes,(void**)replacement.GetAddressOf());
    ComPtr<ID3D11VertexShader> vertex,vertexReplacement;
    if(!compute){UINT instances=0;ctx->VSGetShader(&vertex,nullptr,&instances);
        if(vertex&&!instances){UINT n=sizeof(ID3D11VertexShader*);::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(vertex),AppearanceTag,&n,(void**)vertexReplacement.GetAddressOf());}}
    ProgramInfo currentInfo{};UINT infoBytes=sizeof(currentInfo);

    if(shader)shader->GetPrivateData(DiagnosticTag,&infoBytes,&currentInfo);
    if(!replacement&&!vertexReplacement&&!currentInfo.composite)return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(cache.device.Get()!=device.Get()){cache=Cache{};cache.device=device;}
    const auto frame=frameSerial.load(std::memory_order_relaxed);
    if(cache.settingsFrame!=frame){
        // Read both under the configuration lock; an edit lands on the next complete frame.
        std::lock_guard<std::mutex> lock(mutex);cache.frameSettings.enabled=active.load();
        cache.frameSettings.parameters=parameters;cache.frameRevision=revision.load();cache.settingsFrame=frame;
    }
    const auto version=cache.frameRevision;const auto& s=cache.frameSettings;if(!s.enabled)return;
    const unsigned collisionMask=PrepareGrowth(ctx,device.Get(),shader.Get(),compute,s.parameters);
    if(cache.revision!=version||cache.collisionMask!=collisionMask){
        // The user owns scale/appearance on every pass, including initial bloom.
        // Unprepared slots use custom resampling until their map lookup is ready.
        auto constants=Constants(s.parameters);constants[42]=collisionMask?1.f:0.f;constants[43]=float(collisionMask);
        if(!cache.buffer){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(constants);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.Usage=D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA initial{constants.data(),0,0};if(FAILED(device->CreateBuffer(&d,&initial,&cache.buffer)))return;}
        else ctx->UpdateSubresource(cache.buffer.Get(),0,nullptr,constants.data(),0,0);
        cache.revision=version;cache.collisionMask=collisionMask;
        MvmDebugLog_LinefAlways("smoke.appearance.apply","stage=%s revision=%llu slot=%u shader=%p buffer=%p density=%.3f wispy=%.3f speed=%.3f scaleLow=%.3f scaleHigh=%.3f low=%.3f high=%.3f noise=%.3f boil=%.3f rayStep=%.3f",
            compute?"CS":"PS",version,ConstantSlot,shader.Get(),cache.buffer.Get(),s.parameters.density,s.parameters.wispy,s.parameters.speed,s.parameters.scaleLow,s.parameters.scaleHigh,s.parameters.strengthLow,s.parameters.strengthHigh,s.parameters.noise,s.parameters.boil,s.parameters.rayStep);
        MvmDebugLog_LinefAlways("smoke.volume.apply","revision=%llu spread=%.3f resolution=%.0f voxelSize=%.3f cells=%u collision=%u effectiveSpread=%.3f",version,s.parameters.spread,s.parameters.resolution,VoxelSize(s.parameters),VoxelCount(s.parameters),collisionMask,s.parameters.spread);
    }
    context=ctx;auto* buffer=cache.buffer.Get();
    if(s.parameters.spread>1.f&&!compute){
        ComPtr<ID3D11RasterizerState> rasterizer;ctx->RSGetState(&rasterizer);D3D11_RASTERIZER_DESC desc{};
        if(rasterizer){rasterizer->GetDesc(&desc);if(desc.ScissorEnable){
            UINT count=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&count,&viewport);
            if(count==1){originalScissorCount=16;ctx->RSGetScissorRects(&originalScissorCount,originalScissors);
                const D3D11_RECT expanded{LONG(viewport.TopLeftX),LONG(viewport.TopLeftY),LONG(std::ceil(viewport.TopLeftX+viewport.Width)),LONG(std::ceil(viewport.TopLeftY+viewport.Height))};
                ctx->RSSetScissorRects(1,&expanded);scissorsChanged=true;
            }
        }}
    }
    // Native stencil coverage is still produced from the original simulated cloud.
    // The recognized ray passes already reject empty rays and test scene depth.
    if(s.parameters.spread>1.f&&!compute&&vertex&&(currentInfo.reads||currentInfo.composite)){
        char name[128]{};UINT nameBytes=127;
        if(SUCCEEDED(vertex->GetPrivateData(WKPDID_D3DDebugObjectName,&nameBytes,name))&&!strcmp(name,"smoke_volume.vfx_vs")){
            ComPtr<ID3D11DepthStencilState> native;UINT reference=0;ctx->OMGetDepthStencilState(&native,&reference);
            D3D11_DEPTH_STENCIL_DESC desc{};if(native)native->GetDesc(&desc);
            if(desc.StencilEnable){
                if(!cache.smokeDepth||cache.smokeDepthSource.Get()!=native.Get()){cache.smokeDepth.Reset();cache.smokeDepthSource=native;desc.StencilEnable=FALSE;device->CreateDepthStencilState(&desc,&cache.smokeDepth);}
                if(cache.smokeDepth){originalDepth=native.Detach();originalStencilReference=reference;ctx->OMSetDepthStencilState(cache.smokeDepth.Get(),reference);depthChanged=true;
                    if(MvmDebugLog_Active()){UINT once=0,n=sizeof(once);shader->GetPrivateData(CoverageStateTag,&n,&once);if(!once){once=1;shader->SetPrivateData(CoverageStateTag,sizeof(once),&once);
                        MvmDebugLog_LinefAlways("smoke.volume.coverage","shader=%p composite=%u spread=%.3f nativeStencil=%u depth=%u",shader.Get(),unsigned(currentInfo.composite),s.parameters.spread,reference,desc.DepthEnable);}}}
            }
        }
    }
    if(vertexReplacement){originalVertex=vertex.Detach();ctx->VSGetConstantBuffers(ConstantSlot,1,&originalVertexBuffer);
        ctx->VSSetConstantBuffers(ConstantSlot,1,&buffer);ctx->VSSetShader(vertexReplacement.Get(),nullptr,0);}
    if(!replacement)return;
    original=shader.Detach();
    if(collisionMask){auto* view=cache.warp.Get();auto* sampler=cache.warpSampler.Get();warpBound=true;
        if(compute){ctx->CSGetShaderResources(WarpTextureSlot,1,&originalWarp);ctx->CSGetSamplers(WarpSamplerSlot,1,&originalWarpSampler);
            ctx->CSSetShaderResources(WarpTextureSlot,1,&view);ctx->CSSetSamplers(WarpSamplerSlot,1,&sampler);}
        else{ctx->PSGetShaderResources(WarpTextureSlot,1,&originalWarp);ctx->PSGetSamplers(WarpSamplerSlot,1,&originalWarpSampler);
            ctx->PSSetShaderResources(WarpTextureSlot,1,&view);ctx->PSSetSamplers(WarpSamplerSlot,1,&sampler);}
    }
    if(compute){ctx->CSGetConstantBuffers(ConstantSlot,1,&originalBuffer);ctx->CSSetConstantBuffers(ConstantSlot,1,&buffer);
        ctx->CSSetShader(static_cast<ID3D11ComputeShader*>(replacement.Get()),nullptr,0);++dispatches;}
    else{ctx->PSGetConstantBuffers(ConstantSlot,1,&originalBuffer);ctx->PSSetConstantBuffers(ConstantSlot,1,&buffer);
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(replacement.Get()),nullptr,0);++draws;}
}
Scope::~Scope(){if(!context)return;
    if(depthChanged){context->OMSetDepthStencilState(originalDepth,originalStencilReference);if(originalDepth)originalDepth->Release();}
    if(scissorsChanged)context->RSSetScissorRects(originalScissorCount,originalScissors);
    if(warpBound){
        if(computeStage){context->CSSetShaderResources(WarpTextureSlot,1,&originalWarp);context->CSSetSamplers(WarpSamplerSlot,1,&originalWarpSampler);}
        else{context->PSSetShaderResources(WarpTextureSlot,1,&originalWarp);context->PSSetSamplers(WarpSamplerSlot,1,&originalWarpSampler);}
        if(originalWarp)originalWarp->Release();if(originalWarpSampler)originalWarpSampler->Release();
    }
    if(original){if(computeStage){context->CSSetShader(static_cast<ID3D11ComputeShader*>(original),nullptr,0);context->CSSetConstantBuffers(ConstantSlot,1,&originalBuffer);}
        else{context->PSSetShader(static_cast<ID3D11PixelShader*>(original),nullptr,0);context->PSSetConstantBuffers(ConstantSlot,1,&originalBuffer);}
        original->Release();if(originalBuffer)originalBuffer->Release();}
    if(originalVertex){context->VSSetShader(originalVertex,nullptr,0);context->VSSetConstantBuffers(ConstantSlot,1,&originalVertexBuffer);
        originalVertex->Release();if(originalVertexBuffer)originalVertexBuffer->Release();}
}
}
