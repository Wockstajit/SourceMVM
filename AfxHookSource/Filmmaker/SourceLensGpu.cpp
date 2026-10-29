#include "stdafx.h"
#undef min
#undef max
#include "SourceLensGpu.h"
#include "SourceLensShaders.h"
#include "d3d9Hooks.h"
#include "AfxShaders.h"
#include "AfxStreams.h"
#include <AfxHookSource2/Filmmaker/Lens/LensFxState.h>
#include <AfxHookSource2/Filmmaker/Lens/FisheyeMath.h>
#include <AfxHookSource2/Filmmaker/Lens/LensFxConstants.h>
#include <AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h>
#include <AfxHookSource2/Filmmaker/Environment/EnvironmentMath.h>
#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <wrl/client.h>
#include <future>
#include <atomic>
#include <array>
#include <chrono>
#include <initializer_list>
#include <stdexcept>

namespace SourceFilmmaker {
namespace {
using Microsoft::WRL::ComPtr;
using namespace Filmmaker;
using namespace Filmmaker::Lens;
struct Code { ComPtr<ID3DBlob> bytes; std::vector<LensShaders::Binding> bindings; };
std::future<std::vector<Code>> g_pending;
std::vector<Code> g_code;
bool g_started=false,g_failed=false;
HRESULT g_frameResult=S_OK;
std::atomic<bool> g_precached{false},g_depthReady{false};
std::atomic<uint64_t> g_warmedRevision{0};
struct Texture {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface;
    unsigned w=0,h=0;
};
struct Gpu {
    IDirect3DDevice9* device=nullptr; // borrowed; the host owns the device
    std::array<ComPtr<IDirect3DPixelShader9>,21> shaders;
    IDirect3DPixelShader9* fisheye=nullptr; // borrowed from HLAE
    IDirect3DVertexShader9* vertex=nullptr; // owned by HLAE's shader cache
    bool ready=false,targets=false,focusValid=false,msaa=false,depthPushed=false;
    unsigned w=0,h=0,irisSerial=0,dirtSerial=0,focusCur=0;
    D3DFORMAT format=D3DFMT_UNKNOWN;
    Texture source,soft,gather,tile[2],half,quarter,eighth,sourceQ,sourceE,sourceH,smoke;
    LensSmokeFrame smokeFrame;unsigned smokeDraws=0;
    Texture ghostSrc,ghost,streak,glow[2],focus[2],sun,iris,dirt,black;
    Texture coc[6],world[6];
    std::vector<Texture> sourceMips;
    ComPtr<IDirect3DTexture9> cocAll,worldAll;
    unsigned levels=1;
    std::chrono::steady_clock::time_point last;
    bool haveLast=false;
    ComPtr<IDirect3DSurface9> focusRead;
    unsigned frames=0,heldFrames=0;
} g;

bool Target(Texture& t,unsigned w,unsigned h,D3DFORMAT fmt,unsigned levels=1) {
    t={}; t.w=(std::max)(w,1u); t.h=(std::max)(h,1u);
    return SUCCEEDED(g.device->CreateTexture(t.w,t.h,levels,D3DUSAGE_RENDERTARGET,fmt,D3DPOOL_DEFAULT,&t.texture,nullptr))
        && SUCCEEDED(t.texture->GetSurfaceLevel(0,&t.surface));
}
bool Mask(Texture& t,const LensMaskImage& image) {
    t={};
    if (!image.width || !image.height || image.pixels.size()!=size_t(image.width)*image.height) return false;
    if (FAILED(g.device->CreateTexture(image.width,image.height,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,&t.texture,nullptr))) return false;
    D3DLOCKED_RECT lock{};
    if (FAILED(t.texture->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD))) { t={}; return false; }
    for (unsigned y=0;y<image.height;++y) {
        auto* row=(unsigned*) ((unsigned char*)lock.pBits+y*lock.Pitch);
        for (unsigned x=0;x<image.width;++x) { const unsigned v=image.pixels[y*image.width+x]; row[x]=0xff000000u|v*0x010101u; }
    }
    t.texture->UnlockRect(0); t.w=image.width; t.h=image.height; return true;
}
bool Targets(const D3DSURFACE_DESC& d) {
    if (g.targets && g.w==d.Width && g.h==d.Height && g.format==d.Format) return true;
    g.targets=false; g.focusValid=false;
    const auto hdr=D3DFMT_A16B16G16R16F;
    const unsigned w=d.Width,h=d.Height;
    g.levels=1; while(g.levels<6 && ((std::min)(w,h)>>g.levels)) ++g.levels;
    g.cocAll.Reset(); g.worldAll.Reset();
    if (FAILED(g.device->CreateTexture(w,h,g.levels,D3DUSAGE_RENDERTARGET,hdr,D3DPOOL_DEFAULT,&g.cocAll,nullptr))
        || FAILED(g.device->CreateTexture(w,h,g.levels,D3DUSAGE_RENDERTARGET,hdr,D3DPOOL_DEFAULT,&g.worldAll,nullptr))) return false;
    for (unsigned m=0;m<g.levels;++m)
        if (!Target(g.coc[m],w>>m,h>>m,hdr) || !Target(g.world[m],w>>m,h>>m,hdr)) return false;
    if (!Target(g.smoke,w,h,hdr) || !Target(g.source,w,h,d.Format) || !Target(g.soft,w,h,hdr) || !Target(g.gather,w,h,hdr)
        || !Target(g.tile[0],(w+15)/16,(h+15)/16,hdr) || !Target(g.tile[1],(w+15)/16,(h+15)/16,hdr)
        || !Target(g.half,(w+1)/2,(h+1)/2,hdr) || !Target(g.quarter,(w+3)/4,(h+3)/4,hdr)
        || !Target(g.eighth,(w+7)/8,(h+7)/8,hdr) || !Target(g.sourceQ,(w+3)/4,(h+3)/4,hdr)
        || !Target(g.sourceE,(w+7)/8,(h+7)/8,hdr,0) || !Target(g.sourceH,(w+7)/8,(h+1)/2,hdr)
        || !Target(g.ghostSrc,(w+3)/4,(h+3)/4,hdr) || !Target(g.ghost,(w+3)/4,(h+3)/4,hdr)
        || !Target(g.streak,(w+1)/2,(h+1)/2,hdr) || !Target(g.glow[0],(w+7)/8,(h+7)/8,hdr)
        || !Target(g.glow[1],(w+7)/8,(h+7)/8,hdr) || !Target(g.focus[0],1,1,D3DFMT_R32F)
        || !Target(g.focus[1],1,1,D3DFMT_R32F) || !Target(g.sun,1,1,hdr)) return false;
    g.sourceMips.clear();
    for(unsigned m=1;m<g.sourceE.texture->GetLevelCount();++m) {
        Texture next;
        if(!Target(next,(std::max)(g.sourceE.w>>m,1u),(std::max)(g.sourceE.h>>m,1u),hdr)) return false;
        g.sourceMips.push_back(std::move(next));
    }
    LensMaskImage black; black.width=black.height=1; black.pixels={0};
    if (!Mask(g.black,black)) return false;
    g.device->ColorFill(g.sun.surface.Get(),nullptr,0);
    g.focusRead.Reset();
    g.device->CreateOffscreenPlainSurface(1,1,D3DFMT_R32F,D3DPOOL_SYSTEMMEM,&g.focusRead,nullptr);
    g.w=w;g.h=h;g.format=d.Format;g.targets=true;
    MvmDebugLog_Linef("lens.targets","D3D9 target=%ux%u fmt=%u CoC levels=%u",w,h,(unsigned)d.Format,g.levels);
    return true;
}
bool Pump() {
    if (g_failed) return false;
    auto* device=AfxGetDirect3DDevice9(); if (!device) return false;
    if (g.device!=device) { g={}; g.device=device; }
    if (g.ready) return true;
    if (g_code.empty() && g_pending.valid() && g_pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try { g_code=g_pending.get(); }
        catch(const std::exception& e) { g_failed=true; LensFx_SetRenderStatus("Lens shader preparation failed; see mvm_debug"); MvmDebugLog_LinefAlways("lens.shader","%s",e.what()); return false; }
    }
    if (g_code.size()!=g.shaders.size()) return false;
    for (unsigned i=0;i<g_code.size();++i)
        if (!g.shaders[i] && FAILED(device->CreatePixelShader((const DWORD*)g_code[i].bytes->GetBufferPointer(),&g.shaders[i]))) return false;
    auto* vertex=g_AfxShaders.GetAcsVertexShader(L"afx_source_pass_vs30.acs",0);
    g.vertex=vertex ? vertex->GetVertexShader() : nullptr;
    auto* fisheye=g_AfxShaders.GetAcsPixelShader(L"afx_source_fisheye_ps30.acs",0);
    g.fisheye=fisheye ? fisheye->GetPixelShader() : nullptr;
    g.ready=g.vertex && g.fisheye;
    if (g.ready) MvmDebugLog_LinefAlways("lens.shader","D3D9 device ready: %u shared lens passes",(unsigned)g.shaders.size());
    return g.ready;
}

// State blocks do not include colour/depth surfaces. Restore them around HLAE's own quad draws.
struct TargetsGuard {
    IDirect3DDevice9* d; ComPtr<IDirect3DSurface9> rt[4],depth; D3DVIEWPORT9 viewport{};
    TargetsGuard(IDirect3DDevice9* device):d(device) {
        for(unsigned i=0;i<4;++i) d->GetRenderTarget(i,&rt[i]);
        d->GetDepthStencilSurface(&depth); d->GetViewport(&viewport); d->SetDepthStencilSurface(nullptr);
    }
    ~TargetsGuard() {
        for(unsigned i=1;i<4;++i) d->SetRenderTarget(i,nullptr);
        d->SetRenderTarget(0,rt[0].Get());
        for(unsigned i=1;i<4;++i) d->SetRenderTarget(i,rt[i].Get());
        d->SetDepthStencilSurface(depth.Get()); d->SetViewport(&viewport);
    }
};
struct Bind { const char* name; IDirect3DTexture9* texture; bool point=false; };
void Pass(unsigned shader,IDirect3DSurface9* output,unsigned w,unsigned h,const GpuConstants& k,
    std::initializer_list<Bind> textures,IDirect3DSurface9* second=nullptr,AfxRectPass::Blend blend=AfxRectPass::Replace) {
    if(FAILED(g_frameResult)) return;
    auto* d=g.device;
    for(unsigned i=1;i<4;++i) d->SetRenderTarget(i,nullptr);
    g_frameResult=d->SetRenderTarget(0,output);
    if(SUCCEEDED(g_frameResult) && second) g_frameResult=d->SetRenderTarget(1,second);
    if(FAILED(g_frameResult)) return;
    AfxRectPass pass;pass.vertex=g.vertex;pass.pixel=g.shaders[shader].Get();pass.blend=blend;
    pass.constants=(const float*)&k;pass.constantCount=sizeof(k)/16;pass.result=&g_frameResult;
    for(const auto& binding:g_code[shader].bindings) if(binding.set==3) {
        pass.textures[binding.first]=g.black.texture.Get();
        for(const auto& value:textures) if(binding.name==value.name) {
            pass.textures[binding.first]=value.texture ? value.texture : g.black.texture.Get();
            if(value.point) pass.pointSamplers|=1u<<binding.first;
            break;
        }
    }
    AfxDrawRect(nullptr,0,0,w,h,0,1,1,0,&pass);
}
void Pass(unsigned shader,const Texture& output,const GpuConstants& k,std::initializer_list<Bind> textures,const Texture* second=nullptr) {
    Pass(shader,output.surface.Get(),output.w,output.h,k,textures,second ? second->surface.Get() : nullptr);
}
bool CopyLevel(IDirect3DTexture9* texture,unsigned level,IDirect3DSurface9* source) {
    ComPtr<IDirect3DSurface9> target;
    return SUCCEEDED(texture->GetSurfaceLevel(level,&target)) && SUCCEEDED(g.device->StretchRect(source,nullptr,target.Get(),nullptr,D3DTEXF_NONE));
}
} // namespace

