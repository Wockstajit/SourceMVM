#define NOMINMAX
#include "EnvironmentNativeMaterialDetail.h"
// Binding the owned lighting inputs around each lit draw, and the scopes that bracket a pass.
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
using namespace NativeDetail;
NativeMaterialUnbakeScope::NativeMaterialUnbakeScope(ID3D11DeviceContext* ctx,bool enabled) {
    // One bool decides the common case: with baked lighting on, this costs a single test.
    if(!enabled||!ctx||!state.unbake||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!ps)return;
    // Not the sun ABI: only the declared array slots, which every program publishes. A material
    // that declares no baked array has no lightmap to take away and is left untouched.
    NativeSunAbiInfo info{};UINT size=sizeof(info);
    if(FAILED(ps->GetPrivateData(SunAbiInfoTag,&size,&info))||size!=sizeof(info))return;
    if(!info.lightmapArrayMask&&!info.probeSlot){
        // A program with no baked array is not necessarily unbaked: props carry theirs through
        // the light-probe path instead, so the world unbakes while they keep their baked look.
        // Record what such a program DOES declare, so the second carrier can be addressed.
        if(MvmDebugLog_Active()){
            static std::set<uint64_t> missed;
            if(missed.size()<128&&missed.insert(info.programHash).second){
                char name[128]{};UINT length=sizeof(name)-1;
                ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
                if(vs)vs->GetPrivateData(WKPDID_D3DDebugObjectName,&length,name);
                MvmDebugLog_LinefAlways("environment.unbake.miss","hash=%016llx vs=%s probeSlot=%u probeOcclusionSlot=%u lightSlot=%u flags=%u",
                    info.programHash,name,info.probeSlot,info.probeOcclusionSlot,info.lightSlot,info.flags);
            }
        }
        return;
    }
    const auto neutral=NeutralizeLightmapArrays(info.lightmapArrayMask);
    if(!neutral.Any()&&!info.probeSlot)return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!device||!EnsureUnbakeResources(device.Get(),ctx))return;
    for(UINT slot=1;slot<=3;++slot){
        const bool black=(neutral.blackSlots&(1u<<slot))!=0,fill=(neutral.fillSlots&(1u<<slot))!=0;
        if(!black&&!fill)continue;
        auto view=fill?state.unbakeFillSrv.Get():state.proofBlackArray.Get();
        slots[count]=slot;
        ctx->PSGetShaderResources(slot,1,&previous[count]);
        ctx->PSSetShaderResources(slot,1,&view);
        ++count;
    }
    if(info.probeSlot){
        // Only the irradiance volume. The occlusion volume beside it describes geometry rather
        // than the light we are removing, and the screen-space AO lesson applies: forcing it
        // unoccluded deletes every contact shadow and reads as fullbright.
        auto view=state.unbakeProbeSrv.Get();
        slots[count]=info.probeSlot;
        ctx->PSGetShaderResources(info.probeSlot,1,&previous[count]);
        ctx->PSSetShaderResources(info.probeSlot,1,&view);
        ++count;
    }
    // Reflections, but only on a material that was already carrying baked lighting. Restricting
    // it that way keeps the substitution on world and prop surfaces instead of every shader that
    // happens to declare a cube for some unrelated purpose.
    for(UINT slot=0;slot<32&&count<kMaxSlots;++slot){
        const bool cube=(info.cubeSlotMask&(1u<<slot))!=0,array=(info.cubeArraySlotMask&(1u<<slot))!=0;
        if(!cube&&!array)continue;
        auto view=array?state.unbakeCubeArraySrv.Get():state.unbakeCubeSrv.Get();
        slots[count]=slot;
        ctx->PSGetShaderResources(slot,1,&previous[count]);
        ctx->PSSetShaderResources(slot,1,&view);
        ++count;
    }
    if(!count)return;
    context=ctx;
    if(MvmDebugLog_Active()){
        // Once per program: what it declared and which slots it handed over. A family that never
        // appears here is one the unbake does not reach, which is the first thing to check when
        // a surface keeps its baked light.
        static std::set<uint64_t> seen;
        if(seen.size()<128&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("environment.unbake","hash=%016llx lightmapMask=%u blackSlots=0x%x fillSlots=0x%x probeSlot=%u cubeMask=0x%x cubeArrayMask=0x%x bound=%u fill=%g sunAbi=%u",
                info.programHash,info.lightmapArrayMask,neutral.blackSlots,neutral.fillSlots,info.probeSlot,
                info.cubeSlotMask,info.cubeArraySlotMask,count,state.unbakeFillValue,info.lightSlot);
    }
}
NativeMaterialUnbakeScope::~NativeMaterialUnbakeScope() {
    if(!context)return;
    for(unsigned i=0;i<count;++i){
        context->PSSetShaderResources(slots[i],1,&previous[i]);
        if(previous[i])previous[i]->Release();
    }
}
namespace {
// Temporary census: why each draw was or was not taken, by vertex shader and viewport depth.
void Census(ID3D11DeviceContext* ctx,const char* reason,const D3D11_VIEWPORT& vp){
    if(!MvmDebugLog_Active())return;
    static std::map<std::string,unsigned> counts;static ULONGLONG last=0;
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);char name[128]{};UINT size=127;
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name)))strcpy_s(name,"<unnamed>");
    char abi[96]="ps=none";
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(ps){NativeSunAbiInfo info{};UINT infoSize=sizeof(info);
        if(SUCCEEDED(ps->GetPrivateData(SunAbiInfoTag,&infoSize,&info)))
            sprintf_s(abi,"ps[flags=%u cb3=%u light=%u lm=%u probe=%u cube=%x/%x]",info.flags,info.cb3Count,info.lightSlot,info.lightmapArrayMask,info.probeSlot,info.cubeSlotMask,info.cubeArraySlotMask);
        else strcpy_s(abi,"ps=untagged");}
    // Constant-buffer window: the engine may bind a sub-range of a larger buffer, and a patched copy
    // that ignores the window would be read from the wrong place.
    char window[64]="";
    ComPtr<ID3D11DeviceContext1> ctx1;
    if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))){
        ID3D11Buffer* cbs[1]{};UINT first=0,count=0;ctx1->PSGetConstantBuffers1(3,1,cbs,&first,&count);
        if(cbs[0]){D3D11_BUFFER_DESC bd{};cbs[0]->GetDesc(&bd);sprintf_s(window," cb3[bytes=%u first=%u count=%u]",bd.ByteWidth,first,count);cbs[0]->Release();}
    }
    char key[520];sprintf_s(key,"%s | %s | %s%s | vp=%gx%g depth=%g..%g",reason,name,abi,window,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth);
    ++counts[key];
    const auto now=GetTickCount64();
    if(now-last>4000){last=now;for(auto& kv:counts)MvmDebugLog_LinefAlways("relight.census","%u x %s",kv.second,kv.first.c_str());counts.clear();}
}
// The engine's lighting constant buffer is read-only to us, so a patched copy is bound in its
// place for the length of one draw. Two GPU-side buffers: a plain one that takes byte-range
// patches, and a constant-buffer-bindable one that receives the result.
bool EnsureConstantClones(ID3D11Device* device,UINT bytes){
    RenderPrivateTextureScope privateTextures;
    D3D11_BUFFER_DESC prior{};if(state.clone)state.clone->GetDesc(&prior);
    if(state.clone&&state.raw&&prior.ByteWidth==bytes)return true;
    const auto saved=state.constantClonePool.find(bytes);
    if(saved!=state.constantClonePool.end()) {
        state.clone=saved->second[0];state.raw=saved->second[1];return true;
    }
    state.clone.Reset();state.raw.Reset();
    D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.clone)))return false;
    b.BindFlags=0;
    if(FAILED(device->CreateBuffer(&b,nullptr,&state.raw)))return false;
    state.constantClonePool.emplace(bytes,std::array<ComPtr<ID3D11Buffer>,2>{state.clone,state.raw});
    return true;
}
void RestoreOwnedInputs(ID3D11DeviceContext* ctx){
    if(state.sharedLightingActive){auto buffer=state.previousSharedLighting.Get();ctx->PSSetConstantBuffers(NativeLightingConstantSlot,1,&buffer);state.previousSharedLighting.Reset();state.sharedLightingActive=false;}
    if(state.sharedAoActive){auto view=state.previousSharedAo.Get();ctx->PSSetConstantBuffers(NativeAoConstantSlot,1,&view);state.previousSharedAo.Reset();state.sharedAoActive=false;}
    if(state.viewActive){auto view=state.previousView.Get();ctx->PSSetConstantBuffers(4,1,&view);state.previousView.Reset();state.viewActive=false;}
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){
        state.proofResources.Restore(ctx);
        auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;
    }
}
void InspectLiveInputs(ID3D11DeviceContext* ctx,ID3D11Device* device,const NativeSunAbiInfo& info,bool replaced){
    if(!MvmDebugLog_Active())return;
    static uint64_t generation=~0ull;
    static unsigned seen=0;
    if(generation!=state.frame.relight.generation){generation=state.frame.relight.generation;seen=0;}
    const unsigned kind=info.lightmapArrayMask?1:2;
    if(seen&kind)return;seen|=kind;
    // Debug logging can remain enabled while time-of-day animates every frame.
    // Bound synchronous GPU readbacks independently of settings generations.
    static ULONGLONG previous[2]{};
    const unsigned index=kind==1?0:1;
    const ULONGLONG now=GetTickCount64();
    if(previous[index]&&now-previous[index]<1000)return;
    previous[index]=now;
    ComPtr<ID3D11Buffer> bound;ctx->PSGetConstantBuffers(3,1,&bound);
    if(!bound)return;
    D3D11_BUFFER_DESC desc{};bound->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> read;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&read)))return;
    ctx->CopyResource(read.Get(),bound.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
    const auto values=static_cast<const float*>(mapped.pData);
    const auto integers=static_cast<const UINT*>(mapped.pData);
    MvmDebugLog_LinefAlways("relight.live.inputs",
        "generation=%llu hash=%016llx deviceFlags=%08x replaced=%u clone=%u sun=(%g %g %g) ambient=(%g %g %g) floor=(%g %g %g) words=%u probes=%u intensity=%g indirect=%g",
        generation,info.programHash,device->GetCreationFlags(),unsigned(replaced),unsigned(bound.Get()==state.clone.Get()),
        values[1946*4],values[1946*4+1],values[1946*4+2],values[7*4+3],values[8*4+3],values[9*4+3],
        values[18*4],values[18*4+1],values[18*4+2],integers[19*4+2],integers[21*4+2],state.frame.relight.intensity,state.frame.relight.indirect);
    ctx->Unmap(read.Get(),0);
    ComPtr<ID3D11ShaderResourceView> volume;ctx->PSGetShaderResources(replaced?Relight::IrradianceTextureSlot:info.probeSlot,1,&volume);
    ComPtr<ID3D11Resource> resource;if(volume)volume->GetResource(&resource);
    ComPtr<ID3D11Texture3D> texture;
    if(!resource||FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE3D_DESC td{};texture->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return;
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture3D> copy;if(FAILED(device->CreateTexture3D(&td,nullptr,&copy)))return;
    ctx->CopyResource(copy.Get(),texture.Get());
    if(FAILED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
    const auto radiance=static_cast<const float*>(mapped.pData);
    MvmDebugLog_LinefAlways("relight.live.volume","hash=%016llx size=%ux%ux%u rgb=(%g %g %g) owned=%u",
        info.programHash,td.Width,td.Height,td.Depth,radiance[0],radiance[1],radiance[2],unsigned(volume.Get()==state.probeBinding.irradiance));
    ctx->Unmap(copy.Get(),0);
}
// Replaces the map's own lighting inputs for one engine draw with ours: sun and lights as
// clustered records with their shadow atlas, the cluster masks, the ambient rows, and every
// baked input (lightmap, probe list, reflection cubes) neutralised. The engine's shader does the
// shading; this only changes what it reads. Leaves the patched buffer in state.clone.
// Player bodies: CS2's skinned character vertex shader. Cached per shader object.
bool CharacterDraw(ID3D11DeviceContext* ctx){
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
    if(!vs)return false;
    static ID3D11VertexShader* lastShader=nullptr;static bool lastResult=false;
    if(vs.Get()==lastShader)return lastResult;
    char name[128]{};UINT size=sizeof(name)-1;
    lastShader=vs.Get();
    lastResult=SUCCEEDED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name))&&strstr(name,"csgo_character")!=nullptr;
    return lastResult;
}
// Repair only the AO addressing triple. Verified shaders read it from a shared
// buffer uploaded when resolution/viewport changes; unfamiliar programs retain
// the full native-view clone so mutable engine constants are never cached by pointer.
void BindAoView(ID3D11DeviceContext* ctx,ID3D11Device* device,const D3D11_VIEWPORT& vp){
    if(vp.MinDepth>0)return;
    UINT aoWidth=0,aoHeight=0;AmbientOcclusionSize(aoWidth,aoHeight);
    if(!aoWidth||!aoHeight)return;
    ComPtr<ID3D11PixelShader> shader,shared;ctx->PSGetShader(&shader,nullptr,nullptr);
    UINT bytes=sizeof(ID3D11PixelShader*);
    static unsigned sharedDraws=0,uploads=0,fallbackDraws=0,unchangedDraws=0;
    UINT unchanged=0,unchangedBytes=sizeof(unchanged);
    if(NativeAoRepairReuse.load(std::memory_order_relaxed)&&shader&&
        SUCCEEDED(shader->GetPrivateData(UnchangedAoViewTag,&unchangedBytes,&unchanged))&&
        unchangedBytes==sizeof(unchanged)&&unchanged==1){++unchangedDraws;return;}
    if(shader&&SUCCEEDED(::Filmmaker::GetOwnedInterface(shader.Get(),SharedAoShaderTag,&bytes,(void**)shared.GetAddressOf()))&&shared){
        const std::array<float,4> row{1.f/float(aoWidth),1.f/float(aoHeight),float(aoWidth)/vp.Width,0};
        if(!state.sharedAo){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=row.data();
            if(SUCCEEDED(device->CreateBuffer(&d,&initial,&state.sharedAo))){state.sharedAoRow=row;++uploads;}}
        if(state.sharedAo){
            if(row!=state.sharedAoRow){ctx->UpdateSubresource(state.sharedAo.Get(),0,nullptr,row.data(),0,0);state.sharedAoRow=row;++uploads;}
            ctx->PSGetConstantBuffers(NativeAoConstantSlot,1,state.previousSharedAo.ReleaseAndGetAddressOf());
            auto constants=state.sharedAo.Get();ctx->PSSetConstantBuffers(NativeAoConstantSlot,1,&constants);
            ctx->PSSetShader(shared.Get(),nullptr,0);state.sharedAoActive=true;++sharedDraws;
        }
    }
    if(!state.sharedAoActive){
        ++fallbackDraws;
        ComPtr<ID3D11Buffer> nativeView;ctx->PSGetConstantBuffers(4,1,&nativeView);
        if(aoWidth&&aoHeight&&nativeView){
            D3D11_BUFFER_DESC vd{};nativeView->GetDesc(&vd);
            if(vd.ByteWidth>=17*16&&!(vd.ByteWidth%16)){
                D3D11_BUFFER_DESC have{};if(state.viewClone)state.viewClone->GetDesc(&have);
                if(!state.viewClone||!state.viewCloneRaw||have.ByteWidth!=vd.ByteWidth){state.viewClone.Reset();state.viewCloneRaw.Reset();
                    D3D11_BUFFER_DESC d{};d.ByteWidth=vd.ByteWidth;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;device->CreateBuffer(&d,nullptr,&state.viewClone);
                    d.BindFlags=0;device->CreateBuffer(&d,nullptr,&state.viewCloneRaw);}
                if(state.viewClone&&state.viewCloneRaw){
                    // A constant buffer takes only whole updates: patch row 16 in a plain buffer and copy
                    // it across (a partial UpdateSubresource on the constant buffer itself is dropped).
                    // Re-copied per draw: the engine rewrites its per-view buffer between passes.
                    ctx->CopyResource(state.viewCloneRaw.Get(),nativeView.Get());
                    const float row[3]{1.f/float(aoWidth),1.f/float(aoHeight),float(aoWidth)/vp.Width};
                    const D3D11_BOX box{16*16,0,0,16*16+12,1,1};ctx->UpdateSubresource(state.viewCloneRaw.Get(),0,&box,row,0,0);
                    ctx->CopyResource(state.viewClone.Get(),state.viewCloneRaw.Get());
                    state.previousView=nativeView;auto repaired=state.viewClone.Get();ctx->PSSetConstantBuffers(4,1,&repaired);state.viewActive=true;
                }
            }
        }
    }
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.ao.shared","draws=%u uploads=%u fallback=%u unchanged=%u",sharedDraws,uploads,fallbackDraws,unchangedDraws);
        sharedDraws=uploads=fallbackDraws=unchangedDraws=0;}}
}
bool BindOwnedInputs(ID3D11DeviceContext* ctx,ID3D11Device* device,const D3D11_VIEWPORT& vp,ID3D11Buffer* native,const NativeSunAbiInfo& info){
    LiveLap(0);
    if(!ProofResources(device,ctx,vp)){LiveLap(1);return false;}
    LiveLap(1);
    const auto& set=state.frame.relight;
    state.inputsProvisional=false;
    auto& patches=state.constantPatches;patches.Reset();state.lightingCopies.clear();
    auto patchRange=[&](UINT offset,UINT bytes,const void* value){patches.Write(offset,bytes,value);};
    auto copyRange=[&](UINT offset,ID3D11Buffer* source,UINT bytes){
        patches.Overwrite(offset,bytes);
        state.lightingCopies.push_back({offset,bytes,source});
    };
    const UINT zeros[4]{};
    // The native sun goes dark; the sun is one of our clustered records instead, with its own
    // shadow tile, so the engine's cascades must not shadow it a second time.
    patchRange(NativeSunRadianceOffset,12,zeros);
    patchRange(NativeSunCascadesOffset,4,zeros);
    // The 3D skybox (depth .95..1) is a scaled miniature in its own coordinate space. Map
    // shadow maps and map light positions do not describe it. Its exported world node has
    // a separate sun depth map in those miniature coordinates, and no foreground local records.
    const bool skybox=vp.MinDepth>0;
    auto skyboxShadow=skybox?SkyboxSunShadow(nullptr):nullptr;
    const bool ownedSun=info.ownedCascades;
    ID3D11ShaderResourceView* sunVisibility=nullptr;
    if(ownedSun){
        const auto direction=Relight::SunDirection(set.sunAngles),radiance=Relight::SunRadiance(set);
        const float sunDirection[4]{direction.x,direction.y,direction.z,0};
        patchRange(NativeSunDirectionOffset,sizeof(sunDirection),sunDirection);
        patchRange(NativeSunRadianceOffset,sizeof(radiance),&radiance);
        const UINT count=set.shadows&&set.sunShadows?(skybox?(skyboxShadow?1u:0u):(state.proofSunShadowed?4u:0u)):0u;
        patchRange(NativeSunCascadesOffset,4,&count);
        // Physically sized sun penumbra: a per-pixel visibility pass takes over the cascade filter for
        // programs that expose the engine's screen-space shadow hook (info.screenShadowSlot). The flag
        // row says whether this draw supplies the texture; without it the native filter runs.
        // A forced cascade (mirv_relight suncascade, diagnostic) runs the native loop unchanged.
        // Players keep the native filter: the visibility pass rebuilds normals from the depth buffer,
        // which on a moving body turned every self-shadow terminator into stair-stepped teeth that
        // crawled frame to frame, and its receiver tolerance cannot carry the body's pose-lag bias.
        // The native loop uses the vertex normal and the character bias (cb3[1953].z) instead.
        if(count&&!skybox&&info.screenShadowSlot&&set.sunCascade<0&&!CharacterDraw(ctx)){
            Matrix cascades[4];for(unsigned i=0;i<4;++i)cascades[i]=state.proofMatrices[OwnedSunTiles[i]];
            const auto rects=NativeSunRects();Matrix toWorld{};
            if(state.frame.hasWorldToClip&&Inverse(state.frame.worldToClip,toWorld)){
                SunVisibilityInput input;input.cascades=cascades;input.rects=rects.data();input.atlas=state.proofAtlas.Get();
                input.atlasWidth=float(Relight::AtlasWidth);input.atlasHeight=float(Relight::AtlasHeight);
                input.tileSize=float(Relight::SunCascadeSize);input.camera=state.frame.camera;input.clipToWorld=&toWorld;
                input.tanHalfAngle=SunTanHalfAngle(set.sunAngle)*set.shadowSoftness;input.bias=set.shadowBias;
                input.summary=SunShadowPyramid(input.atlas);input.summaryLevels=SunShadowPyramidLevels;
                if(input.atlas){sunVisibility=SunVisibility(ctx,vp,input);state.inputsProvisional=SunVisibilityProvisional();}
            }
        }
        const UINT screenShadowFlag=sunVisibility?1u:0u;
        patchRange(ScreenShadowFlagRow*16+4,4,&screenShadowFlag);
        // Begin blending in the outer 12% of each light-space square. The
        // shader already blends the next tier; the final tier covers the map.
        float thresholds[4]{.985f,.985f,.985f,1.f};
        if(set.sunCascade>=0)for(int i=0;i<4;++i)if(i!=set.sunCascade)thresholds[i]=0;
        const float blend[4]{-7.333333f,8.333333f,0,0};
        patchRange(1954*16,sizeof(thresholds),thresholds);patchRange(1955*16,sizeof(blend),blend);
        auto columnSource=CharacterDraw(ctx)&&state.characterSunCascadeColumns?state.characterSunCascadeColumns.Get():state.sunCascadeColumns.Get();
        copyRange(1956*16,columnSource,4*UINT(sizeof(Matrix)));
        auto rects=NativeSunRects();
        if(skyboxShadow)for(unsigned i=0;i<4;++i)std::copy(NativeSkyboxSunRect.begin(),NativeSkyboxSunRect.end(),rects.begin()+i*4);
        patchRange(1988*16,sizeof(rects),rects.data());
        // Native probe contact modulation belongs to baked lighting; the owned
        // sun's geometry visibility is entirely in the current shadow atlas.
        patchRange(1947*16,16,zeros);
        if(MvmDebugLog_Active()){static ULONGLONG last[2]{};const auto now=GetTickCount64();auto& reported=last[skybox?1:0];if(now-reported>=3000){reported=now;
            MvmDebugLog_LinefAlways(skybox?"environment.skybox.sun.cascades":"environment.relight.sun.cascades","count=%u resolution=%u hash=%016llx camera=(%.2f %.2f %.2f) nativeViewRebase=1 localSunSuppressed=1",
                count,Relight::SunCascadeSize,info.programHash,state.frame.camera.x,state.frame.camera.y,state.frame.camera.z);}}
    }
    // cb3[0..3] carry the shadow filter kernel; zeroing them collapses every tap onto one texel.
    float filterRows[16];Relight::ShadowFilterRows(Relight::LampFilterTexels(set.lampAngle,set.shadowSoftness),filterRows);
    filterRows[0]=2*std::tan(std::clamp(set.lampAngle,0.f,5.f)*.00872664626f)*std::clamp(set.shadowSoftness,0.f,4.f);
    filterRows[1]=Relight::FaceGuardTexels/Relight::AtlasWidth;
    filterRows[2]=Relight::FaceGuardTexels/Relight::AtlasHeight;
    if(skyboxShadow){
        Relight::ShadowFilterRows(Relight::NativeFilterTexels*set.sunAngle/.53f*set.shadowSoftness,filterRows);
        filterRows[9]*=float(Relight::AtlasWidth)/Relight::SunCascadeSize;filterRows[10]=-filterRows[9];
        filterRows[13]*=float(Relight::AtlasHeight)/Relight::SunCascadeSize;filterRows[14]=-filterRows[13];
    }
    patchRange(0,sizeof(filterRows),filterRows);
    const float bias=-(CharacterDraw(ctx)?CharacterLocalBias(set):set.shadowBias);patchRange(1953*16+8,4,&bias);
    // cb3[19].z is how many 32-bit mask words the clustered loop reads: capacity is 32 * words.
    const UINT words=skybox&&ownedSun?0u:set.cost==1?1:state.proofMaskWords;
    auto cull=state.clusterGrid.ListRow(state.clusterGrid.lights,state.clusterMode);cull[2]=words;
    const auto tiles=state.clusterGrid.TileConstants(state.clusterMode);
    const auto slices=state.clusterGrid.SliceConstants(state.clusterMode);
    // cb3[20] and cb3[21] are the reflection-cube and irradiance-probe lists. Emptying them takes
    // the baked cubemaps and the baked probe volume out of the shader's reach entirely.
    // The empty reflection list keeps its grid offsets and reads no mask words, as the light list does.
    auto noReflections=state.clusterGrid.ListRow(state.clusterGrid.reflections,state.clusterMode);noReflections[2]=0;
    patchRange(19*16,16,cull.data());patchRange(20*16,16,noReflections.data());patchRange(21*16,16,zeros);
    patchRange(22*16,16,tiles.data());patchRange(23*16,16,slices.data());
    if(state.clusterMode&&!(skybox&&ownedSun))copyRange(19*16,state.clusterRows.Get(),80);
    // Sky-over-ground hemisphere rows; the shader multiplies them by albedo itself.
    float ambient[12]{};
    {const auto fill=Relight::AmbientFill(set);Relight::AmbientRows(fill[2],fill[5],ambient);}
    if(set.probeCheck==1||set.probeCheck==2)Relight::AmbientRows(Relight::ProbeNeutral,Relight::ProbeNeutral,ambient);
    patchRange(7*16,sizeof(ambient),ambient);
    // cb3[18] is a flat floor added regardless of orientation or occlusion; only the ground term
    // may go there, and never where the lightmap already carries the fill.
    const float floorTerm[4]{ambient[3]-ambient[2],ambient[7]-ambient[6],ambient[11]-ambient[10],0};
    if(!(info.lightmapArrayMask&2))patchRange(18*16,16,floorTerm);else patchRange(18*16,16,zeros);
    // Live reflections: one map-wide record for the capture marched from the camera through the
    // lit bounce volume, in the clustered reflection list the baked cubes used to occupy.
    const auto& live=state.probeBinding;
    const bool reflections=!skybox&&(state.probeTraced||set.lite)&&set.reflections>0&&live.reflection&&live.reflectionNorm&&info.cubeArraySlotMask;
    if(reflections){
        const auto& grid=state.clusterGrid;
        const UINT list[4]{grid.reflections.slices,grid.reflections.tiles,1,0};patchRange(20*16,16,list);
        const auto rows=Relight::ReflectionRecordRows(live.reflectionOrigin,state.probeTraced?set.reflections:set.reflections*Relight::LiteReflectionStrength);
        patchRange(25*16,UINT(sizeof(rows)),rows.data());
        copyRange(33*16,live.reflectionNorm,16);
        const float mips=float(live.reflectionMips?live.reflectionMips-1:0);patchRange(24*16+4,4,&mips);
        patchRange(6*16,8,Relight::ReflectionNormalizationCap.data());
        if(MvmDebugLog_Active()){static std::set<uint64_t> seen;
            if(seen.size()<256&&seen.insert(info.programHash).second)
                MvmDebugLog_LinefAlways("relight.reflection.program","hash=%016llx cubeArrayMask=0x%x cubeMask=0x%x mips=%u origin=(%.0f %.0f %.0f) strength=%.2f viewportDepth=%g..%g",
                    info.programHash,info.cubeArraySlotMask,info.cubeSlotMask,live.reflectionMips,live.reflectionOrigin.x,live.reflectionOrigin.y,live.reflectionOrigin.z,set.reflections,vp.MinDepth,vp.MaxDepth);}
    }else if(MvmDebugLog_Active()&&info.cubeArraySlotMask){static std::set<uint64_t> seen;
        if(seen.size()<256&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("relight.reflection.skip","hash=%016llx skybox=%u traced=%u strength=%.2f capture=%u",
                info.programHash,unsigned(skybox),unsigned(state.probeTraced),set.reflections,unsigned(live.reflection!=nullptr));}
    state.proofLightSlot=info.lightSlot;
    ComPtr<ID3D11Buffer> nativeView;ctx->PSGetConstantBuffers(4,1,&nativeView);
    // The material adds cb4[35].xyz to its position varying for world-space lookups (probe
    // volume, reflections). Compare it between the world pass and the first-person pass.
    if(MvmDebugLog_Active()&&nativeView){
        static ULONGLONG last[3]{};const unsigned pass=skybox?2:vp.MaxDepth<=.1f+1e-6f?1:0;const auto now=GetTickCount64();
        if(now-last[pass]>=3000){last[pass]=now;
            D3D11_BUFFER_DESC d{};nativeView->GetDesc(&d);
            if(d.ByteWidth>=36*16){d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Buffer> read;D3D11_MAPPED_SUBRESOURCE m{};
                if(SUCCEEDED(device->CreateBuffer(&d,nullptr,&read))){ctx->CopyResource(read.Get(),nativeView.Get());
                    if(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m))){const auto f=static_cast<const float*>(m.pData);
                        MvmDebugLog_LinefAlways("relight.view.offset","pass=%s hash=%016llx cb4[35]=(%.2f %.2f %.2f) camera=(%.2f %.2f %.2f) probe=%u traced=%u vp=%g..%g",
                            pass==2?"skybox":pass==1?"viewmodel":"world",info.programHash,f[140],f[141],f[142],state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,
                            info.probeSlot,unsigned(state.probeTraced),vp.MinDepth,vp.MaxDepth);
                        if(pass==1&&d.ByteWidth>=41*16)MvmDebugLog_LinefAlways("relight.view.offset","viewmodel cb4[36]=(%.3f %.3f %.3f %.3f) shadowBypass=0x%08x",f[144],f[145],f[146],f[147],static_cast<const unsigned*>(m.pData)[8]);
                        ctx->Unmap(read.Get(),0);}}}
            // The first-person path moves its lighting position with per-view cb1 rows 23-28.
            ComPtr<ID3D11Buffer> perView;ctx->PSGetConstantBuffers(1,1,&perView);
            if(pass==1&&perView){D3D11_BUFFER_DESC v{};perView->GetDesc(&v);
                if(v.ByteWidth>=29*16){v.Usage=D3D11_USAGE_STAGING;v.BindFlags=v.MiscFlags=0;v.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Buffer> read;D3D11_MAPPED_SUBRESOURCE m{};
                    if(SUCCEEDED(device->CreateBuffer(&v,nullptr,&read))){ctx->CopyResource(read.Get(),perView.Get());
                        if(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m))){const auto g=static_cast<const float*>(m.pData);
                            MvmDebugLog_LinefAlways("relight.view.offset","viewmodel cb1[20]=(%g %g %g %g) [23]=(%g %g %g %g) [24]=(%g %g %g %g) [26]=(%g %g %g %g) [28]=(%g %g %g %g)",
                                g[80],g[81],g[82],g[83],g[92],g[93],g[94],g[95],g[96],g[97],g[98],g[99],g[104],g[105],g[106],g[107],g[112],g[113],g[114],g[115]);
                            ctx->Unmap(read.Get(),0);}}}}}
    }
    Matrix skyMatrices[SkyVisibilityDirections]{};SkyVisibilityInput sky{};
    if(set.skyOcclusion>0&&!state.probeTraced&&!state.liteGrid&&skyVisibilitySource&&skyVisibilitySource(&sky.maps,skyMatrices)){sky.matrices=skyMatrices;sky.strength=Relight::LiteSkyOcclusion(set);}
    Matrix clipToWorld{};const bool unprojects=state.frame.hasWorldToClip&&Inverse(state.frame.worldToClip,clipToWorld);
    LiveLap(0);
    auto ao=AmbientOcclusion(ctx,state.frame.camera,vp,nativeView.Get(),clipToWorld,unprojects?set.ao:0.f,unprojects?&sky:nullptr,unprojects&&state.probeTraced&&!skybox);
    state.inputsProvisional=state.inputsProvisional||AmbientOcclusionProvisional();
    LiveLap(2);
    if(state.probeTraced&&!skybox&&!set.probeCheck){
        UINT width=0,height=0;auto positions=AmbientOcclusionPositions(ctx,width,height);
        Relight::MarkVisibleProbeGi(ctx,positions,width,height,state.probeFrame);
    }
    UINT slots[48]{13,state.proofLightSlot,6,7};
    ID3D11ShaderResourceView* views[48]{state.proofMaskSrv.Get(),ownedSun?state.proofLocalLightSrv.Get():state.proofLightSrv.Get(),skyboxShadow?skyboxShadow:state.proofAtlas.Get(),ao};
    unsigned bindings=4;
    // The lamp filter's atlas summary: unbound (no early-out) unless slot 6 holds the map atlas.
    slots[bindings]=NativeLampPyramidSlot;views[bindings++]=skyboxShadow?nullptr:LampShadowPyramid(state.proofAtlas.Get());
    if(sunVisibility){slots[bindings]=info.screenShadowSlot;views[bindings++]=sunVisibility;}
    for(unsigned slot=0;slot<32;++slot){
        if(info.cubeSlotMask&(1u<<slot)){slots[bindings]=slot;views[bindings++]=state.proofBlackCube.Get();}
        if(info.cubeArraySlotMask&(1u<<slot)){slots[bindings]=slot;views[bindings++]=reflections?live.reflection:state.proofBlackCubeArray.Get();}
    }
    // Stained glass beams select their window from this cookie volume by radiance.w.
    if(state.glassCookies&&info.hasCookie&&!skybox){slots[bindings]=info.cookieSlot;views[bindings++]=state.glassCookies;}
    const auto& probe=skybox?state.skyboxBinding:state.probeBinding;
    if(probe.constants&&info.probeSlot){
        const UINT table[4]{state.clusterGrid.probes.slices,state.clusterGrid.probes.tiles,1,0};
        patchRange(21*16,16,table);patchRange(1177*16,6*16,probe.packing.probe.data());
        patchRange(1947*16,16,zeros);
        if(set.probeCheck!=1&&set.probeCheck!=2)patchRange(18*16,16,zeros);
        ctx->PSGetConstantBuffers(2,1,state.previousProbeConstants.ReleaseAndGetAddressOf());
        auto cb2=probe.constants;ctx->PSSetConstantBuffers(2,1,&cb2);state.probeActive=true;state.proofOurProbeConstants=cb2;
        slots[bindings]=info.probeSlot;views[bindings++]=probe.irradiance;
        if(info.probeOcclusionSlot){slots[bindings]=info.probeOcclusionSlot;views[bindings++]=probe.sunOcclusion;}
    }
    for(unsigned array=0;array<3;++array)if(info.lightmapArrayMask&(1u<<array)){
        slots[bindings]=array+1;
        if(array==1&&!state.engineLightmapWidth){
            ComPtr<ID3D11ShaderResourceView> engine;ctx->PSGetShaderResources(2,1,&engine);
            if(engine){
                ComPtr<ID3D11Resource> resource;engine->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
                if(resource&&SUCCEEDED(resource.As(&texture))){
                    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                    state.engineLightmapWidth=desc.Width;state.engineLightmapHeight=desc.Height;state.engineLightmapArray=desc.ArraySize;
                    if(MvmDebugLog_Active())MvmDebugLog_LinefAlways("relight.lightmap.engine","width=%u height=%u array=%u format=%u",desc.Width,desc.Height,desc.ArraySize,unsigned(desc.Format));
                }
            }
        }
        // The replacement shader queries world-space irradiance. All three baked
        // arrays are unreachable; black is also the safe value for an unknown query.
        views[bindings]=state.proofBlackArray.Get();
        ++bindings;
    }
    for(unsigned i=0;i<bindings;++i)if(slots[i]<state.proofOurViews.size())state.proofOurViews[slots[i]]=views[i];
    state.proofResources.Bind(ctx,bindings,slots,views);
    state.proofSamplerSlot=info.comparisonSampler;
    ctx->PSGetSamplers(state.proofSamplerSlot,1,state.proofPreviousSampler.ReleaseAndGetAddressOf());
    auto sampler=state.proofComparison.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);
    state.proofActive=true;
    return true;
}
bool FinalizeOwnedConstants(ID3D11DeviceContext* ctx,ID3D11Device* device,ID3D11Buffer* native){
    static unsigned sharedDraws=0,sharedUploads=0,fallbackDraws=0;
    ComPtr<ID3D11PixelShader> current,shared;ctx->PSGetShader(&current,nullptr,nullptr);
    UINT bytes=sizeof(ID3D11PixelShader*);NativeSharedLightingInfo layout{};UINT layoutBytes=sizeof(layout);
    const bool sparseView=state.clusterSummaries&&state.clusterMode==1&&state.proofViewport.MinDepth==0&&
        std::abs(state.proofViewport.MaxDepth-.95f)<1e-6f;
    if(current&&sparseView){ComPtr<ID3D11PixelShader> sparseShader;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(current.Get(),SparseClusterShaderTag,&bytes,(void**)sparseShader.GetAddressOf()))&&sparseShader)shared=std::move(sparseShader);
    }
    if(!shared&&current){bytes=sizeof(ID3D11PixelShader*);::Filmmaker::GetOwnedInterface(current.Get(),SharedLightingShaderTag,&bytes,(void**)shared.GetAddressOf());}

    auto& covered=state.lightingCovered;covered.clear();
    state.constantPatches.Flush([&](UINT offset,UINT size,const void*){covered.push_back({offset,offset+size});});
    for(const auto& copy:state.lightingCopies)covered.push_back({copy.offset,copy.offset+copy.bytes});
    const bool eligible=current&&shared&&
        SUCCEEDED(shared->GetPrivateData(SharedLightingInfoTag,&layoutBytes,&layout))&&layoutBytes==sizeof(layout)&&
        NativeSharedLightingCovered(layout,covered);
    ID3D11Buffer* selected=nullptr;
    if(eligible){
        UINT sparse=0,sparseBytes=sizeof(sparse);
        const UINT summaryOffset=state.clusterSummaries&&state.clusterMode==1&&state.proofViewport.MinDepth==0&&
            SUCCEEDED(shared->GetPrivateData(SparseClusterInfoTag,&sparseBytes,&sparse))&&sparse?state.clusterGrid.size:0u;
        // Exact plan equality within one frame/view. Engine buffer identities are
        // deliberately absent: native rows stay on b3 and may change every draw.
        auto& key=state.lightingKey;key.clear();
        auto append=[&](const void* data,size_t size){const auto* p=static_cast<const uint8_t*>(data);key.insert(key.end(),p,p+size);};
        append(&state.proofUploads,sizeof(state.proofUploads));
        append(&summaryOffset,sizeof(summaryOffset));
        const UINT cpuRanges=UINT(covered.size()-state.lightingCopies.size()),gpuRanges=UINT(state.lightingCopies.size());
        append(&cpuRanges,sizeof(cpuRanges));append(&gpuRanges,sizeof(gpuRanges));
        state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){append(&offset,sizeof(offset));append(&size,sizeof(size));append(value,size);});
        for(const auto& copy:state.lightingCopies){append(&copy.offset,sizeof(copy.offset));append(&copy.bytes,sizeof(copy.bytes));append(&copy.source,sizeof(copy.source));}
        SharedLightingBuffer* entry=nullptr;
        for(unsigned i=0;i<state.sharedLightingUsed;++i)if(state.sharedLightingPool[i].key==key){entry=&state.sharedLightingPool[i];break;}
        if(!entry&&state.sharedLightingUsed<64){
            if(state.sharedLightingUsed==state.sharedLightingPool.size())state.sharedLightingPool.emplace_back();
            entry=&state.sharedLightingPool[state.sharedLightingUsed];
            if(!entry->raw||!entry->constants){
                RenderPrivateTextureScope privateTextures;D3D11_BUFFER_DESC d{};d.ByteWidth=NativeLightingRows*16;
                if(!entry->raw)device->CreateBuffer(&d,nullptr,&entry->raw);
                d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                if(!entry->constants)device->CreateBuffer(&d,nullptr,&entry->constants);
            }
            if(entry->raw&&entry->constants){
                std::array<uint8_t,NativeLightingRows*16> data{};
                state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){std::memcpy(data.data()+offset,value,size);});
                ctx->UpdateSubresource(entry->raw.Get(),0,nullptr,data.data(),0,0);
                for(const auto& copy:state.lightingCopies)
                    state.constantPatches.ForEachUnwritten(copy.offset,copy.bytes,[&](UINT offset,UINT size){
                        const UINT sourceOffset=offset-copy.offset;
                        const D3D11_BOX range{sourceOffset,0,0,sourceOffset+size,1,1};
                        ctx->CopySubresourceRegion(entry->raw.Get(),0,offset,0,0,copy.source,0,&range);
                    });
                // GPU cluster rows overwrite the CPU row during the copies above.
                // Enable occupancy only for the selected, validated shared variant.
                const D3D11_BOX summaryFlag{19*16+12,0,0,19*16+16,1,1};
                if(summaryOffset)ctx->UpdateSubresource(entry->raw.Get(),0,&summaryFlag,&summaryOffset,0,0);
                ctx->CopyResource(entry->constants.Get(),entry->raw.Get());entry->key=key;++state.sharedLightingUsed;++sharedUploads;
            }else entry=nullptr;
        }
        if(entry){
            selected=entry->constants.Get();ctx->PSGetConstantBuffers(NativeLightingConstantSlot,1,state.previousSharedLighting.ReleaseAndGetAddressOf());
            ctx->PSSetConstantBuffers(NativeLightingConstantSlot,1,&selected);state.sharedLightingActive=true;state.proofOurLighting=selected;
            ctx->PSSetShader(shared.Get(),nullptr,0);++sharedDraws;
            if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(summaryOffset&&now-last>=3000){last=now;
                MvmDebugLog_LinefAlways("relight.cluster.sparse","offset=%u words=%u rasterOnly=1",summaryOffset,state.proofMaskWords);}}
        }
    }
    if(!selected){
        D3D11_BUFFER_DESC desc{};native->GetDesc(&desc);const D3D11_BOX nativeRange{0,0,0,desc.ByteWidth,1,1};
        ctx->CopySubresourceRegion(state.raw.Get(),0,0,0,0,native,0,&nativeRange);
        for(const auto& copy:state.lightingCopies){const D3D11_BOX range{0,0,0,copy.bytes,1,1};
            ctx->CopySubresourceRegion(state.raw.Get(),0,copy.offset,0,0,copy.source,0,&range);}
        state.constantPatches.Flush([&](UINT offset,UINT size,const void* value){const D3D11_BOX range{offset,0,0,offset+size,1,1};
            ctx->UpdateSubresource(state.raw.Get(),0,&range,value,0,0);});
        ctx->CopyResource(state.clone.Get(),state.raw.Get());auto clone=state.clone.Get();ctx->PSSetConstantBuffers(3,1,&clone);++fallbackDraws;
    }
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("relight.constants.shared","draws=%u uploads=%u fallback=%u variants=%u",sharedDraws,sharedUploads,fallbackDraws,state.sharedLightingUsed);
        sharedDraws=sharedUploads=fallbackDraws=0;}}
    return selected!=nullptr;
}
}
NativeMaterialSkyScope::NativeMaterialSkyScope(ID3D11DeviceContext* ctx,bool enabled){
    if(!enabled||!ctx||!state.frame.active||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    const auto climate=EffectiveClimate(state.frame.scene.climate);
    if(climate[CL_sky]<=0)return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    char name[128]{};UINT bytes=sizeof(name)-1;
    if(!ps||FAILED(ps->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,name))||bytes>=sizeof(name))return;
    if(std::strcmp(name,"sky.vfx_ps")&&std::strcmp(name,"sky_pc_50_ps"))return;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    if(count!=1||vp.Width<256||vp.Height<256)return;
    Matrix inverse{};if(!Inverse(state.frame.worldToClip,inverse))return;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!state.skyShader){
        ComPtr<ID3DBlob> code,error;
        if(FAILED(CompileEnvironmentShader(NativeSkyShader,sizeof(NativeSkyShader)-1,"EnvironmentNativeSky",nullptr,
            "NativeSkyPS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&error))||
           FAILED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&state.skyShader))){
            Evidence("native-sky:compile-failed");return;
        }
    }
    struct Row {float x,y,z,w;};
    struct Constants {Matrix inverse;Row camera,viewport,sun,radiance,sky,tint,moon,moonLight;};
    if(!state.skyConstants){
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(Constants);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.skyConstants)))return;
    }
    // The sky draws the sun itself and the moon, whichever of them is lighting the map.
    const auto direction=ClimateTrueSunDirection(climate),radiance=ClimateTrueSunRadiance(climate);
    const auto moon=ClimateMoonDirection(climate);
    Constants constants{inverse,{state.frame.camera.x,state.frame.camera.y,state.frame.camera.z,0},
        {vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height},
        {direction.x,direction.y,direction.z,climate[CL_sunElevation]},
        {radiance.x,radiance.y,radiance.z,climate[CL_haze]},
        {climate[CL_stars],nativePostCompressedHdr.load(std::memory_order_relaxed)?1.f:0.f,0,0},
        {climate[CL_sunColorR],climate[CL_sunColorG],climate[CL_sunColorB],climate[CL_moonPhase]},
        {moon.x,moon.y,moon.z,Radians(climate[CL_moonSize]*.5f)},
        {climate[CL_moonColorR],climate[CL_moonColorG],climate[CL_moonColorB],climate[CL_moon]},
    };
    ctx->UpdateSubresource(state.skyConstants.Get(),0,nullptr,&constants,0,0);
    ctx->PSGetConstantBuffers(13,1,&previousConstants);
    auto cb=state.skyConstants.Get();ctx->PSSetConstantBuffers(13,1,&cb);
    original=ps.Detach();ctx->PSSetShader(state.skyShader.Get(),nullptr,0);context=ctx;
    state.skyApplied=true;state.skyGeneration=state.frame.generation;
    Evidence("native-sky:geometry-coverage");
}
NativeMaterialSkyScope::~NativeMaterialSkyScope(){
    if(!context)return;
    context->PSSetShader(original,nullptr,0);original->Release();
    context->PSSetConstantBuffers(13,1,&previousConstants);
    if(previousConstants)previousConstants->Release();
}
bool PrepareSmokeInputs(ID3D11Device* device,ID3D11DeviceContext* ctx){
    if(!state.proofResourcesReady||!state.probeBinding.irradiance||state.frame.smokes.empty())return false;
    if(state.smokeFrame==state.probeFrame&&state.smokeLightSrv)return true;
    const auto& set=state.frame.relight;
    std::vector<std::pair<float,unsigned>> ranked;
    for(unsigned i=0;i<set.lights.size();++i){const auto& light=set.lights[i];
        if(!set.localLights||!light.enabled||Relight::Power(light,set.intensity)<=0)continue;
        float nearest=INFINITY;
        for(const auto& smoke:state.frame.smokes){const auto d=smoke.center-light.position;nearest=(std::min)(nearest,std::sqrt(Dot(d,d))-smoke.radius);}
        if(nearest>light.range)continue;
        ranked.push_back({nearest-(light.transient||light.sceneObject?100000.f:0.f),i});
    }
    std::stable_sort(ranked.begin(),ranked.end());if(ranked.size()>8)ranked.resize(8);
    state.smokeRecords.clear();
    for(const auto& entry:ranked){const unsigned index=entry.second;const auto& light=set.lights[index];
        const unsigned first=index<state.proofFirstTiles.size()?state.proofFirstTiles[index]:~0u;
        if(first==~0u){if(!set.shadows||!light.transient)state.smokeRecords.push_back(Relight::ToRecord(light,set.intensity));continue;}
        for(unsigned face=0;face<Relight::FixtureFaces(light);++face)
            state.smokeRecords.push_back(Relight::ToShadowedRecord(light,set.intensity,state.proofMatrices[first+face].data(),first+face));
    }
    state.smokeRecords.push_back(Relight::SunRecord(Relight::SunDirection(set.sunAngles),Relight::SunRadiance(set),
        state.proofMatrices[Relight::SunTile].data(),state.proofSunShadowed&&set.shadows&&set.sunShadows));
    if(!state.smokeLights||!state.smokeLightSrv||!state.smokeConstants||!state.smokeProbes){
        state.smokeLights.Reset();state.smokeLightSrv.Reset();state.smokeConstants.Reset();state.smokeProbes.Reset();
        D3D11_BUFFER_DESC d{};d.ByteWidth=64*sizeof(Relight::LightRecord);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(Relight::LightRecord);
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeLights)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=64;
        if(FAILED(device->CreateShaderResourceView(state.smokeLights.Get(),&view,&state.smokeLightSrv)))return false;
        d={};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeConstants)))return false;
        d.ByteWidth=8*16;if(FAILED(device->CreateBuffer(&d,nullptr,&state.smokeProbes)))return false;
    }
    const D3D11_BOX range{0,0,0,UINT(state.smokeRecords.size()*sizeof(Relight::LightRecord)),1,1};
    ctx->UpdateSubresource(state.smokeLights.Get(),0,&range,state.smokeRecords.data(),0,0);
    struct Constants{UINT count;float bias,padding[2];} constants{UINT(state.smokeRecords.size()),set.shadowBias,{}};
    ctx->UpdateSubresource(state.smokeConstants.Get(),0,nullptr,&constants,0,0);
    const auto& packing=state.probeBinding.packing;std::array<Relight::ProbeRow,8> rows{};
    for(unsigned axis=0;axis<3;++axis){rows[0][axis]=packing.probe[axis][axis];rows[1][axis]=packing.probe[axis][3];
        rows[2][axis]=packing.volume[3][axis]*(axis==2?6.f:1.f);rows[3][axis]=packing.volume[4][axis]*(axis==2?6.f:1.f);}
    rows[4]=packing.volume[5];rows[6]=packing.volume[6];rows[7]=packing.volume[7];
    ctx->UpdateSubresource(state.smokeProbes.Get(),0,nullptr,rows.data(),0,0);
    state.smokeFrame=state.probeFrame;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=1000){last=now;
        MvmDebugLog_LinefAlways("environment.smoke.lighting","records=%zu lights=%zu clouds=%zu liveGi=1 atlas=%d",state.smokeRecords.size(),ranked.size(),state.frame.smokes.size(),bool(state.proofAtlas));}}
    return true;
}
// THE 3D SKYBOX'S UNLIT DRAWS AT NIGHT. Its lit draws are relit like the map; what has no light slot
// (Mirage's haze cards over the far city, a painted skyline) keeps the daylight painted into it, so
// it stood out white against a night sky. It is dimmed here by the share of the authored daylight the
// relit world still gets (Frame::bakedDaylight, the same ratio the relit map is lit by), through the
// blend state: exact for an opaque draw and for an additive or premultiplied one (src x ONE becomes
// src x k). A card blended by its own alpha (SRC_ALPHA, Mirage's haze) would need alpha and k in one
// product, which a blend state cannot give, so it draws with a variant of its own program that writes
// its colour times k and its alpha times a coverage factor (EnvironmentNativeScaledColorMath.h). Haze
// shows by the light it scatters, so as it darkens it also thins: the coverage factor is sqrt(k), no
// lower than a quarter. Fully covered it hid the night sky behind it as a dark slab.
// THE MAP'S OWN HAZE CARDS (`world`): Mirage rings its edge with alpha-blended csgo_effects cards in
// the main view, the white blobs over the far rooftops at night. Only that alpha path applies to them;
// additive effects in the main view keep UnlitOverlayScale and every other unlit draw is untouched.
void NativeMaterialLiveScope::UnlitCardDim(ID3D11DeviceContext* ctx,bool world){
    const float k=state.frame.bakedDaylight>=0?state.frame.bakedDaylight
        :BakedDaylightScale(state.frame.relight.sunAngles.x,state.frame.relight.indirect);
    if(!(k<.98f))return;
    {ComPtr<ID3D11RenderTargetView> colorTarget;ctx->OMGetRenderTargets(1,&colorTarget,nullptr);if(!colorTarget)return;} // a depth pass
    ComPtr<ID3D11BlendState> blend;float factor[4]{};UINT mask=0xffffffff;
    ctx->OMGetBlendState(&blend,factor,&mask);
    D3D11_BLEND_DESC desc{};
    if(blend)blend->GetDesc(&desc);
    else for(auto& rt:desc.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ZERO;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;}
    if(!desc.RenderTarget[0].RenderTargetWriteMask)return; // a depth pass
    D3D11_BLEND_DESC scaled=desc;const char* kind="opaque";bool exact=true;
    for(unsigned i=0;i<(desc.IndependentBlendEnable?8u:1u);++i){
        auto& rt=scaled.RenderTarget[i];
        if(!rt.BlendEnable){
            rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_BLEND_FACTOR;rt.DestBlend=D3D11_BLEND_ZERO;rt.BlendOp=D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        }else if(rt.SrcBlend==D3D11_BLEND_ONE&&rt.BlendOp==D3D11_BLEND_OP_ADD){
            rt.SrcBlend=D3D11_BLEND_BLEND_FACTOR;kind=rt.DestBlend==D3D11_BLEND_ONE?"additive":"premultiplied";
        }else{exact=false;kind=rt.SrcBlend==D3D11_BLEND_SRC_ALPHA?"alpha":"other";}
    }
    // In the main view an additive or premultiplied card is dimmed only in CS2's transparency colour
    // pass: an RGB target other than the scene's. Its moment pass (single- and two-channel float
    // targets) keeps the coverage, and additive glows drawn straight into the scene keep their light.
    bool transparencyColour=false;
    if(world&&exact&&std::strcmp(kind,"opaque")){
        ComPtr<ID3D11RenderTargetView> target;ctx->OMGetRenderTargets(1,&target,nullptr);
        if(target){
            D3D11_RENDER_TARGET_VIEW_DESC td{};target->GetDesc(&td);ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
            const auto f=td.Format;
            const bool rgb=f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT||f==DXGI_FORMAT_R11G11B10_FLOAT||
                f==DXGI_FORMAT_R10G10B10A2_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
                f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            transparencyColour=rgb&&state.sceneTarget&&resource.Get()!=state.sceneTarget;
        }
    }
    const bool skip=world&&!transparencyColour&&(exact||std::strcmp(kind,"alpha"));
    ID3D11BlendState* applied=nullptr;
    ComPtr<ID3D11PixelShader> scaledPs;bool variantPending=false;
    if(!skip&&!exact&&!std::strcmp(kind,"alpha")){
        ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
        UINT bytes=sizeof(ID3D11PixelShader*);
        if(ps&&FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps.Get()),ScaledColorShaderTag,&bytes,(void**)scaledPs.GetAddressOf())))scaledPs.Reset();
        if(ps&&!scaledPs)variantPending=QueueScaledColorVariant(ps.Get());
        if(scaledPs){
            // One 16-byte buffer, rewritten when the factors change (they move with the time of day).
            static ComPtr<ID3D11Buffer> constants;static float last[2]{-1,-1};
            if(!constants){ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
                D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(device)device->CreateBuffer(&d,nullptr,&constants);}
            const float factors[4]{k,(std::max)(std::sqrt(k),.25f),0,0};
            if(constants&&(factors[0]!=last[0]||factors[1]!=last[1])){ctx->UpdateSubresource(constants.Get(),0,nullptr,factors,0,0);last[0]=factors[0];last[1]=factors[1];}
            if(constants){
                ctx->PSGetConstantBuffers(ScaledColorSlot,1,&previousScaledConstants);
                auto bound=constants.Get();ctx->PSSetConstantBuffers(ScaledColorSlot,1,&bound);
                previousScaledShader=ps.Detach();ctx->PSSetShader(scaledPs.Get(),nullptr,0);
                skyboxScaled=true;context=ctx;
            }
        }
    }
    if(!skip&&exact){
        // A few blend states per map; render thread only.
        static std::vector<std::pair<D3D11_BLEND_DESC,ComPtr<ID3D11BlendState>>> made;
        for(auto& entry:made)if(!std::memcmp(&entry.first,&scaled,sizeof(scaled))){applied=entry.second.Get();break;}
        if(!applied&&made.size()<64){
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);ComPtr<ID3D11BlendState> created;
            if(device&&SUCCEEDED(device->CreateBlendState(&scaled,&created))){made.push_back({scaled,created});applied=created.Get();}
        }
    }
    if(MvmDebugLog_Active()){
        static std::map<std::string,ULONGLONG> last;const auto now=GetTickCount64();
        char vsName[96]{},psName[96]{};UINT bytes=sizeof(vsName)-1;
        ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);if(vs)vs->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,vsName);
        bytes=sizeof(psName)-1;ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);if(ps)ps->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,psName);
        ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,nullptr);
        unsigned formats[8]{},bound=0;
        for(unsigned i=0;i<8;++i)if(targets[i]){D3D11_RENDER_TARGET_VIEW_DESC td{};targets[i]->GetDesc(&td);formats[i]=unsigned(td.Format);bound=i+1;targets[i]->Release();}
        char formatList[64]{};for(unsigned i=0,at=0;i<bound&&at<60;++i)at+=sprintf_s(formatList+at,sizeof(formatList)-at,i?",%u":"%u",formats[i]);
        D3D11_VIEWPORT kv{};UINT kn=1;ctx->RSGetViewports(&kn,&kv);
        const std::string key=std::string(world?"world ":"skybox ")+kind+vsName+psName+formatList+std::to_string(int(kv.Width));
        if(now-last[key]>2000){last[key]=now;
            ComPtr<ID3D11RenderTargetView> rtv;ctx->OMGetRenderTargets(1,&rtv,nullptr);D3D11_RENDER_TARGET_VIEW_DESC rd{};if(rtv)rtv->GetDesc(&rd);
            ComPtr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);D3D11_DEPTH_STENCIL_DESC dd{};if(ds)ds->GetDesc(&dd);
            const auto& rt=desc.RenderTarget[0];
            MvmDebugLog_LinefAlways("environment.unlit.card","pass=%s targets=%s vp=%.0f kind=%s skip=%d applied=%d variant=%s k=%.3f vs=%s ps=%s blend=%d src=%d dst=%d op=%d srcA=%d dstA=%d mask=%x rt=%u depthWrite=%d",
                world?"world":"skybox",formatList,kv.Width,kind,int(skip),applied||skyboxScaled?1:0,skyboxScaled?"bound":variantPending?"pending":"none",k,vsName,psName,int(rt.BlendEnable),int(rt.SrcBlend),int(rt.DestBlend),int(rt.BlendOp),int(rt.SrcBlendAlpha),int(rt.DestBlendAlpha),
                unsigned(rt.RenderTargetWriteMask),unsigned(rd.Format),int(dd.DepthWriteMask));
        }
    }
    if(skip||!applied)return;
    const float dim[4]={k,k,k,1};
    previousOverlayBlend=blend.Detach();
    std::copy(factor,factor+4,previousOverlayFactor);
    previousOverlayMask=mask;
    ctx->OMSetBlendState(applied,dim,mask);
    overlayScaled=true;context=ctx;
}
NativeMaterialLiveScope::NativeMaterialLiveScope(ID3D11DeviceContext* ctx,bool enabled){
    // THE 3D SKYBOX'S UNLIT DRAWS first, apart from everything below that concerns lit draws: taken
    // after it, Mirage's haze cards were skipped on some frames and runs without a trace.
    if(ctx&&state.live&&ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE){
        D3D11_VIEWPORT sv{};UINT n=1;ctx->RSGetViewports(&n,&sv);
        if(n==1&&sv.MinDepth>.5f&&sv.MinDepth<1&&NativeLightingViewport(sv.Width,sv.Height,sv.MinDepth,sv.MaxDepth)){
            ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
            NativeSunAbiInfo info{};UINT size=sizeof(info);
            const bool unlit=ps&&SUCCEEDED(ps->GetPrivateData(SunAbiInfoTag,&size,&info))&&size==sizeof(info)&&
                (!info.lightSlot||(info.flags&(2u|32u))!=(2u|32u))&&info.cb3Count!=1990;
            if(unlit){
                if(enabled&&state.frame.relight.enabled&&!state.reuseActive&&!state.proofActive){
                    UnlitCardDim(ctx,false);
                    if(skyboxScaled||overlayScaled)return;
                }else if(MvmDebugLog_Active()){
                    static ULONGLONG last=0;const auto now=GetTickCount64();
                    if(now-last>2000){last=now;MvmDebugLog_LinefAlways("environment.skybox.unlit.skip","enabled=%d relight=%d reuse=%d proof=%d",
                        int(enabled),int(state.frame.relight.enabled),int(state.reuseActive),int(state.proofActive));}
                }
            }
        }
    }
    // One bool decides the common case: with live lighting off this costs a single test.
    if(!enabled&&ctx&&state.live&&MvmDebugLog_Active()){
        D3D11_VIEWPORT diagnostic{};UINT n=1;ctx->RSGetViewports(&n,&diagnostic);
        if(n==1)Census(ctx,"live-disabled-hook",diagnostic);
    }
    if(ctx&&!state.proofActive&&!state.reuseActive&&PixelStateLayer::Active(ctx)){
        // The previous lit draw left its inputs bound. Anything this decision depends on ends that
        // substitution when it changes, so a draw that still finds it held needs exactly those inputs.
        // For the length of this draw the context is as a freshly set up draw leaves it: our
        // bindings visible to the code that runs inside the draw's detour, nested draws left alone.
        if(enabled&&state.live){
            PixelStateLayer::NoteReuse();++state.litDraws;
            reusing=true;state.reuseActive=true;PixelStateLayer::Suspend();return;
        }
        PixelStateLayer::Flush(PixelStateLayer::End::Ineligible);
    }
    if(!enabled||!ctx||!state.live||state.proofActive||state.reuseActive||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    LiveLapStart();
    struct LapTail{~LapTail(){LiveLap(0);}} lapTail;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    // Distant 3D skybox meshes are real surfaces too. Their separate .95..1
    // depth interval must not let baked lightmaps and probes escape replacement.
    if(count!=1||!NativeLightingViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth))return;
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!ps)return;
    // The clustered-light interface is derived from each program's bytecode, so any material
    // family that carries it is lit, whatever its vertex shader is called.
    NativeSunAbiInfo info{};UINT size=sizeof(info);
    if(FAILED(ps->GetPrivateData(SunAbiInfoTag,&size,&info))||size!=sizeof(info))return;
    // Depth/stencil passes still need their native shader for alpha rejection,
    // but have no colour output to relight. Leave every native binding intact.
    // A bound target whose blend state masks every channel is a depth pass too.
    ID3D11RenderTargetView* colorTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,colorTargets,nullptr);
    ComPtr<ID3D11BlendState> colorBlend;ctx->OMGetBlendState(&colorBlend,nullptr,nullptr);
    D3D11_BLEND_DESC colorWrites{};if(colorBlend)colorBlend->GetDesc(&colorWrites);
    bool hasColor=false;
    for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i)if(colorTargets[i]){
        if(!colorBlend||colorWrites.RenderTarget[colorWrites.IndependentBlendEnable?i:0].RenderTargetWriteMask)hasColor=true;
        colorTargets[i]->Release();
    }
    if(!hasColor){Census(ctx,"live-depth-only",vp);return;}
    if(!info.lightSlot&&info.cb3Count==1990){
        ComPtr<ID3D11PixelShader> smoke;UINT bytes=sizeof(ID3D11PixelShader*);
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),NativeSmokeTag,&bytes,(void**)smoke.GetAddressOf()))&&smoke){
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);D3D11_BUFFER_DESC desc{};if(native)native->GetDesc(&desc);
            if(device&&native&&desc.ByteWidth>=NativeSunBufferBytes&&EnsureConstantClones(device.Get(),desc.ByteWidth)&&PrepareSmokeInputs(device.Get(),ctx)){
                ctx->CopyResource(state.raw.Get(),native.Get());
                const float zero[3]{};const D3D11_BOX radiance{NativeSunRadianceOffset,0,0,NativeSunRadianceOffset+12,1,1};
                ctx->UpdateSubresource(state.raw.Get(),0,&radiance,zero,0,0);ctx->CopyResource(state.clone.Get(),state.raw.Get());
                ctx->PSGetShaderResources(120,3,previousSmokeViews);ctx->PSGetSamplers(14,2,previousSmokeSamplers);ctx->PSGetConstantBuffers(12,2,previousSmokeConstants);
                ID3D11ShaderResourceView* views[]{state.smokeLightSrv.Get(),state.proofAtlas.Get(),state.probeBinding.irradiance};ctx->PSSetShaderResources(120,3,views);
                ID3D11SamplerState* samplers[]{state.probeBinding.sampler,state.proofComparison.Get()};ctx->PSSetSamplers(14,2,samplers);
                ID3D11Buffer* constants[]{state.smokeConstants.Get(),state.smokeProbes.Get()};ctx->PSSetConstantBuffers(12,2,constants);
                auto clone=state.clone.Get();ctx->PSSetConstantBuffers(3,1,&clone);
                original=native.Detach();originalShader=ps.Detach();ctx->PSSetShader(smoke.Get(),nullptr,0);
                smokeActive=true;context=ctx;return;
            }
        }
    }
    if(!info.lightSlot||(info.flags&(2u|32u))!=(2u|32u)){
        Census(ctx,"live-no-abi",vp);
        // Additive shafts and unlit effect cards have no light slot, so the
        // material path never sees them. Scale the add by a night factor. Skybox-depth
        // draws were taken at the top of this scope (UnlitCardDim).
        if(vp.MinDepth<.5f&&state.frame.relight.enabled){
            ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);
            char name[96]{};UINT nameBytes=sizeof(name)-1;
            const bool named=vs&&SUCCEEDED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&nameBytes,name));
            // The map's own effect cards (Mirage's haze over the far rooftops, drawn through CS2's
            // order-independent transparency) and unlit cards, in the main view, not the gun's.
            if(named&&vp.MaxDepth>.5f&&(!strcmp(name,"csgo_effects.vfx_vs")||!strcmp(name,"csgo_unlitgeneric.vfx_vs"))){
                UnlitCardDim(ctx,true);if(skyboxScaled)return;
            }
            const bool sprite=named&&(!strcmp(name,"spritecard.vfx_vs")||!strcmp(name,"csgo_effects.vfx_vs"));
            if(sprite){
                ComPtr<ID3D11BlendState> blend;float factor[4]{};UINT mask=0xffffffff;
                ctx->OMGetBlendState(&blend,factor,&mask);
                D3D11_BLEND_DESC blendDesc{};if(blend)blend->GetDesc(&blendDesc);
                const auto& rt=blendDesc.RenderTarget[0];
                const bool additive=rt.BlendEnable&&rt.DestBlend==D3D11_BLEND_ONE
                    &&(rt.SrcBlend==D3D11_BLEND_ONE||rt.SrcBlend==D3D11_BLEND_SRC_ALPHA);
                const float scale=UnlitOverlayScale(state.frame.relight.sunAngles.x,state.frame.relight.indirect);
                if(additive&&scale<.98f){
                    if(!state.unlitOverlayBlend){
                        auto scaled=blendDesc;scaled.RenderTarget[0].SrcBlend=D3D11_BLEND_BLEND_FACTOR;
                        scaled.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_BLEND_FACTOR;
                        scaled.RenderTarget[0].DestBlend=D3D11_BLEND_ONE;
                        scaled.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE;
                        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
                        if(device)device->CreateBlendState(&scaled,&state.unlitOverlayBlend);
                    }
                    if(state.unlitOverlayBlend){
                        const float dim[4]={scale,scale,scale,scale};
                        previousOverlayBlend=blend.Detach();
                        std::copy(factor,factor+4,previousOverlayFactor);
                        previousOverlayMask=mask;
                        ctx->OMSetBlendState(state.unlitOverlayBlend.Get(),dim,mask);
                        overlayScaled=true;context=ctx;
                        if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();
                            if(now-last>1000){last=now;MvmDebugLog_LinefAlways("environment.unlit.overlay","shader=%s scale=%.3f elevation=%.2f",name,scale,state.frame.relight.sunAngles.x);}}
                    }
                }
            }
        }
        return;
    }
    if(MvmDebugLog_Active()){
        static std::map<uint64_t,bool> reported;
        if(reported.size()<512&&!reported[info.programHash]){
            reported[info.programHash]=true;
            MvmDebugLog_LinefAlways("relight.live.program","hash=%016llx flags=%u light=%u lightmap=%u probe=%u cascades=%u shadows=%u indirect=%g",
                info.programHash,info.flags,info.lightSlot,info.lightmapArrayMask,info.probeSlot,unsigned(info.ownedCascades),
                unsigned(state.frame.relight.shadows),state.frame.relight.indirect);
        }
    }
    if(state.frame.relight.programFilter==1&&!info.lightmapArrayMask)return;
    if(state.frame.relight.programFilter==2&&info.lightmapArrayMask)return;
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
    ComPtr<ID3D11HullShader> hs;ctx->HSGetShader(&hs,nullptr,nullptr);
    ComPtr<ID3D11DomainShader> ds;ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds){Census(ctx,"live-extra-stages",vp);return;}
    ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);
    D3D11_BUFFER_DESC desc{};if(native)native->GetDesc(&desc);
    if(!native||desc.ByteWidth<NativeSunBufferBytes||desc.ByteWidth>65536||desc.ByteWidth%16){Census(ctx,"live-cb3",vp);return;}
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!device||!EnsureConstantClones(device.Get(),info.ownedCascades?(std::max)(desc.ByteWidth,OwnedSunBufferBytes):desc.ByteWidth)){Census(ctx,"live-alloc",vp);return;}
    if(vp.MaxDepth>.1f+1e-6f)RestoreWorldSunTile(ctx);
    if(!BindOwnedInputs(ctx,device.Get(),vp,native.Get(),info)){RestoreOwnedInputs(ctx);Census(ctx,"live-bind-failed",vp);return;}
    // First-person draws read a close sun cascade redrawn in their own pose (once per frame). Only
    // after BindOwnedInputs, which prepares this frame's shadow maps: when the first-person block was
    // the frame's first lit draw, the pass backed up the previous frame's tile and the world was then
    // restored to it, so every body near the camera lost its sun shadow in first person (2026-10-13).
    if(viewmodelShadowPass&&vp.MaxDepth<=.1f+1e-6f&&state.frame.relight.shadows&&state.frame.relight.sunShadows)viewmodelShadowPass(ctx);
    if(state.frame.relight.bindingCheck){
        ComPtr<ID3D11PixelShader> coverage;UINT bytes=sizeof(ID3D11PixelShader*);
        const auto& tag=state.frame.relight.bindingCheck==2?BlackCoverageShaderTag:CoverageShaderTag;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),tag,&bytes,(void**)coverage.GetAddressOf()))&&coverage){
            originalShader=ps.Detach();ctx->PSSetShader(coverage.Get(),nullptr,0);
        }
    }else if((info.lightmapArrayMask&2)||info.probeSlot){
        ComPtr<ID3D11PixelShader> dynamic;UINT bytes=sizeof(ID3D11PixelShader*);
        const auto& probe=vp.MinDepth>0?state.skyboxBinding:state.probeBinding;
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),DynamicIrradianceTag,&bytes,(void**)dynamic.GetAddressOf()))&&dynamic&&probe.irradiance){
            const auto& packing=probe.packing;
            std::array<float,40> rows{};
            for(unsigned axis=0;axis<3;++axis){
                rows[axis]=packing.probe[axis][axis];rows[4+axis]=packing.probe[axis][3];
                rows[8+axis]=packing.volume[3][axis]*(axis==2?6.f:1.f);
                rows[12+axis]=packing.volume[4][axis]*(axis==2?6.f:1.f);
            }
            rows[15]=probe.distance?1.f:0.f;
            for(unsigned axis=0;axis<3;++axis)rows[32+axis]=packing.probe[axis][axis]*probe.normalBias;
            ID3D11ShaderResourceView* screen=nullptr;
            if(AmbientViewport(vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth)&&state.frame.hasWorldToClip&&state.frame.relight.screenGi){
                UINT width=0,height=0;auto positions=AmbientOcclusionPositions(ctx,width,height);
                screen=Relight::PrepareScreenGi(device.Get(),ctx,probe,positions,width,height,
                    state.frame.camera,state.frame.worldToClip,state.probeFrame);
                if(screen){rows[15]=2;std::copy(state.frame.worldToClip.begin(),state.frame.worldToClip.end(),rows.begin()+16);
                    rows[36]=float(width);rows[37]=float(height);rows[38]=vp.Width/width;rows[39]=vp.Height/height;}
            }
            if(!state.irradianceConstants||rows!=state.irradiancePacking){
                D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(rows);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                d.Usage=D3D11_USAGE_DEFAULT;
                if(!state.irradianceConstants)device->CreateBuffer(&d,nullptr,&state.irradianceConstants);
                if(state.irradianceConstants){
                    ctx->UpdateSubresource(state.irradianceConstants.Get(),0,nullptr,rows.data(),0,0);
                    state.irradiancePacking=rows;
                }
            }
            if(state.irradianceConstants){
                ctx->PSGetConstantBuffers(13,1,&previousIrradianceConstants);
                ctx->PSGetShaderResources(126,1,&previousIrradiance);
                ctx->PSGetShaderResources(125,1,&previousDistance);
                ctx->PSGetShaderResources(124,1,&previousWorldIrradiance);
                ctx->PSGetSamplers(15,1,&previousIrradianceSampler);
                auto constants=state.irradianceConstants.Get();auto volume=screen?screen:probe.irradiance;auto sampler=probe.sampler;
                ctx->PSSetConstantBuffers(13,1,&constants);ctx->PSSetShaderResources(126,1,&volume);ctx->PSSetSamplers(15,1,&sampler);
                auto distance=probe.distance;ctx->PSSetShaderResources(125,1,&distance);
                auto world=probe.irradiance;ctx->PSSetShaderResources(124,1,&world);
                ourIrradiance[0]=world;ourIrradiance[1]=distance;ourIrradiance[2]=volume;ourIrradianceSampler=sampler;
                originalShader=ps.Detach();ctx->PSSetShader(dynamic.Get(),nullptr,0);
                irradianceActive=true;
            }
        }else Census(ctx,"live-irradiance-unsupported",vp);
    }
    if(!originalShader){
        ComPtr<ID3D11PixelShader> atmosphere;UINT bytes=sizeof(ID3D11PixelShader*);
        if(SUCCEEDED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),NativeAtmosphereTag,&bytes,(void**)atmosphere.GetAddressOf()))&&atmosphere){
            originalShader=ps.Detach();ctx->PSSetShader(atmosphere.Get(),nullptr,0);
        }
    }
    BindAoView(ctx,device.Get(),vp);
    const bool sharedLighting=FinalizeOwnedConstants(ctx,device.Get(),native.Get());
    InspectLiveInputs(ctx,device.Get(),info,irradianceActive);
    context=ctx;original=native.Detach();
    // Live lighting already ran inside the native material draw, before MSAA
    // resolves. Report that ownership just as the HDR replay does: the late
    // display pass must not reconstruct and relight an already resolved edge.
    state.composited=true;state.renderedGeneration=state.frame.generation;
    if(!state.sceneTarget&&vp.MinDepth==0&&vp.MaxDepth>.5f){
        ID3D11RenderTargetView* target=nullptr;ctx->OMGetRenderTargets(1,&target,nullptr);
        if(target){ComPtr<ID3D11Resource> resource;target->GetResource(&resource);state.sceneTarget=resource.Get();target->Release();}
    }
    Census(ctx,"live-lit",vp);
    BeginTiming(ctx,false,true,info.programHash,uint8_t(irradianceActive?1:originalShader?2:0),&vp);
    ++state.litDraws;
    // These inputs can stay bound for the next draw of the run unless this draw copied engine
    // constants (the unshared lighting or view fallback) or is being timed on its own.
    // Nor while the frame's ambient occlusion or sun visibility is still to come: a later draw gets
    // the real one, and setting up each draw until then is what finds the first that can prepare it.
    sticky=PixelStateLayer::Enabled()&&sharedLighting&&originalShader&&!state.viewActive&&!state.inputsProvisional&&
        state.timing.active<0&&!state.timing.measuring;
}
void NativeMaterialLiveCost(float& prepareMs,float& ambientOcclusionMs,float& totalMs){
    prepareMs=liveCostMs[0].load(std::memory_order_relaxed);ambientOcclusionMs=liveCostMs[1].load(std::memory_order_relaxed);
    totalMs=liveCostMs[2].load(std::memory_order_relaxed);
}
NativeMaterialLiveScope::~NativeMaterialLiveScope(){
    if(reusing){state.reuseActive=false;PixelStateLayer::Resume();return;}
    if(!context)return;
    if(skyboxScaled){
        context->PSSetShader(previousScaledShader,nullptr,0);
        if(previousScaledShader)previousScaledShader->Release();
        context->PSSetConstantBuffers(ScaledColorSlot,1,&previousScaledConstants);
        if(previousScaledConstants)previousScaledConstants->Release();
        return;
    }
    if(overlayScaled){
        context->OMSetBlendState(previousOverlayBlend,previousOverlayFactor,previousOverlayMask);
        if(previousOverlayBlend)previousOverlayBlend->Release();
        return;
    }
    if(!smokeActive)EndTiming(context);
    if(sticky){
        PixelStateLayer::Substitution held;bool fits=true;
        for(unsigned slot=0;slot<RenderPixelResourceBindings::Capacity&&fits;++slot)if(state.proofResources.bound[slot]){
            if(held.viewCount+3>=std::size(held.viewSlot)){fits=false;break;}
            held.viewSlot[held.viewCount]=slot;held.oursView[held.viewCount]=state.proofOurViews[slot];
            held.engineView[held.viewCount++]=state.proofResources.previous[slot].Get();
        }
        if(fits){
            // From here every saved engine binding belongs to the layer.
            for(unsigned i=0;i<held.viewCount;++i){state.proofResources.previous[held.viewSlot[i]].Detach();state.proofResources.bound[held.viewSlot[i]]=false;}
            held.engineShader=originalShader;
            held.samplerSlot[0]=state.proofSamplerSlot;held.engineSampler[0]=state.proofPreviousSampler.Detach();
            held.oursSampler[0]=state.proofComparison.Get();held.samplerCount=1;
            if(state.probeActive){held.bufferSlot[held.bufferCount]=2;held.engineBuffer[held.bufferCount]=state.previousProbeConstants.Detach();
                held.oursBuffer[held.bufferCount++]=state.proofOurProbeConstants;state.probeActive=false;}
            held.bufferSlot[held.bufferCount]=NativeLightingConstantSlot;held.engineBuffer[held.bufferCount]=state.previousSharedLighting.Detach();
            held.oursBuffer[held.bufferCount++]=state.proofOurLighting;state.sharedLightingActive=false;
            if(state.sharedAoActive){held.bufferSlot[held.bufferCount]=NativeAoConstantSlot;held.engineBuffer[held.bufferCount]=state.previousSharedAo.Detach();
                held.oursBuffer[held.bufferCount++]=state.sharedAo.Get();state.sharedAoActive=false;}
            if(irradianceActive){
                held.bufferSlot[held.bufferCount]=13;held.engineBuffer[held.bufferCount]=previousIrradianceConstants;
                held.oursBuffer[held.bufferCount++]=state.irradianceConstants.Get();
                ID3D11ShaderResourceView* const engine[3]{previousWorldIrradiance,previousDistance,previousIrradiance};
                for(unsigned i=0;i<3;++i){held.viewSlot[held.viewCount]=124+i;held.engineView[held.viewCount]=engine[i];held.oursView[held.viewCount++]=ourIrradiance[i];}
                if(state.proofSamplerSlot==15){
                    // One slot, bound twice: the engine's sampler is the first one saved.
                    if(previousIrradianceSampler)previousIrradianceSampler->Release();
                    held.oursSampler[0]=ourIrradianceSampler;
                }else{held.samplerSlot[1]=15;held.engineSampler[1]=previousIrradianceSampler;held.oursSampler[1]=ourIrradianceSampler;held.samplerCount=2;}
            }
            held.dependencySlot[0]=1;held.dependencySlot[1]=3;held.dependencySlot[2]=4;held.dependencyCount=3;
            original->Release();state.proofActive=false;
            PixelStateLayer::Adopt(context,held);
            return;
        }
    }
    if(originalShader){
        context->PSSetShader(originalShader,nullptr,0);originalShader->Release();
    }
    if(smokeActive){
        context->PSSetShaderResources(120,3,previousSmokeViews);context->PSSetSamplers(14,2,previousSmokeSamplers);context->PSSetConstantBuffers(12,2,previousSmokeConstants);
        for(auto resource:previousSmokeViews)if(resource)resource->Release();
        for(auto sampler:previousSmokeSamplers)if(sampler)sampler->Release();
        for(auto constant:previousSmokeConstants)if(constant)constant->Release();
        context->PSSetConstantBuffers(3,1,&original);original->Release();return;
    }
    if(irradianceActive){
        context->PSSetConstantBuffers(13,1,&previousIrradianceConstants);
        context->PSSetShaderResources(126,1,&previousIrradiance);
        context->PSSetShaderResources(125,1,&previousDistance);
        context->PSSetShaderResources(124,1,&previousWorldIrradiance);
        context->PSSetSamplers(15,1,&previousIrradianceSampler);
        if(previousIrradianceConstants)previousIrradianceConstants->Release();
        if(previousIrradiance)previousIrradiance->Release();
        if(previousDistance)previousDistance->Release();
        if(previousWorldIrradiance)previousWorldIrradiance->Release();
        if(previousIrradianceSampler)previousIrradianceSampler->Release();
    }
    context->PSSetConstantBuffers(3,1,&original);original->Release();
    RestoreOwnedInputs(context);
}
bool NativeMaterialDrawWorkNeeded() {
    return state.frame.active || state.wanted || state.unbake || state.live;
}
bool BeginNativeMaterialDraw(ID3D11DeviceContext* ctx){
    if(ctx&&MvmDebugLog_Active()&&(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!state.wanted||state.active)){
        D3D11_VIEWPORT v{};UINT c=1;ctx->RSGetViewports(&c,&v);
        Census(ctx,ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE?"deferred":!state.wanted?"not-wanted":"nested",v);}
    if(!ctx||!state.wanted||state.active||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    D3D11_VIEWPORT vp{};UINT count=1;ctx->RSGetViewports(&count,&vp);
    if(count==1&&BeginTonemap(ctx,vp))return true;
    const bool viewmodel=std::abs(vp.MaxDepth-.1f)<1e-6f;
    if(count!=1||vp.Width<256||vp.Height<256||vp.MinDepth!=0||(!viewmodel&&std::abs(vp.MaxDepth-.95f)>1e-6f)){Census(ctx,"viewport",vp);return false;}
    Census(ctx,"viewport-ok",vp);
    ComPtr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,nullptr,nullptr);char name[128]{};UINT size=sizeof(name)-1;
    if(!vs||FAILED(vs->GetPrivateData(WKPDID_D3DDebugObjectName,&size,name)))return false;
    const bool supported=viewmodel?(!strcmp(name,"csgo_weapon.vfx_vs")||!strcmp(name,"csgo_character.vfx_vs")):
        (!strcmp(name,"csgo_lightmappedgeneric.vfx_vs")||!strcmp(name,"csgo_vertexlitgeneric.vfx_vs")||
         !strcmp(name,"csgo_complex.vfx_vs")||!strcmp(name,"csgo_simple.vfx_vs")||!strcmp(name,"csgo_simple_2way_blend.vfx_vs")||!strcmp(name,"csgo_static_overlay.vfx_vs"));
    if(!supported){Evidence((std::string("unsupported-family:")+name).c_str());return false;}
    ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);UINT tag=0;size=sizeof(tag);
    if(MvmDebugLog_Active()&&ps){
        NativeSunAbiInfo info{};UINT infoSize=sizeof(info);ps->GetPrivateData(SunAbiInfoTag,&infoSize,&info);
        static std::set<uint64_t> seen;
        if(seen.size()<64&&seen.insert(info.programHash).second)
            MvmDebugLog_LinefAlways("environment.native.material.abi","vs=%s flags=%u cb3Count=%u hash=%016llx",name,info.flags,info.cb3Count,info.programHash);
    }
    // A program still waiting for its variants goes to the front of the derivation queue.
    if(ps){UINT queued=0;if(ps->GetPrivateData(SunAbiInfoTag,&queued,nullptr)==DXGI_ERROR_NOT_FOUND)PrioritizeNativeMaterial(ps.Get());}
    if(!ps||FAILED(ps->GetPrivateData(SunAbiTag,&size,&tag))||tag!=1){Evidence("unsupported-sun-abi");return false;}
    NativeSunAbiInfo fixtureInfo{};UINT fixtureInfoSize=sizeof(fixtureInfo);
    state.fixtureAbi=SUCCEEDED(ps->GetPrivateData(SunAbiInfoTag,&fixtureInfoSize,&fixtureInfo))&&(fixtureInfo.flags&32);
    if(state.fixturePass&&!state.fixtureAbi)Evidence("unsupported-fixture-abi:retained-native-baseline");
    const bool owned=state.frame.proof.enabled||state.frame.relight.enabled;
    if(owned&&!fixtureInfo.lightSlot){Evidence("owned:no-derived-fixture-abi-native-retained");return false;}
    state.coveragePs.Reset();size=sizeof(ID3D11PixelShader*);
    if(FAILED(::Filmmaker::GetOwnedInterface(::Filmmaker::RawOwner(ps),CoverageShaderTag,&size,(void**)state.coveragePs.GetAddressOf()))||!state.coveragePs){Evidence("unsupported-native-coverage");return false;}
    ComPtr<ID3D11Buffer> native;ctx->PSGetConstantBuffers(3,1,&native);D3D11_BUFFER_DESC bufferDesc{};
    if(native)native->GetDesc(&bufferDesc);
    if(bufferDesc.ByteWidth<NativeSunBufferBytes||bufferDesc.ByteWidth>65536||bufferDesc.ByteWidth%16){Evidence("lighting-buffer-size",bufferDesc.ByteWidth);return false;}
    ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
    ComPtr<ID3D11HullShader> hs;ctx->HSGetShader(&hs,nullptr,nullptr);
    ComPtr<ID3D11DomainShader> ds;ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs||hs||ds){Evidence("extra-shader-stages");return false;}
    ComPtr<ID3D11BlendState> blend;float factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);
    D3D11_BLEND_DESC nativeBlend{};
    if(blend)blend->GetDesc(&nativeBlend);else nativeBlend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    // Preserve native blend equations in both HDR passes. Coverage runs the same
    // PS and alpha-to-coverage, but writes a validity bit without color blending.
    if(nativeBlend.IndependentBlendEnable){Evidence("independent-blend-pass");return false;}
    // Alpha-to-coverage retains native per-sample coverage. Blended overlays still
    // require a validated ordering contract before replacing their background.
    if(owned&&nativeBlend.RenderTarget[0].BlendEnable){Evidence("owned:blended-material-not-yet-validated");return false;}
    ComPtr<ID3D11RenderTargetView> targets[8];ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* rawTargets[8]{};ctx->OMGetRenderTargets(8,rawTargets,depth.GetAddressOf());
    for(unsigned i=0;i<8;++i)targets[i].Attach(rawTargets[i]);
    if(!targets[0]||!depth){Evidence("no-color-or-depth");return false;}
    for(unsigned i=1;i<8;++i)if(targets[i]){Evidence("multiple-render-targets");return false;}
    ComPtr<ID3D11Resource> resource,depthResource;targets[0]->GetResource(&resource);depth->GetResource(&depthResource);
    ComPtr<ID3D11Texture2D> texture,depthTexture;
    if(FAILED(resource.As(&texture))||FAILED(depthResource.As(&depthTexture)))return false;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC rt{};targets[0]->GetDesc(&rt);
    if(td.ArraySize!=1||td.MipLevels!=1||td.Width!=UINT(vp.Width)||td.Height!=UINT(vp.Height)){Evidence("target-dimensions");return false;}
    if(state.captured&&(state.source.Get()!=texture.Get()||state.depth.Get()!=depthTexture.Get()))return false;
    ComPtr<ID3D11DepthStencilState> previousDepth;UINT stencil=0;ctx->OMGetDepthStencilState(&previousDepth,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd{};
    if(previousDepth)previousDepth->GetDesc(&dd);else {dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;}
    // Native forward materials can use a read-only EQUAL pass after the depth prepass.
    if(!dd.DepthEnable){Evidence("depth-disabled");return false;}
    if(dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO)dd.DepthFunc=D3D11_COMPARISON_EQUAL;
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilWriteMask=0;
    ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!Resources(device.Get(),td,rt,bufferDesc.ByteWidth)){Evidence("resource-allocation");return false;}
    auto coverageBlend=nativeBlend;coverageBlend.RenderTarget[0].BlendEnable=FALSE;coverageBlend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    if(!state.maskBlend||std::memcmp(&coverageBlend,&state.maskBlendDesc,sizeof(coverageBlend))){
        state.maskBlend.Reset();if(FAILED(device->CreateBlendState(&coverageBlend,&state.maskBlend)))return false;state.maskBlendDesc=coverageBlend;
    }
    if(!state.depthState||std::memcmp(&dd,&state.depthDesc,sizeof(dd))){
        state.depthState.Reset();if(FAILED(device->CreateDepthStencilState(&dd,&state.depthState)))return false;state.depthDesc=dd;}
    // Clone on the GPU: no readback stall and no mutation of engine-owned values.
    BeginTiming(ctx,false);
    ctx->CopyResource(state.raw.Get(),native.Get());
    if(owned&&MvmDebugLog_Active()) {
        static uint64_t sampled=~0ull;
        if(sampled!=state.frame.relight.generation) {
            sampled=state.frame.relight.generation;
            D3D11_BUFFER_DESC d{};native->GetDesc(&d);
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
            ComPtr<ID3D11Buffer> readback;
            if(SUCCEEDED(device->CreateBuffer(&d,nullptr,&readback))) {
                ctx->CopyResource(readback.Get(),native.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                if(SUCCEEDED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped))) {
                    const float* values=static_cast<const float*>(mapped.pData);
                    MvmDebugLog_LinefAlways("relight.native.reference","generation=%llu sunDirection=(%g %g %g) sunRadiance=(%g %g %g) ambientFloor=(%g %g %g)",
                        sampled,values[1945*4],values[1945*4+1],values[1945*4+2],values[1946*4],values[1946*4+1],values[1946*4+2],values[18*4],values[18*4+1],values[18*4+2]);
                    ctx->Unmap(readback.Get(),0);
                }
            }
        }
    }
    auto patchRange=[&](UINT offset,UINT bytes,const void* value){
        const D3D11_BOX range{offset,0,0,offset+bytes,1,1};
        ctx->UpdateSubresource(state.raw.Get(),0,&range,value,0,0);
    };
    if(!state.frame.relight.reference) {
    patchRange(NativeSunDirectionOffset,12,state.next.direction);
    patchRange(NativeSunRadianceOffset,12,state.next.radiance);
    const UINT noNativeCascades=0;
    patchRange(NativeSunCascadesOffset,sizeof(noNativeCascades),&noNativeCascades);
    if(owned){
        if(!ProofResources(device.Get(),ctx,vp)){Evidence("owned:resource-allocation-failed");return false;}
        const UINT zeros[4]{};
        patchRange(NativeSunRadianceOffset,12,zeros);
        // cb3[0..3] carry the shadow filter kernel. Zeroing them, as the single-fixture
        // proof did, collapses all nine taps onto one texel and every shadow becomes a
        // hard binary edge.
        float filterRows[16];
        Relight::ShadowFilterRows(state.frame.relight.enabled?Relight::NativeFilterTexels:0.f,filterRows);
        patchRange(0,sizeof(filterRows),filterRows);
        const float bias=state.frame.relight.enabled?-state.frame.relight.shadowBias:-.0002f;patchRange(1953*16+8,4,&bias);
        // cb3[19].z is how many 32-bit mask words the clustered loop reads,
        // so it is what raises capacity past a single light.
        const UINT words=state.frame.relight.enabled&&state.frame.relight.cost==1?1:state.proofMaskWords;
        auto cull=state.clusterGrid.ListRow(state.clusterGrid.lights,state.clusterMode);cull[2]=words;
        const auto tiles=state.clusterGrid.TileConstants(state.clusterMode);
        const auto slices=state.clusterGrid.SliceConstants(state.clusterMode);
        patchRange(19*16,16,cull.data());patchRange(20*16,16,zeros);patchRange(21*16,16,zeros);
        patchRange(22*16,16,tiles.data());patchRange(23*16,16,slices.data());
        // GPU validates cb1[20] before publishing the real grid. An unsupported
        // viewport receives the identical full-mask constants without a readback stall.
        if(state.clusterMode) {
            const D3D11_BOX rows{0,0,0,80,1,1};
            ctx->CopySubresourceRegion(state.raw.Get(),0,19*16,0,0,state.clusterRows.Get(),0,&rows);
        }
        float ambient[12]{};
        if(state.frame.relight.enabled){
            // A sky-over-ground hemisphere rather than a flat constant, so an upward
            // face sees the sky and a downward face sees the bounce. The native shader
            // multiplies this by the surface albedo on our behalf.
            const auto& set=state.frame.relight;
            {const auto fill=Relight::AmbientFill(set);Relight::AmbientRows(fill[2],fill[5],ambient);}
        }else{
            const float fill=state.frame.proof.indirect;
            const float flat[12]{0,0,0,fill,0,0,0,fill,0,0,0,fill};
            std::copy(std::begin(flat),std::end(flat),std::begin(ambient));
        }
        if(state.frame.relight.enabled&&state.frame.relight.probeCheck)
            Relight::AmbientRows(Relight::ProbeNeutral,Relight::ProbeNeutral,ambient);
        patchRange(7*16,sizeof(ambient),ambient);
        // cb3[18] is a flat ambient floor the shader adds regardless of sun visibility
        // or surface orientation, and it is the channel that actually reaches an unlit
        // face -- the hemisphere rows above only fill the share the probe loop leaves
        // behind. Because it is flat, whatever goes here is added to every surface
        // equally, so putting the hemisphere AVERAGE in it lights the shadowed side of a
        // wall exactly as brightly as the lit side and the whole frame reads as
        // fullbright. Each row holds (sky+ground)/2 in .w and (sky-ground)/2 in .z, so
        // .w-.z is the ground term: the least any face can receive. The sky share is left
        // to the orientation-dependent rows, which is where it belongs.
        const float floorTerm[4]{ambient[3]-ambient[2],ambient[7]-ambient[6],
                                 ambient[11]-ambient[10],0};
        if(state.frame.relight.enabled&&!(fixtureInfo.lightmapArrayMask&2))patchRange(18*16,16,floorTerm);
        else patchRange(18*16,16,zeros);
        ctx->CopyResource(state.clone.Get(),state.raw.Get());
        state.proofLightSlot=fixtureInfo.lightSlot;
        // t7 is the engine's screen-space ambient occlusion, a multiplier on the ambient
        // term. It is left alone deliberately. Replacing it with black deletes all ambient
        // light, and replacing it with white -- which this did -- deletes every contact
        // shadow and crevice in the frame, which is most of what makes a surface read as
        // lit at all. It is a screen-space term computed from the depth buffer, so it
        // describes the geometry in front of the camera and not the lighting we replaced;
        // it stays valid when the light set changes and is the only occlusion the small
        // detail in the frame gets.
        UINT slots[10]{12,13,state.proofLightSlot,6};
        ID3D11ShaderResourceView* views[10]{state.proofBlackCube.Get(),state.proofMaskSrv.Get(),state.proofLightSrv.Get(),state.proofAtlas.Get()};
        // Neutralise only the baked arrays this program actually declares. A slot it
        // types differently has to keep whatever the engine bound there.
        unsigned bindings=4;
        const auto& probe=state.probeBinding;
        if(probe.constants&&fixtureInfo.probeSlot) {
            // All table entries name our one map-wide probe. The shared tile/slice
            // grid can change independently; there is no probe-grid capacity limit.
            const UINT table[4]{state.clusterGrid.probes.slices,state.clusterGrid.probes.tiles,1,0};
            patchRange(21*16,16,table);patchRange(1177*16,6*16,probe.packing.probe.data());
            patchRange(1947*16,16,zeros);
            // The traced sky already includes occlusion. A flat floor would add that
            // energy back in shadow; retain it only for the neutral ABI comparison.
            if(!state.frame.relight.probeCheck)patchRange(18*16,16,zeros);
            ctx->PSGetConstantBuffers(2,1,state.previousProbeConstants.ReleaseAndGetAddressOf());
            auto cb2=probe.constants;ctx->PSSetConstantBuffers(2,1,&cb2);state.probeActive=true;
            slots[bindings]=fixtureInfo.probeSlot;views[bindings++]=probe.irradiance;
            if(fixtureInfo.probeOcclusionSlot){slots[bindings]=fixtureInfo.probeOcclusionSlot;views[bindings++]=probe.sunOcclusion;}
            if(MvmDebugLog_Active()) {
                static uint64_t logged=~0ull;
                if(logged!=state.frame.relight.generation){logged=state.frame.relight.generation;
                    MvmDebugLog_LinefAlways("relight.probe","generation=%llu hash=%016llx cb2=%p irradianceSlot=%u occlusionSlot=%u neutral=%d",
                        logged,fixtureInfo.programHash,cb2,fixtureInfo.probeSlot,fixtureInfo.probeOcclusionSlot,state.frame.relight.probeCheck==2);}
            }
        }
        for(unsigned array=0;array<3;++array)if(fixtureInfo.lightmapArrayMask&(1u<<array)){
            // t3 is geometric bent-normal data, not radiance. Keep its normal response.
            // Send owned fill through t2 so native diffuse energy and ambient occlusion
            // apply; cb3[18] bypasses those terms and made recesses uniformly bright.
            if(state.frame.relight.enabled&&array==2)continue;
            slots[bindings]=array+1;
            // Once per session, record the engine's lightmap dimensions. Calculated world
            // irradiance must eventually match this atlas layout, not our diagnostic size.
            if(array==1&&!state.engineLightmapWidth){
                ComPtr<ID3D11ShaderResourceView> engine;
                ctx->PSGetShaderResources(2,1,&engine);
                if(engine){
                    ComPtr<ID3D11Resource> resource;engine->GetResource(&resource);
                    ComPtr<ID3D11Texture2D> texture;if(resource&&SUCCEEDED(resource.As(&texture))){
                        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                        state.engineLightmapWidth=desc.Width;state.engineLightmapHeight=desc.Height;
                        state.engineLightmapArray=desc.ArraySize;
                        if(MvmDebugLog_Active())
                            MvmDebugLog_LinefAlways("relight.lightmap.engine","width=%u height=%u array=%u format=%u",
                                desc.Width,desc.Height,desc.ArraySize,unsigned(desc.Format));
                    }
                }
            }
            views[bindings]=state.frame.relight.enabled&&array==1?state.relightAmbientSrv.Get():state.proofBlackArray.Get();
            ++bindings;
        }
        state.proofResources.Bind(ctx,bindings,slots,views);
        state.proofSamplerSlot=fixtureInfo.comparisonSampler;ctx->PSGetSamplers(state.proofSamplerSlot,1,state.proofPreviousSampler.ReleaseAndGetAddressOf());auto sampler=state.proofComparison.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);
        state.proofActive=true;
        const uint64_t generation=state.frame.relight.enabled?state.frame.relight.generation:state.frame.proof.generation;
        static uint64_t logged=~0ull;if(logged!=generation){logged=generation;MvmDebugLog_LinefAlways("environment.owned","generation=%llu bound hash=%016llx slot=%u sampler=%u records=%u words=%u",generation,fixtureInfo.programHash,fixtureInfo.lightSlot,fixtureInfo.comparisonSampler,state.proofRecordCount,state.proofMaskWords);}
    }
    ctx->CopyResource(state.clone.Get(),state.raw.Get());
    }
    if(state.frame.relight.reference)ctx->CopyResource(state.clone.Get(),state.raw.Get());
    if(!state.captured){const float clear[4]{};ctx->ClearRenderTargetView(state.rtv.Get(),clear);ctx->ClearRenderTargetView(state.coverageRtv.Get(),clear);ctx->ClearRenderTargetView(state.baselineRtv.Get(),clear);
        if(state.fixturePass)ctx->ClearRenderTargetView(state.withoutFixturesRtv.Get(),clear);
        state.rendered=state.next;state.renderedGeneration=state.generation;}
    state.previousConstants=native;state.previousRtv=targets[0];state.previousDsv=depth;
    state.previousDepthState=previousDepth;state.previousStencil=stencil;
    state.previousBlend=blend;std::memcpy(state.previousFactors,factors,sizeof(factors));state.previousMask=mask;
    auto cb=state.clone.Get();auto target=state.rtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,depth.Get());ctx->OMSetDepthStencilState(state.depthState.Get(),stencil);
    ctx->OMSetBlendState(blend.Get(),factors,mask);
    if(MvmDebugLog_Active()){static bool once=false;if(!once){once=true;char textureName[256]{};UINT length=255;texture->GetPrivateData(WKPDID_D3DDebugObjectName,&length,textureName);
        MvmDebugLog_LinefAlways("environment.native.material.target","name=%s resource=%p textureFormat=%u viewFormat=%u samples=%u",textureName,texture.Get(),td.Format,rt.Format,td.SampleDesc.Count);}}
    state.source=texture;state.depth=depthTexture;state.sourceDsv=depth;state.worldActive=!viewmodel;state.active=true;return true;
}
bool BeginNativeMaterialBaseline(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive)return false;
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){state.proofResources.Restore(ctx);auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;}
    auto cb=state.previousConstants.Get();auto target=state.baselineRtv.Get();
    ctx->PSSetConstantBuffers(3,1,&cb);ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());return true;
}
bool BeginNativeMaterialWithoutFixtures(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive||state.proofActive||!state.fixturePass)return false;
    auto cb=state.previousConstants.Get();
    if(state.fixtureAbi){
        ++state.fixtureDraws;
        // Clone the ORIGINAL baseline, not the custom sun pass. Only clustered
        // direct evaluation changes; native textures, emission, shadow/cookie data,
        // coverage and blending remain identical in this differential pair.
        ctx->CopyResource(state.raw.Get(),state.previousConstants.Get());
        const UINT zero=0;const D3D11_BOX range{NativeFixtureWordsOffset,0,0,NativeFixtureWordsOffset+4,1,1};
        ctx->UpdateSubresource(state.raw.Get(),0,&range,&zero,0,0);
        ctx->CopyResource(state.clone.Get(),state.raw.Get());cb=state.clone.Get();
    }
    auto target=state.withoutFixturesRtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());return true;
}
bool BeginNativeMaterialCoverage(ID3D11DeviceContext* ctx){
    if(!state.active||state.postActive||state.proofActive||state.coverageActive)return false;
    auto cb=state.previousConstants.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->PSGetShader(&state.previousPs,nullptr,nullptr);
    auto target=state.coverageRtv.Get();ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());
    ctx->PSSetShader(state.coveragePs.Get(),nullptr,0);ctx->OMSetBlendState(state.maskBlend.Get(),state.previousFactors,state.previousMask);state.coverageActive=true;return true;
}
void EndNativeMaterialDraw(ID3D11DeviceContext* ctx){
    if(!state.active||!ctx)return;
    EndTiming(ctx);
    if(state.probeActive){auto cb2=state.previousProbeConstants.Get();ctx->PSSetConstantBuffers(2,1,&cb2);state.previousProbeConstants.Reset();state.probeActive=false;}
    if(state.proofActive){
        state.proofResources.Restore(ctx);
        auto sampler=state.proofPreviousSampler.Get();ctx->PSSetSamplers(state.proofSamplerSlot,1,&sampler);state.proofPreviousSampler.Reset();state.proofActive=false;
    }
    if(state.coverageActive){ctx->PSSetShader(state.previousPs.Get(),nullptr,0);state.previousPs.Reset();state.coverageActive=false;}
    if(state.postActive){
        auto input=state.previousInput.Get();auto target=state.previousRtv.Get();ctx->PSSetShaderResources(12,1,&input);ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());
        state.previousInput.Reset();state.previousRtv.Reset();state.previousDsv.Reset();state.postActive=state.active=false;state.toned=true;Evidence("native-tonemap-replay");return;
    }
    auto cb=state.previousConstants.Get();auto target=state.previousRtv.Get();ctx->PSSetConstantBuffers(3,1,&cb);
    ctx->OMSetRenderTargets(1,&target,state.previousDsv.Get());ctx->OMSetDepthStencilState(state.previousDepthState.Get(),state.previousStencil);
    ctx->OMSetBlendState(state.previousBlend.Get(),state.previousFactors,state.previousMask);state.previousBlend.Reset();
    state.previousConstants.Reset();state.previousRtv.Reset();state.previousDsv.Reset();state.previousDepthState.Reset();
    state.active=false;state.captured=true;++state.draws;if(state.worldActive)++state.worldDraws;Evidence("native-sun-replay",NativeSunBufferBytes);
}
} // namespace Filmmaker::Environment
