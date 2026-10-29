#include "stdafx.h"

#include "SourceRenderPasses.h"
#include "SourceLens.h"
#include "SourceRenderPassMath.h"
#include <algorithm>
#include "SourceConCommand.h"
#include "SourceEngine.h"
#include "SourceSdk2013.h"

#include "AfxStreams.h"
#include "addresses.h"
#include "d3d9Hooks.h"
#include "AfxShaders.h"
#include <Shaders/build/afx_depth_ps20.h>

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Render/RenderSessionDriver.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace SourceFilmmaker {
namespace {

using namespace Sdk2013;
using namespace advancedfx;
using Factory = void* (*)(const char*, int*);
using ViewRender = void (__thiscall*)(void*, void*);
using RenderView = void (__thiscall*)(void*, const void*, int, int);
using GetPlayerView = bool (__thiscall*)(void*, void*);

ViewRender g_original = nullptr;
void* g_client = nullptr;
void* g_materials = nullptr;
bool g_armed = false;
using SetTone = void (__thiscall*)(void*, const float*);
SetTone g_setTone = nullptr;
bool g_linearTone = false;
unsigned g_toneCalls = 0;

void __fastcall HookSetTone(void* context, const float* scale) {
    const float linear[3] = {1,1,1};
    if (g_linearTone && Filmmaker::MvmDebugLog_Active() && g_toneCalls++ < 6)
        Filmmaker::MvmDebugLog_Linef("render.tone", "client scale %g %g %g -> 1 1 1", scale[0],scale[1],scale[2]);
    g_setTone(context, g_linearTone ? linear : scale);
}
int g_frames = 0;
ExtraPassLook g_look;
using DrawModelExecute = void (__thiscall*)(void*,const void*,const void*,const void*);
DrawModelExecute g_drawModel = nullptr;
void* g_passContext = nullptr;
ExtraPass g_activePass = ExtraPass::NoGun;
float g_beautyTone[3] = {1,1,1};
using namespace RenderPassMath;
std::array<unsigned char,4096> g_subjects{};

void SnapshotSubjects() {
    g_subjects.fill(0);
    std::vector<EntityState> entities;
    Entities_Snapshot(entities,true);
    for (const auto& e : entities) if (e.index > 0 && e.index < (int)g_subjects.size()) {
        g_subjects[e.index] = (unsigned char)SubjectClass(e.className.c_str());
    }
    for (const auto& e : entities) if (e.index > 0 && e.index < (int)g_subjects.size()) {
        const int owners[2] = {e.ownerIndex,e.weaponOwnerIndex};
        for (int owner : owners) if (owner > 0 && owner < (int)g_subjects.size() && (g_subjects[owner]&Player))
            g_subjects[e.index] |= Held;
    }
}

int ModelIndex(const void* info) {
    __try { return *(const int*)((const unsigned char*)info + kModelRenderEntityIndex); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void __fastcall HookDrawModel(void* model, const void* state, const void* info, const void* bones) {
    if (!g_passContext) { g_drawModel(model,state,info,bones); return; }
    const int index = ModelIndex(info);
    const unsigned bits = index > 0 && index < (int)g_subjects.size() ? g_subjects[index] : 0;
    if ((g_activePass == ExtraPass::World || g_activePass == ExtraPass::DepthWorld || g_activePass == ExtraPass::NormalWorld) && bits) return;
    const bool isolated = g_activePass == ExtraPass::KeyPlayers && (bits & (Player|Weapon|Held)) && !(bits&Viewmodel);
    const bool gun = g_activePass == ExtraPass::KeyGun && (bits&Viewmodel);
    if (!isolated && !gun) { g_drawModel(model,state,info,bones); return; }
    using FogGet = int (__thiscall*)(void*);
    using FogSet = void (__thiscall*)(void*,int);
    const int fog = ((FogGet)(*(void***)g_passContext)[Slot::RenderContext_GetFogMode])(g_passContext);
    ((FogSet)(*(void***)g_passContext)[Slot::RenderContext_FogMode])(g_passContext,0);
    g_linearTone = false;
    g_setTone(g_passContext,g_beautyTone);
    g_drawModel(model,state,info,bones);
    g_linearTone = true;
    const float linear[3] = {1,1,1};
    g_setTone(g_passContext,linear);
    ((FogSet)(*(void***)g_passContext)[Slot::RenderContext_FogMode])(g_passContext,fog);
}

// Pass failures are reported to the job before it finalizes its writers.
struct Failure {
	std::mutex mutex;
	std::string message;
	void Set(const char* text) {
		std::lock_guard<std::mutex> lock(mutex);
		if (message.empty()) message = text;
	}
};
std::shared_ptr<Failure> g_failure = std::make_shared<Failure>();
std::string g_error;

struct Pass { ExtraPass kind; CCaptureNode* node; };
std::vector<Pass> g_passes;
IDirect3DSurface9* g_worldSurface = nullptr;
IDirect3DTexture9* g_worldDepth = nullptr;
bool g_previewEnabled = false;
ExtraPass g_previewKind = ExtraPass::NoGun;
ExtraPassLook g_previewLook;
void* g_previewQueue = nullptr;
std::string g_previewSavedQueue;
int g_previewWarmFrames = 0;


struct Variable { const char* name; void* handle = nullptr; std::string saved; };
std::array<Variable, 18> g_vars = {{
	{ "fog_override" }, { "fog_enable" }, { "fog_maxdensity" }, { "fog_start" }, { "fog_end" },
	{ "fog_color" }, { "fog_colorskybox" }, { "r_skybox" }, { "r_3dsky" }, { "gl_clear" },
	{ "r_drawparticles" }, { "r_drawdecals" }, { "r_renderoverlayfragment" }, { "mat_force_tonemap_scale" },
	{ "fog_enableskybox" }, { "fog_startskybox" }, { "fog_endskybox" }, { "r_shadows" }
}};

bool PrepareDraw(ExtraPass kind, std::string& error) {
    for (Variable& v : g_vars) {
        if (!v.handle) v.handle = ConVar_Find(v.name);
        if (!v.handle) { error = std::string("missing pass console variable: ") + v.name; return false; }
    }
    if (!PassNeedsDepth(kind)) return true;
    const bool source = PassIsNormal(kind) || PassDepthMask(kind);
    if (source) {
        auto* vertex = g_AfxShaders.GetAcsVertexShader(L"afx_source_pass_vs30.acs",0);
        if (!vertex || !vertex->GetVertexShader()) { error = "the pass vertex shader is unavailable"; return false; }
    }
    auto* shader = source ? g_AfxShaders.GetAcsPixelShader(L"afx_source_normal_ps30.acs",0)
        : g_AfxShaders.GetAcsPixelShader(L"afx_depth_ps20.acs",ShaderCombo_afx_depth_ps20::GetCombo(
            ShaderCombo_afx_depth_ps20::AFXCLIP_0,ShaderCombo_afx_depth_ps20::AFXDEPTHMODE_3,ShaderCombo_afx_depth_ps20::AFXD24_0));
    if (!AfxD3d9_DrawDepthSupported() || !shader || !shader->GetPixelShader()) {
        error = "HLAE's INTZ depth capture or its shader is unavailable"; return false;
    }
    return true;
}

void Set(const char* name, const char* value) {
	for (const Variable& v : g_vars) if (0 == std::strcmp(v.name, name)) { ConVar_SetString(v.handle, value); return; }
}
void Number(const char* name, float value) {
	char text[32]; std::snprintf(text, sizeof(text), "%g", value); Set(name, text);
}
void Restore() { for (const Variable& v : g_vars) if (v.handle) ConVar_SetString(v.handle, v.saved.c_str()); }

// Guard the version-coupled calls separately from C++ objects with destructors.
bool PlayerView(void* client, void* view) {
	__try { return ((GetPlayerView)(*(void***)client)[Slot::Client_GetPlayerView])(client, view); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* Context() {
	__try {
		using Fn = void* (__thiscall*)(void*);
		return ((Fn)(*(void***)g_materials)[Slot::MaterialSystem_GetRenderContext])(g_materials);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void ReleaseContext(void* context) {
	using Fn = int (__thiscall*)(void*);
	((Fn)(*(void***)context)[Slot::RenderContext_Release])(context);
}
void ClearColor(void* context, const unsigned char color[3]) {
	using Fn = void (__thiscall*)(void*, unsigned char, unsigned char, unsigned char, unsigned char);
	((Fn)(*(void***)context)[Slot::RenderContext_ClearColor4ub])(context, color[0], color[1], color[2], 255);
}
bool DrawView(void* client, const void* view, int drawFlags) {
	__try {
		((RenderView)(*(void***)client)[Slot::Client_RenderView])(client, view,
			kViewClearColor | kViewClearDepth | kViewClearStencil, drawFlags);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ToneScale(void* context, float scale[3], bool write) {
    __try {
        if (write) {
            using Fn = void (__thiscall*)(void*, const float*);
            ((Fn)(*(void***)context)[Slot::RenderContext_SetToneMappingScaleLinear])(context, scale);
        } else {
            // MSVC returns the 12-byte Vector through the caller's buffer in RDX.
            using Fn = void* (__thiscall*)(void*, float*);
            ((Fn)(*(void***)context)[Slot::RenderContext_GetToneMappingScaleLinear])(context, scale);
        }
        return std::isfinite(scale[0]) && std::isfinite(scale[1]) && std::isfinite(scale[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Fog(const unsigned char color[3], float nearDistance, float farDistance) {
	Set("fog_override", "1"); Set("fog_enable", "1"); Set("fog_maxdensity", "1");
	Number("fog_start", nearDistance); Number("fog_end", farDistance);
	Set("fog_enableskybox", "1"); Set("fog_startskybox", "0"); Set("fog_endskybox", "1");
	char rgb[32]; std::snprintf(rgb, sizeof(rgb), "%u %u %u", color[0], color[1], color[2]);
	Set("fog_color", rgb); Set("fog_colorskybox", rgb);
	Set("r_skybox", "0"); Set("r_3dsky", "0"); Set("gl_clear", "1");
	Set("r_drawparticles", "0"); Set("r_drawdecals", "0"); Set("r_renderoverlayfragment", "0");
    // mat_force_tonemap_scale is sampled by the client's post-processing update, which is
    // skipped by this view. Set the render context's linear scale immediately instead.
}

void DrawPasses(void* client, const std::vector<Pass>& passes, const ExtraPassLook& look, bool preview) {
	alignas(16) unsigned char view[kViewSetupSize] = {};
	if (!PlayerView(client, view)) { g_failure->Set("the client did not return its player view"); return; }
	for (Variable& v : g_vars) if (v.handle && !ConVar_GetString(v.handle, v.saved)) {
		g_failure->Set("a pass console variable could not be saved"); return;
	}
	void* context = Context();
	if (!context) { g_failure->Set("the material render context is unavailable"); return; }
    if (!g_setTone) {
        void** slot = &(*(void***)context)[Slot::RenderContext_SetToneMappingScaleLinear];
        AfxDetourPtr((PVOID*)slot,(PVOID)&HookSetTone,(PVOID*)&g_setTone);
    }
    if (!g_setTone) { g_failure->Set("the HDR scale hook is unavailable"); ReleaseContext(context); return; }
    float savedTone[3];
    if (!ToneScale(context, savedTone, false)) {
        g_failure->Set("the HDR tone mapping scale could not be saved"); ReleaseContext(context); return;
    }
    float linearTone[3] = { 1, 1, 1 };
	const unsigned char black[3] = { 0, 0, 0 };
    SnapshotSubjects();
    std::copy(savedTone,savedTone+3,g_beautyTone);
    // Players' depth is what is closer than a world-only draw from the same simulation state.
    // Keep this INTZ surface separate from HLAE's working surface, shared by both isolated outputs.
    bool worldReference = false;
    for (const auto& pass : passes) worldReference |= PassDepthMask(pass.kind) == 2;
    if (worldReference) {
        if (!g_worldSurface) g_worldSurface = AfxCreateCompatibleDepthStencilINTZTextureSurface(AfxGetDepthStencilSurface());
        if (!g_worldSurface || (!g_worldDepth && FAILED(g_worldSurface->GetContainer(__uuidof(IDirect3DTexture9),(void**)&g_worldDepth)))) {
            g_failure->Set("HLAE could not create the world reference depth"); ReleaseContext(context); return;
        }
        alignas(16) unsigned char reference[kViewSetupSize]; std::memcpy(reference,view,sizeof(reference));
        reference[kViewBloomAndToneMapping] = 0;
        Set("r_skybox","0"); Set("r_3dsky","0"); Set("r_drawparticles","0"); Set("r_shadows","0");
        const bool msaa = g_AfxStreams.DrawingThread_HasRenderTargetMsaa();
        if (msaa) g_AfxStreams.DrawingThread_SetRenderTargetNoMsaa();
        AfxD3d9PushDepthStencilEx(g_worldSurface);
        g_passContext = context; g_activePass = ExtraPass::World;
        const bool drawn = DrawView(client,reference,0);
        g_passContext = nullptr;
        AfxD3d9PopDepthStencil();
        if (msaa) g_AfxStreams.DrawingThread_UnsetRenderTargetNoMsaa(false);
        Restore(); ToneScale(context,savedTone,true);
        if (!drawn) { g_failure->Set("the world reference draw failed"); ReleaseContext(context); return; }
    }
	for (const Pass& pass : passes) {
		Restore();
        ToneScale(context, savedTone, true);
		alignas(16) unsigned char extra[kViewSetupSize];
		std::memcpy(extra, view, sizeof(extra));
        const bool depth = PassNeedsDepth(pass.kind);
        const bool normal = PassIsNormal(pass.kind);
        const int mask = PassDepthMask(pass.kind);
        const bool sourcePass = normal || mask;
        const bool fog = pass.kind == ExtraPass::KeyGun || pass.kind == ExtraPass::KeyPlayers;
        const bool world = pass.kind == ExtraPass::World || pass.kind == ExtraPass::DepthWorld || pass.kind == ExtraPass::NormalWorld;
        const int flags = pass.kind == ExtraPass::NoGun || pass.kind == ExtraPass::DepthNoGun || pass.kind == ExtraPass::NormalNoGun || mask == 2 || world || pass.kind == ExtraPass::KeyPlayers ? 0 : kRenderViewDrawViewModel;
        if (fog || depth) extra[kViewBloomAndToneMapping] = 0;
        if (world) Set("r_shadows","0");
        if (fog) { Fog(look.key,0,1); ToneScale(context,linearTone,true); }
        if (depth) {
            // The same INTZ buffer and draw shader HLAE's zDepth streams use. No fog/material
            // reconstruction: a surface's colour cannot contaminate the depth.
            Set("r_skybox","0"); Set("r_3dsky","0"); Set("r_drawparticles","0");
            if (!g_AfxStreams.DrawingThread_GetOrCreateIntZTextureSurface()) { g_failure->Set("HLAE could not create its depth surface"); break; }
            g_AfxStreams.DrawingThread_SetIntZTextureSurface();
            if (g_AfxStreams.DrawingThread_HasRenderTargetMsaa()) g_AfxStreams.DrawingThread_SetRenderTargetNoMsaa();
        }
        ClearColor(context,fog ? look.key : black);
        g_passContext = context; g_activePass = pass.kind; g_linearTone = fog;
        const bool beauty = !depth && !fog;
        if (beauty) Lens_ViewBegin(true,flags);
        const bool drawn = DrawView(client,extra,flags);
        if (beauty) Lens_ViewEnd();
        g_linearTone = false; g_passContext = nullptr;
        float projection[4][4] = {};
        const float zn = *(const float*)(view+kViewNear), zf = *(const float*)(view+kViewFar);
        if (sourcePass) {
            const float width = (float)*(const int*)(view+kViewWidth), height = (float)*(const int*)(view+kViewHeight);
            const float tangent = (float)std::tan(View_ScreenFov(*(const float*)(view+kViewFov),width,height)*3.141592653589793/360.0);
            projection[0][0] = 1/tangent; projection[1][1] = width/(height*tangent);
            projection[2][2] = zf/(zf-zn); projection[2][3] = -zf*zn/(zf-zn); projection[3][2] = 1;
        }
        AfxDepthPassExtras extras; extras.mask = mask; extras.worldDepth = g_worldDepth;
        if (drawn && depth) AfxDrawDepth(normal ? AfxDrawDepthEncode_Normal : (mask ? AfxDrawDepthEncode_MaskedLinear : AfxDrawDepthEncode_Gray),AfxDrawDepthMode_PyramidalLinear,false,
            look.depthFar,look.depthNear,0,0,*(const int*)(view+kViewWidth),*(const int*)(view+kViewHeight),
            zn,zf,sourcePass ? projection : nullptr,&extras);
        if (drawn && pass.node) pass.node->GpuCapture();
        if (depth) {
            if (g_AfxStreams.DrawingThread_HasRenderTargetMsaa()) g_AfxStreams.DrawingThread_UnsetRenderTargetNoMsaa(false);
            g_AfxStreams.DrawingThread_UnsetIntZTextureSurface();
        }
        if (!drawn) { g_failure->Set("the extra view render failed"); break; }

	}
	Restore();
    ToneScale(context, savedTone, true);
	ClearColor(context, black);
    if (!preview) {
        Lens_ViewBegin(true);
        if (!DrawView(client,view,kRenderViewDrawViewModel | kRenderViewDrawHud)) g_failure->Set("the Beauty view could not be restored");
        Lens_ViewEnd();
    }
    ToneScale(context, savedTone, true);
	ReleaseContext(context);
	if (preview) return;
	++g_frames;
	if (Filmmaker::MvmDebugLog_Active() && (g_frames <= 3 || g_frames % 60 == 0))
		Filmmaker::MvmDebugLog_Linef("render.passes", "frame %d: %u pass(es), view %dx%d", g_frames, (unsigned)passes.size(),
			*(const int*)(view + kViewWidth), *(const int*)(view + kViewHeight));
}

void __fastcall HookViewRender(void* client,
#ifndef _WIN64
	void*,
#endif
	void* rect) {
    Lens_ViewBegin();
	g_original(client, rect);
    Lens_ViewEnd();
    if (g_armed) {
        g_armed = false;
        if (!g_passes.empty()) DrawPasses(client,g_passes,g_look,false);
    } else if (g_previewEnabled && !Filmmaker::RenderSessionDriver_IsActive() && Demo_State().playing) {
        const std::vector<Pass> preview{{g_previewKind,nullptr}};
        if (g_previewWarmFrames > 0) --g_previewWarmFrames;
        else DrawPasses(client,preview,g_previewLook,true);
    }
}

} // namespace

void RenderPasses_Install() {
	if (g_original || g_SourceSdkVer != SourceSdkVer_CSS || sizeof(void*) != 8) return;
	HMODULE clientDll = GetModuleHandleW(L"client.dll"), materialsDll = GetModuleHandleW(L"materialsystem.dll");
	auto clientFactory = clientDll ? (Factory)GetProcAddress(clientDll, "CreateInterface") : nullptr;
	auto materialsFactory = materialsDll ? (Factory)GetProcAddress(materialsDll, "CreateInterface") : nullptr;
	g_client = clientFactory ? clientFactory("VClient017", nullptr) : nullptr;
	g_materials = materialsFactory ? materialsFactory("VMaterialSystem080", nullptr) : nullptr;
	if (!g_client || !g_materials) return;
	void** slot = &(*(void***)g_client)[Slot::Client_View_Render];
	AfxDetourPtr((PVOID*)slot, (PVOID)&HookViewRender, (PVOID*)&g_original);
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    auto factory = engine ? (Factory)GetProcAddress(engine,"CreateInterface") : nullptr;
    void* model = factory ? factory("VEngineModel016",nullptr) : nullptr;
    if (model) AfxDetourPtr((PVOID*)&(*(void***)model)[Slot::ModelRender_DrawModelExecute],(PVOID)&HookDrawModel,(PVOID*)&g_drawModel);
}

bool RenderPasses_Installed() { return g_original != nullptr; }

bool RenderPasses_LensDepth(int drawFlags,bool smoke) {
    alignas(16) unsigned char view[kViewSetupSize]={};
    if(!g_client || !PlayerView(g_client,view)) return false;
    void* context=Context();if(!context) return false;
    float tone[3];const bool saved=ToneScale(context,tone,false);
    // The helper supplies depth, never another exposure measurement or a second HUD.
    view[kViewBloomAndToneMapping]=0;
    if(smoke) Lens_DepthMaterialScope(context,true);
    const bool drawn=saved && DrawView(g_client,view,drawFlags & ~kRenderViewDrawHud);
    if(smoke) Lens_DepthMaterialScope(context,false);
    if(saved) ToneScale(context,tone,true);
    ReleaseContext(context);
    return drawn;
}

bool RenderPasses_Begin(const std::vector<ExtraPassJob>& passes, const ExtraPassLook& look, std::string& error) {
	RenderPasses_End();
	g_failure = std::make_shared<Failure>();
	g_error.clear(); g_frames = 0; g_toneCalls = 0; g_look = look;
	if (passes.empty()) return true;
	RenderPasses_Install();
	if (!g_original || !g_drawModel) { error = "the client render hooks are unavailable"; return false; }
    for (const auto& pass : passes) if (!PrepareDraw(pass.kind,error)) return false;

	for (const ExtraPassJob& pass : passes) {
		if (!pass.creator) { error = "a pass has no output writer"; RenderPasses_End(); return false; }
		ICaptureOutput* output = CAfxStreams::NewScreenOutput(pass.creator);
		auto* node = new CCaptureNode(new CCaptureInputRenderTarget(), output);
		node->AddRef();
		g_passes.push_back({ pass.kind, node });
	}
	return true;
}

void RenderPasses_Arm() { g_armed = !g_passes.empty(); }

void RenderPasses_Preview(ExtraPass kind, const ExtraPassLook& look, bool enabled) {
    if (!enabled) {
        if (g_previewEnabled && g_previewQueue) ConVar_SetString(g_previewQueue,g_previewSavedQueue.c_str());
        g_previewEnabled = false;
        if (g_passes.empty()) {
            if (g_worldDepth) { g_worldDepth->Release(); g_worldDepth = nullptr; }
            if (g_worldSurface) { g_worldSurface->Release(); g_worldSurface = nullptr; }
        }
        return;
    }
    if (g_previewEnabled && kind == g_previewKind && look.depthNear == g_previewLook.depthNear
        && look.depthFar == g_previewLook.depthFar && std::equal(look.key,look.key+3,g_previewLook.key)) return;
    RenderPasses_Install();
    std::string error;
    if (!g_original || !g_drawModel || !PrepareDraw(kind,error)) {
        advancedfx::Warning("render preview: %s\n",error.empty() ? "the render hook is unavailable" : error.c_str());
        return;
    }
    if (!g_previewEnabled) {
        g_previewQueue = ConVar_Find("mat_queue_mode");
        if (!g_previewQueue || !ConVar_GetString(g_previewQueue,g_previewSavedQueue)) return;
        ConVar_SetString(g_previewQueue,"0");
        g_previewWarmFrames = 2;
    }
    g_previewKind = kind; g_previewLook = look; g_previewEnabled = true;
    if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("render.preview","pass %d, depth %g..%g, key %u %u %u",
        (int)kind,look.depthNear,look.depthFar,look.key[0],look.key[1],look.key[2]);
}

void RenderPasses_DeviceLost() {
    if (g_worldDepth) { g_worldDepth->Release(); g_worldDepth = nullptr; }
    if (g_worldSurface) { g_worldSurface->Release(); g_worldSurface = nullptr; }
    if (Filmmaker::RenderSessionDriver_IsActive()) g_failure->Set("the render device was reset during capture; start the job again");
}

void RenderPasses_End() {
	g_armed = false;
	for (Pass& pass : g_passes) { pass.node->GpuRelease(); pass.node->Release(); }
	g_passes.clear();
    if (g_worldDepth) { g_worldDepth->Release(); g_worldDepth = nullptr; }
    if (g_worldSurface) { g_worldSurface->Release(); g_worldSurface = nullptr; }
}

int RenderPasses_Frames() { return g_frames; }

const std::string& RenderPasses_Error() {
	std::lock_guard<std::mutex> lock(g_failure->mutex);
	g_error = g_failure->message;
	return g_error;
}

void RenderPasses_PrintHealth() {
	advancedfx::Message("  render passes: %s, %u active, %d frames, %s\n", g_original ? "hooked" : "not installed",
		(unsigned)g_passes.size(), g_frames, RenderPasses_Error().empty() ? "no error" : g_error.c_str());
}

} // namespace SourceFilmmaker
