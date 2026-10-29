#include "stdafx.h"
#include "SourceLens.h"
#include "SourceLensGpu.h"
#include "SourceRenderPasses.h"
#include "SourceEngine.h"
#include "SourceSdk2013.h"
#include "addresses.h"
#include "d3d9Hooks.h"
#include "MaterialSystemHooks.h"
#include <AfxHookSource2/Filmmaker/Lens/LensFxState.h>
#include <AfxHookSource2/Filmmaker/Lens/FisheyeState.h>
#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>
#include <atomic>
#include <memory>
#include <functional>
#include <array>
#include <cstring>
#include <set>
#include <string>

namespace SourceFilmmaker {
namespace {
using Paint = void (__thiscall*)(void*,int);
Paint g_paint = nullptr;
struct View { bool began=false,rendered=false,hold=false,fisheye=false,lens=false; int drawFlags=1; unsigned long long number=0; };
std::shared_ptr<View> g_view;
std::array<float,3> g_sun{};bool g_haveSun=false;ULONGLONG g_sunRead=0;
unsigned long long g_viewNumber=0,g_boundaries=0;
std::atomic<unsigned long long> g_renderBoundaries{0};
class Work final : public CMaterialSystemFunctor {
    std::function<void()> m_call;
public:
    Work(std::function<void()> call):m_call(std::move(call)) {}
    void operator()() override { m_call(); }
};
void RenderWorld(const std::shared_ptr<View>& view,int mode) {
    float matrix[16]={}; const bool have=Engine_WorldToClip(matrix);
    // Values and matrices cross the engine/render queue by value; no engine calls on the worker.
    std::array<float,16> copy{};std::copy(matrix,matrix+16,copy.begin());
    const auto sun=g_sun;const bool haveSun=g_haveSun;
    if(view->lens) {
        LensSmokeFrame smoke;
        smoke.count=Engine_SmokeVolumes(smoke.spheres[0].data(),(int)smoke.spheres.size());
        const auto camera=View_Rendered();
        for(unsigned a=0;a<3;++a) smoke.camera[a]=(float)camera.origin[a];
        MaterialSystem_ExecuteOnRenderThread(new Work([smoke] {LensGpu_DepthBegin(smoke);}));
        const bool drawn=RenderPasses_LensDepth(view->drawFlags,smoke.count>0);
        MaterialSystem_ExecuteOnRenderThread(new Work([drawn] {
            LensGpu_DepthEnd();
            if(!drawn) Filmmaker::LensFx_SetRenderStatus("The lens depth view failed");
        }));
        if(!drawn) return;
    }
    MaterialSystem_ExecuteOnRenderThread(new Work([view,copy,have,mode,sun,haveSun] {
        const auto number=++g_renderBoundaries;
        if(Filmmaker::MvmDebugLog_Active() && (number<=3 || number%120==0)) {
            D3DSURFACE_DESC color{},depth{};
            if(auto* surface=AfxGetRenderTargetSurface()) surface->GetDesc(&color);
            if(auto* surface=AfxGetDepthStencilSurface()) surface->GetDesc(&depth);
            Filmmaker::MvmDebugLog_Linef("lens.boundary","view=%llu before HUD mode=%d colour=%ux%u depth=%ux%u fmt=%u active=%d matrix=%d",
                view->number,mode,color.Width,color.Height,depth.Width,depth.Height,(unsigned)depth.Format,view->began,have);
        }
        if(view->began) LensGpu_Render(have ? copy.data() : nullptr,haveSun ? sun.data() : nullptr,view->hold,view->lens,view->fisheye);
    }));
}
void __fastcall HookPaint(void* self,
#ifndef _WIN64
    void*,
#endif
    int mode) {
    if(g_view && !g_view->rendered && (mode&2)) {
        g_view->rendered=true;++g_boundaries;RenderWorld(g_view,mode);
    }
    g_paint(self,mode);
}
using Bind = void (__thiscall*)(void*,void*,void*);
std::array<Bind,4> g_binds{};
std::array<void**,4> g_bindTables{};
thread_local bool g_depthMaterials=false;
bool MaterialName(void* material,char* name,unsigned capacity) {
    __try {
        if(!material) return false;
        using GetName=const char* (__thiscall*)(void*);
        const char* text=((GetName)(*(void***)material)[0])(material);
        unsigned i=0;for(;i+1<capacity && text[i];++i) name[i]=text[i];name[i]=0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<unsigned N> void __fastcall HookBind(void* self,
#ifndef _WIN64
    void*,
#endif
    void* material,void* proxy) {
    // Binding may flush the preceding batch; keep its tag until that flush is queued.
    g_binds[N](self,material,proxy);
    if(!g_depthMaterials) return;
    char name[256]={};const bool named=MaterialName(material,name,sizeof(name));
    const bool smoke=named && std::strncmp(name,"particle/particle_smokegrenade",30)==0;
    MaterialSystem_ExecuteOnRenderThread(new Work([smoke] {LensSmoke_Material(smoke);}));
    if(named && Filmmaker::MvmDebugLog_Active() && std::strstr(name,"smoke")) {
        static std::set<std::string> seen;
        if(seen.insert(name).second) Filmmaker::MvmDebugLog_Linef("lens.smoke.material","%s captured=%d",name,smoke);
    }
}
} // namespace
void Lens_DepthMaterialScope(void* context,bool enter) {
    g_depthMaterials=false;
    if(enter && context) {
        auto** table=*(void***)context;
        const void* hooks[]={ (void*)&HookBind<0>,(void*)&HookBind<1>,(void*)&HookBind<2>,(void*)&HookBind<3> };
        for(unsigned i=0;i<g_binds.size();++i) {
            if(!g_bindTables[i]) {
                AfxDetourPtr((PVOID*)&table[9],(PVOID)hooks[i],(PVOID*)&g_binds[i]);
                if(g_binds[i]) g_bindTables[i]=table;
            }
            if(g_bindTables[i]==table) {g_depthMaterials=true;break;}
        }
    }
    if(!enter) MaterialSystem_ExecuteOnRenderThread(new Work([] {LensSmoke_Material(false);}));
}
void Lens_Frame() {
    if(sizeof(void*)!=8 || g_SourceSdkVer!=SourceSdkVer_CSS) return;
    LensGpu_Precache();
    if(Filmmaker::LensFx_FlareOn() && GetTickCount64()-g_sunRead>1000) {
        g_sunRead=GetTickCount64();g_haveSun=Engine_SunDirection(g_sun.data());
        if(Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("lens.sun","CSun=%d direction=(%.4f %.4f %.4f)",g_haveSun,g_sun[0],g_sun[1],g_sun[2]);
    }
    if(!Demo_State().playing) g_haveSun=false;
    if(g_paint) return;
    HMODULE engine=GetModuleHandleW(L"engine.dll");
    using Factory=void* (*)(const char*,int*);
    auto factory=engine ? (Factory)GetProcAddress(engine,"CreateInterface") : nullptr;
    void* vgui=factory ? factory("VEngineVGui001",nullptr) : nullptr;
    if(!vgui || !Hook_MaterialSystem()) return;
    auto** slot=&(*(void***)vgui)[Sdk2013::Slot::EngineVGui_Paint];
    AfxDetourPtr((PVOID*)slot,(PVOID)&HookPaint,(PVOID*)&g_paint);
    Filmmaker::MvmDebugLog_Linef("lens.boundary","VEngineVGui001 Paint slot %d hooked=%d",Sdk2013::Slot::EngineVGui_Paint,g_paint!=nullptr);
}
void Lens_ViewBegin(bool holdFocus,int drawFlags) {
    if(!g_paint || !Demo_State().playing) return;
    const bool enabled=Filmmaker::LensFx_Active(),fisheye=Filmmaker::Fisheye_Enabled();
    const auto revision=Filmmaker::LensFx_PictureRevision();
    if(!enabled && !fisheye && !Filmmaker::MvmDebugLog_Active() && LensGpu_Precached(revision)) return;
    auto view=std::make_shared<View>();view->number=++g_viewNumber;view->hold=holdFocus;view->fisheye=fisheye;view->lens=enabled && LensGpu_DepthPrepared();view->drawFlags=drawFlags;g_view=view;
    MaterialSystem_ExecuteOnRenderThread(new Work([view,revision] {view->began=LensGpu_Begin(view->lens,view->fisheye,revision);}));
}
void Lens_ViewEnd() {
    if(!g_view) return;
    auto view=g_view;g_view.reset();
    if(!view->rendered) RenderWorld(view,0);
    MaterialSystem_ExecuteOnRenderThread(new Work([view] {LensGpu_End(view->began);}));
}
void Lens_DeviceLost() { LensGpu_DeviceLost(); }
void Lens_PrintHealth() {
    advancedfx::Message("  lens:       HUD boundary %s, %llu main / %llu render callbacks; shared D3D9 passes\n",
        g_paint ? "hooked" : "not hooked",g_boundaries,g_renderBoundaries.load());
}
} // namespace SourceFilmmaker