void LensGpu_Precache() {
    if(g_started) return; g_started=true;
    g_pending=std::async(std::launch::async,[] {
        const auto source=LensShaders::Source(); std::vector<Code> result;
        const auto started=GetTickCount64();
        for(const char* entry:LensShaders::Entries()) {
            Code code;ComPtr<ID3DBlob> errors;
            const HRESULT hr=Environment::CompileEnvironmentShader(source.data(),source.size(),"SourceLens",nullptr,entry,"ps_3_0",
                D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY|D3DCOMPILE_OPTIMIZATION_LEVEL3,&code.bytes,&errors);
            if(FAILED(hr) || !LensShaders::ReadBindings(code.bytes->GetBufferPointer(),code.bytes->GetBufferSize(),code.bindings)) {
                MvmDebugLog_LinefAlways("lens.shader","D3D9 %s hr=%08x %s",entry,(unsigned)hr,errors ? (const char*)errors->GetBufferPointer() : "invalid bindings");
                throw std::runtime_error("D3D9 lens shader preparation failed");
            }
            result.push_back(std::move(code));
        }
        MvmDebugLog_LinefAlways("lens.shader","D3D9 batch ready passes=%zu ms=%llu",result.size(),GetTickCount64()-started);
        return result;
    });
}
bool LensGpu_Prepared() { return g_precached.load(); }
bool LensGpu_DepthPrepared() { return g_depthReady.load(); }
bool LensGpu_Precached(uint64_t revision) { return g_precached.load() && g_warmedRevision.load()==revision; }
bool LensGpu_Begin(bool enabled,bool fisheye,uint64_t revision) {
    if(!Pump()) return false;
    auto* target=AfxGetRenderTargetSurface();D3DSURFACE_DESC desc{};
    if(!target || FAILED(target->GetDesc(&desc)) || !Targets(desc)) {
        g_precached.store(false);g_depthReady.store(false);
        LensFx_SetRenderStatus("Could not allocate lens surfaces"); return false;
    }
    const auto state=LensFx_ReadRenderState(g.irisSerial,g.dirtSerial);
    if(state.irisChanged) { Mask(g.iris,state.iris);g.irisSerial=state.iris.serial; }
    if(state.dirtChanged) { Mask(g.dirt,state.dirt);g.dirtSerial=state.dirt.serial; }
    // Warm depth and its discarded colour target while the effects are still off.
    g_depthReady.store(g_AfxStreams.DrawingThread_GetOrCreateIntZTextureSurface()
        && g_AfxStreams.DrawingThread_GetOrCreateRenderTargetSurfaceNoMssaa());
    // The queued revision was read BEFORE this snapshot. A concurrent edit therefore queues
    // another warm-up rather than incorrectly claiming a newer, unuploaded mask is ready.
    g_warmedRevision.store(revision);g_precached.store(true);
    g.depthPushed=false;g.msaa=g_AfxStreams.DrawingThread_HasRenderTargetMsaa();
    if(!enabled) { g.focusValid=false;LensFx_SetRenderStatus("Off");return fisheye; }
    if(!g_depthReady.load()) {LensFx_SetRenderStatus("World depth unavailable");return false;}
    return true;
}
void LensGpu_DepthBegin(const LensSmokeFrame& smoke) {
    g.smokeFrame=smoke;g.smokeDraws=0;
    // Only the auxiliary depth view is single-sample. Never replace the original Beauty colour.
    g_AfxStreams.DrawingThread_SetRenderTargetNoMsaa();
    g_AfxStreams.DrawingThread_SetIntZTextureSurface();g.depthPushed=true;
    if(smoke.count && g.smoke.surface) {
        g.device->ColorFill(g.smoke.surface.Get(),nullptr,0xffffffff);
        LensSmoke_Begin(g.device,g.smoke.surface.Get());
    }
}
void LensGpu_DepthEnd() {
    if(!g.depthPushed) return;
    g.smokeDraws=LensSmoke_End();
    g_AfxStreams.DrawingThread_UnsetIntZTextureSurface();g.depthPushed=false;
    g_AfxStreams.DrawingThread_UnsetRenderTargetNoMsaa(false);
}
void RenderLens(const float* worldToClip,const float* sunDirection,bool holdFocus) {
    const auto state=LensFx_ReadRenderState(g.irisSerial,g.dirtSerial);const auto& L=state.values;
    bool dof=L[DofOn]>.5f,flare=L[FlareOn]>.5f;
    if(!dof && !flare) return;
    Environment::Matrix matrix{},inverse{};
    if(worldToClip) std::copy(worldToClip,worldToClip+16,matrix.begin());
    const bool depthValid=worldToClip && Environment::Inverse(matrix,inverse);
    if(dof && !depthValid) { LensFx_SetRenderStatus("Waiting for world depth"); dof=false; }
    if(!dof && !flare) return;
    ComPtr<IDirect3DTexture9> depth;
    auto* surface=g_AfxStreams.DrawingThread_GetOrCreateIntZTextureSurface();
    if(!surface || FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9),(void**)&depth))) return;
    const bool irisChanged=state.irisChanged,dirtChanged=state.dirtChanged;
    if(irisChanged) { Mask(g.iris,state.iris);g.irisSerial=state.iris.serial; }
    if(dirtChanged) { Mask(g.dirt,state.dirt);g.dirtSerial=state.dirt.serial; }
    const auto now=std::chrono::steady_clock::now();
    const bool autoFocus=dof && L[DofAuto]>.5f;
    if(!autoFocus) g.focusValid=false;
    const bool advance=autoFocus && !(holdFocus && g.focusValid);
    const float dt=g.haveLast ? std::clamp(std::chrono::duration<float>(now-g.last).count(),0.f,.25f) : 0.f;
    if(!holdFocus || advance) { g.last=now;g.haveLast=true; }
    auto k=MakeGpuConstants(L,g.w,g.h,g.w,g.h,inverse.data(),dt,g.focusValid,
        bool(g.iris.texture),bool(g.dirt.texture),dof,flare,autoFocus);
    if(g.smokeDraws && g.smokeFrame.count) {
        k.smokeInfo={(float)g.smokeFrame.count,2,(float)g.w,(float)g.h};
        k.cameraPos={g.smokeFrame.camera[0],g.smokeFrame.camera[1],g.smokeFrame.camera[2],0};
        for(unsigned i=0;i<g.smokeFrame.count;++i) {
            const auto& s=g.smokeFrame.spheres[i];k.smokeSpheres[i]={s[0],s[1],s[2],s[3]};
        }
    }
    float sunU=0,sunV=0;
    const bool sunOn=flare && depthValid && sunDirection && sunDirection[2]>-.05f && L[FlareSun]>0
        && SunScreenUv(worldToClip,sunDirection,sunU,sunV);
    k.sunInfo={sunU,sunV,sunOn ? L[FlareSun] : 0.f,0};
    g_frameResult=S_OK;
    TargetsGuard guard(g.device);
    if(FAILED(g.device->StretchRect(guard.rt[0].Get(),nullptr,g.source.surface.Get(),nullptr,D3DTEXF_NONE))) {
        LensFx_SetRenderStatus("Could not copy lens colour");return;
    }
    if(dof) {
        const unsigned cur=advance ? 1-g.focusCur : g.focusCur;
        if(advance) Pass(0,g.focus[cur],k,{{"sceneDepth",depth.Get(),true},{"focusPrev",g.focus[g.focusCur].texture.Get(),true},{"smokeAbsorb",g.smoke.texture.Get(),true}});
        Pass(2,g.coc[0],k,{{"colorTex",g.source.texture.Get()},{"sceneDepth",depth.Get(),true},{"focusTex",g.focus[cur].texture.Get(),true},{"smokeAbsorb",g.smoke.texture.Get(),true}},&g.world[0]);
        for(unsigned m=0;m<g.levels;++m) {
            if(m) Pass(3,g.coc[m],k,{{"cocTex",g.coc[m-1].texture.Get()},{"worldMask",g.world[m-1].texture.Get()}},&g.world[m]);
            if(!CopyLevel(g.cocAll.Get(),m,g.coc[m].surface.Get()) || !CopyLevel(g.worldAll.Get(),m,g.world[m].surface.Get())) {
                LensFx_SetRenderStatus("Could not build lens mip chain");return;
            }
        }
        Pass(4,g.tile[0],k,{{"cocTex",g.cocAll.Get(),true}});
        Pass(5,g.tile[1],k,{{"tileTex",g.tile[0].texture.Get(),true}});
        Pass(6,g.tile[0],k,{{"tileTex",g.tile[1].texture.Get(),true}});
        Pass(7,g.gather,k,{{"cocTex",g.cocAll.Get()},{"tileTex",g.tile[0].texture.Get(),true},{"irisMask",g.iris.texture.Get()},{"worldMask",g.worldAll.Get()}});
        Pass(8,g.soft,k,{{"cocTex",g.cocAll.Get()},{"gatherTex",g.gather.texture.Get()}});
        if(advance) g.focusCur=cur;
        g.focusValid=autoFocus;
    }
    if(flare) {
        if(sunOn) Pass(1,g.sun,k,{{"sceneDepth",depth.Get(),true},{"smokeAbsorb",g.smoke.texture.Get(),true}});
        Pass(9,g.half,k,{{"dofTex",g.source.texture.Get()}});
        k.passInfo={1.f/g.half.w,1.f/g.half.h,0,0};
        Pass(10,g.quarter,k,{{"brightTex",g.half.texture.Get()}});
        k.passInfo={1.f/g.quarter.w,1.f/g.quarter.h,0,0};
        Pass(10,g.eighth,k,{{"brightTex",g.quarter.texture.Get()}});
        Pass(11,g.sourceQ,k,{{"dofTex",g.source.texture.Get()},{"brightTex",g.quarter.texture.Get()},{"bright8Tex",g.eighth.texture.Get()},{"sunVisTex",g.sun.texture.Get(),true}});
        Pass(10,g.sourceE,k,{{"brightTex",g.sourceQ.texture.Get()}});
        // No read/write alias: downsample into a separate surface then copy each mip.
        const Texture* previous=&g.sourceE;
        for(unsigned m=1;m<g.sourceE.texture->GetLevelCount();++m) {
            const Texture& next=g.sourceMips[m-1];
            k.passInfo={1.f/previous->w,1.f/previous->h,0,0};
            Pass(3,next,k,{{"cocTex",previous->texture.Get()}});
            if(!CopyLevel(g.sourceE.texture.Get(),m,next.surface.Get())) return;
            previous=&next;
        }
        Pass(12,g.sourceH,k,{{"sourceTex",g.sourceQ.texture.Get()}});
        Pass(13,g.ghostSrc,k,{{"source8Tex",g.sourceE.texture.Get()}});
        Pass(14,g.ghost,k,{{"ghostSrcTex",g.ghostSrc.texture.Get()}});
        Pass(15,g.streak,k,{{"sourceTex",g.sourceQ.texture.Get()},{"sourceHTex",g.sourceH.texture.Get()}});
        Pass(16,g.glow[1],k,{{"source8Tex",g.sourceE.texture.Get()}});
        Pass(17,g.glow[0],k,{{"glowTex",g.glow[1].texture.Get()}});
    }
    const std::initializer_list<Bind> composite={{"colorTex",g.source.texture.Get(),true},{"cocTex",g.cocAll.Get(),true},
        {"gatherTex",g.soft.texture.Get(),true},{"ghostTex",g.ghost.texture.Get()},{"streakTex",g.streak.texture.Get()},
        {"glowTex",g.glow[0].texture.Get()},{"dirtTex",g.dirt.texture.Get()},{"bright8Tex",g.eighth.texture.Get()},
        {"sunVisTex",g.sun.texture.Get(),true}};
    Pass(19,guard.rt[0].Get(),g.w,g.h,k,composite,nullptr,AfxRectPass::Multiply);
    Pass(20,guard.rt[0].Get(),g.w,g.h,k,composite,nullptr,AfxRectPass::Add);
    if(FAILED(g_frameResult)) {
        LensFx_SetRenderStatus("Lens draw failed; see mvm_debug");
        MvmDebugLog_LinefAlways("lens.draw","D3D9 HRESULT=%08x",(unsigned)g_frameResult);return;
    }
    LensFx_NoteRenderedFrame();LensFx_SetRenderStatus("Rendering");
    ++g.frames;if(holdFocus) ++g.heldFrames;
    if(MvmDebugLog_Active() && (g.frames<=3 || g.frames%120==0 || (holdFocus && g.heldFrames<=3))) {
        MvmDebugLog_Linef("lens.frame","D3D9 dof=%d flare=%d target=%ux%u held=%d auto=%d taps=%d iris=%u dirt=%u samples=%s sun=%d uv=(%.3f %.3f)",
            dof,flare,g.w,g.h,holdFocus,autoFocus,TapCount(L),g.irisSerial,g.dirtSerial,g.msaa ? "original MSAA colour" : "1",sunOn,sunU,sunV);
        if(g.smokeFrame.count) MvmDebugLog_Linef("lens.smoke","clouds=%u draws=%u radius=%.2f centre=(%.2f %.2f %.2f)",
            g.smokeFrame.count,g.smokeDraws,g.smokeFrame.spheres[0][3],g.smokeFrame.spheres[0][0],g.smokeFrame.spheres[0][1],g.smokeFrame.spheres[0][2]);
        if(autoFocus && g.focusRead && SUCCEEDED(g.device->GetRenderTargetData(g.focus[g.focusCur].surface.Get(),g.focusRead.Get()))) {
            D3DLOCKED_RECT lock{};
            if(SUCCEEDED(g.focusRead->LockRect(&lock,nullptr,D3DLOCK_READONLY))) {
                const float distance=*(const float*)lock.pBits;g.focusRead->UnlockRect();
                MvmDebugLog_Linef("lens.focus","D3D9 distance=%.3f units held=%d advanced=%d",distance,holdFocus,advance);
            }
        }
    }
}
void LensGpu_Render(const float* worldToClip,const float* sunDirection,bool holdFocus,bool lens,bool fisheye) {
    if(lens) RenderLens(worldToClip,sunDirection,holdFocus);
    if(!fisheye) return;
    TargetsGuard guard(g.device);
    HRESULT result=g.device->StretchRect(guard.rt[0].Get(),nullptr,g.source.surface.Get(),nullptr,D3DTEXF_NONE);
    if(SUCCEEDED(result)) {
        const auto constants=FisheyeParameters(g.w,g.h);
        for(unsigned i=1;i<4;++i) g.device->SetRenderTarget(i,nullptr);
        AfxRectPass pass;pass.pixel=g.fisheye;pass.vertex=g.vertex;pass.constants=constants.data();
        pass.constantCount=1;pass.textures[0]=g.source.texture.Get();pass.result=&result;
        AfxDrawRect(nullptr,0,0,g.w,g.h,0,1,1,0,&pass);
    }
    if(FAILED(result)) MvmDebugLog_LinefAlways("lens.fisheye","D3D9 failed HRESULT=%08x",(unsigned)result);
    else if(MvmDebugLog_Active()) MvmDebugLog_Linef("lens.fisheye","D3D9 target=%ux%u held=%d",g.w,g.h,holdFocus);
}
void LensGpu_End(bool began) {
    if(began) LensGpu_DepthEnd();
}
void LensGpu_DeviceLost() { LensSmoke_End();g_precached.store(false);g_depthReady.store(false);g={}; }
} // namespace SourceFilmmaker
