#include "RenderPlayersOnlyGreen.h"
#include "RenderPassPlan.h"
#include "../../RenderSystemDX11Hooks.h"

#include "RenderWorldPlayersNoGun.h"
#include "../Debug/MvmDebugLog.h"
#include "../../SceneSystem.h"
#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

constexpr const char* kChromaMaterial = "materials/filmmaker/chromakey_unlit.vmat";
// Swap target for SKIP-CLASS elements only: green in its albedo with a white tint, so foreign
// pipelines that ignore the self-illum combo still render GREEN rather than black. See the
// header comment in chromakey_skip.vmat for the full reasoning.
constexpr const char* kChromaSkipMaterial = "materials/filmmaker/chromakey_skip.vmat";
constexpr const char* kDrawDecalsCvar = "r_drawdecals";
constexpr const char* kDrawSkyboxCvar = "r_drawskybox";
// Hidden in the retail cvar listing, but still honoured by the client renderer. The ordinary
// skybox switch leaves the projected 3D sky mesh (clouds/geometry) alive, which is exactly the
// black/fogged backdrop that contaminates a chroma plate.
constexpr const char* kDraw3dSkyboxCvar = "r_draw3dskybox";
constexpr const char* kDrawRopesCvar = "r_drawropes";
constexpr const char* kDrawTracersCvar = "r_drawtracers";
constexpr const char* kDrawTracersFirstPersonCvar = "r_drawtracers_firstperson";
constexpr const char* kRenderDecalsCvar = "r_csgo_render_decals";
constexpr const char* kRenderDecalsTranslucentCvar = "r_csgo_render_decals_on_translucent";
constexpr const char* kRenderInfernoDecalsCvar = "r_csgo_render_inferno_decals";
constexpr const char* kHumanBloodCvar = "violence_hblood";
constexpr const char* kAlienBloodCvar = "violence_ablood";
constexpr const char* kFogCvar = "fog_enable";
constexpr const char* kSkyFogCvar = "fog_enableskybox";
// CS2's GPU-instanced-mesh scatter (grass tufts, ground leaf/debris litter) renders through
// GPU-driven indirect draws that no scene hook, D3D draw hook, element write, or material poke
// can reach (measured exhaustively 2026-08-20; identified in -tools mode on the cache remake).
// A huge size-cull bias makes the system cull every instance -- the whole scatter layer
// disappears, which is exactly what a chroma plate wants.
// CS2's GPU-instanced-mesh scatter (grass tufts, ground litter) is drawn by GPU-driven
// INDIRECT draws. The sc_instanced_mesh_* cvars that control it exist ONLY in -tools mode:
// measured 2026-08-20 in a retail demo session, sc_instanced_mesh_size_cull_bias changed
// 0.006%% of the frame while a known-good control (r_drawviewmodel) changed 5.4%%. The
// scatter is dropped at the D3D indirect-draw hooks instead; this flag gates that.
std::atomic<bool> g_indirectSkip{true};
constexpr unsigned kSceneCvarReassertFrames = 16;

std::atomic<bool> g_armed{false};
std::atomic<bool> g_renderActive{false};
std::atomic<int> g_renderOrdinal{-1};
std::atomic<bool> g_includeSmoke{true};
std::atomic<bool> g_viewmodelPlayerSide{false};
std::atomic<CMaterial2*> g_material{nullptr};
std::atomic<CMaterial2*> g_skipMaterial{nullptr};
std::atomic<CMaterial2*> g_invisibleMaterial{nullptr};
std::atomic<unsigned long long> g_admitted{0};
std::atomic<unsigned long long> g_replaced{0};
std::atomic<unsigned long long> g_effects{0};
std::atomic<unsigned long long> g_unknown{0};
std::atomic<unsigned long long> g_decalsSkipped{0};
char g_reason[64] = "";

// Skip-class material suppression registry. The decisive fact (measured 2026-08-20): the leaking
// detail foliage / debris / overlay draws replay from LEVEL-LIFETIME cached command lists that
// were recorded before the pass armed, so no draw-time mechanism (scene-hook batch skip, element
// material/colour writes, D3D draw skip) can remove them -- but their MATERIAL CONSTANTS are
// re-uploaded every frame. Poking the material itself therefore reaches even cached draws:
// g_flAlphaTestReference = 2.0 makes every alpha-tested pixel fail (tufts, scraps vanish), and a
// pure-green g_vColorTint neutralises blend pipelines. The scene hooks report skip-class material
// pointers from the render threads; the main-thread pump applies the pokes; disarm restores the
// saved values. Level init CLEARS WITHOUT RESTORING -- the pointers die with the level.
struct SuppressedMaterialState {
	bool hasAlphaRef = false;
	float alphaRef = 0.5f;
	bool hasTint = false;
	float tint[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
};
std::mutex g_suppressMutex;
std::vector<CMaterial2*> g_suppressPending;
std::unordered_map<CMaterial2*, SuppressedMaterialState> g_suppressed;
// Skip-class materials WITHOUT an alpha-test reference are opaque (rock/cloth debris chunks).
// The alpha poke cannot remove those, but the ordinary chroma swap is safe on them -- the
// black-splat failure mode only exists for alpha-tested/translucent atlases. The pump publishes
// this verdict; the render-thread Apply loop queries it under the same mutex.
std::unordered_set<CMaterial2*> g_suppressedWantsChroma;

// Runtime-extendable shader-skip vocabulary (see RenderPlayersOnlyGreen_ShaderNameIsSkipped).
// The builtins live in the query function; this list is the console-driven experimentation
// surface (`render playersonlygreen skipshader add <substr>`), which is how a new leak
// pipeline gets identified live before its needle is promoted to a builtin.
std::mutex g_skipShaderMutex;
std::vector<std::string> g_skipShaderNeedles;
std::atomic<unsigned> g_skipShaderGeneration{1};

bool g_sceneCvarsApplied = false;
std::string g_previousDrawDecals;
std::string g_previousDrawSkybox;
std::string g_previousDraw3dSkybox;
std::string g_previousDrawRopes;
std::string g_previousDrawTracers;
std::string g_previousDrawTracersFirstPerson;
std::string g_previousRenderDecals;
std::string g_previousRenderDecalsTranslucent;
std::string g_previousRenderInfernoDecals;
std::string g_previousHumanBlood;
std::string g_previousAlienBlood;
std::string g_previousFog;
std::string g_previousSkyFog;
unsigned g_sceneCvarReassertFrame = 0;

bool StartsWith(const char* value, const char* prefix) {
	return value && prefix && 0 == std::strncmp(value, prefix, std::strlen(prefix));
}

bool ContainsNoCase(const char* value, const char* needle) {
	if (!value || !needle || !*needle) return false;
	const size_t n = std::strlen(needle);
	for (const char* p = value; *p; ++p)
		if (0 == _strnicmp(p, needle, n)) return true;
	return false;
}

bool IsFirstPersonMaterial(const char* materialName) {
	return ContainsNoCase(materialName, "_fp")
		|| ContainsNoCase(materialName, "_fps")
		|| ContainsNoCase(materialName, "viewmodel")
		|| ContainsNoCase(materialName, "/arms_fp");
}

void ResetCounters() {
	g_admitted.store(0, std::memory_order_relaxed);
	g_replaced.store(0, std::memory_order_relaxed);
	g_effects.store(0, std::memory_order_relaxed);
	g_unknown.store(0, std::memory_order_relaxed);
	g_decalsSkipped.store(0, std::memory_order_relaxed);
}

void RunClientCmd(const char* cmd) {
	if (g_pEngineToClient && cmd)
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

std::string ReadBoolCvarValue(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback ? fallback : "0";
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback ? fallback : "0";
	return handle->m_Value.m_bValue ? "1" : "0";
}

std::string ReadFloatCvarValue(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback ? fallback : "0";
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback ? fallback : "0";
	char buf[48];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%g", handle->m_Value.m_flValue);
	return buf;
}

void ApplySceneCvars() {
	if (g_sceneCvarsApplied) return;
	g_previousDrawDecals = ReadBoolCvarValue(kDrawDecalsCvar, "1");
	g_previousDrawSkybox = ReadBoolCvarValue(kDrawSkyboxCvar, "1");
	g_previousDraw3dSkybox = ReadBoolCvarValue(kDraw3dSkyboxCvar, "1");
	g_previousDrawRopes = ReadBoolCvarValue(kDrawRopesCvar, "1");
	g_previousDrawTracers = ReadBoolCvarValue(kDrawTracersCvar, "1");
	g_previousDrawTracersFirstPerson = ReadBoolCvarValue(kDrawTracersFirstPersonCvar, "1");
	g_previousRenderDecals = ReadBoolCvarValue(kRenderDecalsCvar, "1");
	g_previousRenderDecalsTranslucent = ReadBoolCvarValue(kRenderDecalsTranslucentCvar, "1");
	g_previousRenderInfernoDecals = ReadBoolCvarValue(kRenderInfernoDecalsCvar, "1");
	g_previousHumanBlood = ReadBoolCvarValue(kHumanBloodCvar, "1");
	g_previousAlienBlood = ReadBoolCvarValue(kAlienBloodCvar, "1");
	g_previousFog = ReadBoolCvarValue(kFogCvar, "1");
	g_previousSkyFog = ReadBoolCvarValue(kSkyFogCvar, "1");
	// These are the non-scene overlays that can bypass the material detours. They are deliberately
	// narrower than the old "hide effects" switches: gameplay particles and volumetric smoke stay
	// enabled, while decals/blood, tracers, fog and rope/wire helpers cannot contaminate the
	// plate. The sky dome is deliberately forced ON: its material is overridden with the
	// solid-green sky (SceneSystem_SetPlayersOnlyGreenSkyOverride), so the dome IS the chroma
	// backdrop -- with it off, sky pixels stay at the scene target's black clear. The projected
	// 3D skybox stays off because its geometry draws with normal materials over the green dome.
	RunClientCmd("r_drawdecals 0");
	RunClientCmd("r_drawskybox 1");
	RunClientCmd("r_draw3dskybox 0");
	RunClientCmd("r_drawropes 0");
	RunClientCmd("r_drawtracers 0");
	RunClientCmd("r_drawtracers_firstperson 0");
	RunClientCmd("r_csgo_render_decals 0");
	RunClientCmd("r_csgo_render_decals_on_translucent 0");
	RunClientCmd("r_csgo_render_inferno_decals 0");
	RunClientCmd("violence_hblood 0");
	RunClientCmd("violence_ablood 0");
	RunClientCmd("fog_enable 0");
	RunClientCmd("fog_enableskybox 0");
	// Clear already-materialized surface decals once when the policy is armed. The draw cvars and
	// scene-material classifier cover subsequent submissions; this one-shot also removes decals
	// that were created before the pass (the common case after seeking a repair demo).
	RunClientCmd("r_cleardecals");
	g_sceneCvarsApplied = true;
	g_sceneCvarReassertFrame = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen",
			"scene cvars suppressed: decals=0 skybox=1(green dome) skybox3d=0 ropes=0 tracers=0 fog=0 blood=0 "
			"previous=(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s) smoke/particles unchanged",
			g_previousDrawDecals.c_str(), g_previousDrawSkybox.c_str(), g_previousDraw3dSkybox.c_str(),
			g_previousDrawRopes.c_str(),
			g_previousDrawTracers.c_str(), g_previousDrawTracersFirstPerson.c_str(),
			g_previousRenderDecals.c_str(), g_previousRenderDecalsTranslucent.c_str(),
			g_previousRenderInfernoDecals.c_str(), g_previousHumanBlood.c_str(),
			g_previousAlienBlood.c_str());
}

void RestoreSceneCvars() {
	if (!g_sceneCvarsApplied) return;
	const std::string decals = g_previousDrawDecals.empty() ? "1" : g_previousDrawDecals;
	const std::string skybox = g_previousDrawSkybox.empty() ? "1" : g_previousDrawSkybox;
	const std::string skybox3d = g_previousDraw3dSkybox.empty() ? "1" : g_previousDraw3dSkybox;
	const std::string ropes = g_previousDrawRopes.empty() ? "1" : g_previousDrawRopes;
	const std::string tracers = g_previousDrawTracers.empty() ? "1" : g_previousDrawTracers;
	const std::string tracersFirstPerson = g_previousDrawTracersFirstPerson.empty() ? "1" : g_previousDrawTracersFirstPerson;
	const std::string renderDecals = g_previousRenderDecals.empty() ? "1" : g_previousRenderDecals;
	const std::string renderDecalsTranslucent = g_previousRenderDecalsTranslucent.empty() ? "1" : g_previousRenderDecalsTranslucent;
	const std::string renderInfernoDecals = g_previousRenderInfernoDecals.empty() ? "1" : g_previousRenderInfernoDecals;
	const std::string humanBlood = g_previousHumanBlood.empty() ? "1" : g_previousHumanBlood;
	const std::string alienBlood = g_previousAlienBlood.empty() ? "1" : g_previousAlienBlood;
	const std::string fog = g_previousFog.empty() ? "1" : g_previousFog;
	const std::string skyFog = g_previousSkyFog.empty() ? "1" : g_previousSkyFog;
	char cmd[96];
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawDecalsCvar, decals.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawSkyboxCvar, skybox.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDraw3dSkyboxCvar, skybox3d.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawRopesCvar, ropes.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawTracersCvar, tracers.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawTracersFirstPersonCvar, tracersFirstPerson.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kRenderDecalsCvar, renderDecals.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kRenderDecalsTranslucentCvar, renderDecalsTranslucent.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kRenderInfernoDecalsCvar, renderInfernoDecals.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kHumanBloodCvar, humanBlood.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kAlienBloodCvar, alienBlood.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kFogCvar, fog.c_str());
	RunClientCmd(cmd);
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kSkyFogCvar, skyFog.c_str());
	RunClientCmd(cmd);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen",
			"scene cvars restored: decals=%s skybox=%s skybox3d=%s ropes=%s tracers=%s/%s fog=%s/%s blood=%s/%s",
			decals.c_str(), skybox.c_str(), skybox3d.c_str(), ropes.c_str(), tracers.c_str(), tracersFirstPerson.c_str(),
			fog.c_str(), skyFog.c_str(), humanBlood.c_str(), alienBlood.c_str());
	g_previousDrawDecals.clear();
	g_previousDrawSkybox.clear();
	g_previousDraw3dSkybox.clear();
	g_previousDrawRopes.clear();
	g_previousDrawTracers.clear();
	g_previousDrawTracersFirstPerson.clear();
	g_previousRenderDecals.clear();
	g_previousRenderDecalsTranslucent.clear();
	g_previousRenderInfernoDecals.clear();
	g_previousHumanBlood.clear();
	g_previousAlienBlood.clear();
	g_previousFog.clear();
	g_previousSkyFog.clear();
	g_sceneCvarsApplied = false;
	g_sceneCvarReassertFrame = 0;
}

void ReconcileSceneCvars(bool desired) {
	if (desired != g_sceneCvarsApplied) {
		if (desired) ApplySceneCvars();
		else RestoreSceneCvars();
		return;
	}
	if (!desired) return;
	if (++g_sceneCvarReassertFrame < kSceneCvarReassertFrames) return;
	g_sceneCvarReassertFrame = 0;
	if (ReadBoolCvarValue(kDrawDecalsCvar, "0") != "0") RunClientCmd("r_drawdecals 0");
	if (ReadBoolCvarValue(kDrawSkyboxCvar, "1") != "1") RunClientCmd("r_drawskybox 1");
	if (ReadBoolCvarValue(kDraw3dSkyboxCvar, "0") != "0") RunClientCmd("r_draw3dskybox 0");
	if (ReadBoolCvarValue(kDrawRopesCvar, "0") != "0") RunClientCmd("r_drawropes 0");
	if (ReadBoolCvarValue(kDrawTracersCvar, "0") != "0") RunClientCmd("r_drawtracers 0");
	if (ReadBoolCvarValue(kDrawTracersFirstPersonCvar, "0") != "0") RunClientCmd("r_drawtracers_firstperson 0");
	if (ReadBoolCvarValue(kRenderDecalsCvar, "0") != "0") RunClientCmd("r_csgo_render_decals 0");
	if (ReadBoolCvarValue(kRenderDecalsTranslucentCvar, "0") != "0") RunClientCmd("r_csgo_render_decals_on_translucent 0");
	if (ReadBoolCvarValue(kRenderInfernoDecalsCvar, "0") != "0") RunClientCmd("r_csgo_render_inferno_decals 0");
	if (ReadBoolCvarValue(kHumanBloodCvar, "0") != "0") RunClientCmd("violence_hblood 0");
	if (ReadBoolCvarValue(kAlienBloodCvar, "0") != "0") RunClientCmd("violence_ablood 0");
	if (ReadBoolCvarValue(kFogCvar, "0") != "0") RunClientCmd("fog_enable 0");
	if (ReadBoolCvarValue(kSkyFogCvar, "0") != "0") RunClientCmd("fog_enableskybox 0");
}

// Main thread. Drains the render-thread-reported skip-class materials and applies the
// suppression pokes; see the registry comment above for why this is the mechanism that works.
void PumpSuppressedMaterials() {
	std::vector<CMaterial2*> pending;
	{
		std::lock_guard<std::mutex> lock(g_suppressMutex);
		pending.swap(g_suppressPending);
	}
	for (CMaterial2* material : pending) {
		if (!material || g_suppressed.count(material)) continue;
		SuppressedMaterialState state;
		bool changed = false;
		if (float* alphaRef = SceneSystem_FindMaterialParam(material, "g_flAlphaTestReference")) {
			state.hasAlphaRef = true;
			state.alphaRef = *alphaRef;
			*alphaRef = 2.0f; // no pixel can pass -- alpha-tested geometry disappears
			changed = true;
		}
		if (float* tint = SceneSystem_FindMaterialParam(material, "g_vColorTint")) {
			state.hasTint = true;
			for (int i = 0; i < 4; ++i) state.tint[i] = tint[i];
			tint[0] = 0.0f; tint[1] = 1.0f; tint[2] = 0.0f; tint[3] = 0.0f;
			changed = true;
		}
		if (changed) SceneSystem_UpdateMaterial(material);
		g_suppressed.emplace(material, state);
		{
			// EVERY skip-class material goes back onto the chroma repaint, alpha-tested ones
			// included. The alpha-test poke alone cannot erase them: the engine clamps
			// g_flAlphaTestReference to [0,1], so solid leaf interiors still pass and the sprite
			// renders as a dark silhouette (measured on de_cache detail foliage 2026-08-20).
			// Swapping to the chroma material draws the whole quad flat emissive green, which is
			// invisible against the plate. This is only safe because the chroma material now
			// kills its lit term (black colour tint + albedo factor 0); the black splats this
			// used to cause came from the older lit chroma, not from the swap itself.
			std::lock_guard<std::mutex> lock(g_suppressMutex);
			g_suppressedWantsChroma.insert(material);
		}
	if (MvmDebugLog_Active()) {
			char name[256] = {};
			SceneSystem_MaterialName(material, name, sizeof(name));
			MvmDebugLog_Linef("render.playersonlygreen.suppress",
				"material='%s' alphaRef=%d(was %.2f) tint=%d chromaSwap=%d applied=%d total=%u",
				name[0] ? name : "<unnamed>", state.hasAlphaRef ? 1 : 0, state.alphaRef,
				state.hasTint ? 1 : 0, state.hasAlphaRef ? 0 : 1, changed ? 1 : 0,
				(unsigned)g_suppressed.size());
		}
	}
}

void RestoreSuppressedMaterials() {
	unsigned restored = 0;
	for (auto& entry : g_suppressed) {
		CMaterial2* material = entry.first;
		const SuppressedMaterialState& state = entry.second;
		bool changed = false;
		if (state.hasAlphaRef) {
			if (float* alphaRef = SceneSystem_FindMaterialParam(material, "g_flAlphaTestReference")) {
				*alphaRef = state.alphaRef;
				changed = true;
			}
		}
		if (state.hasTint) {
			if (float* tint = SceneSystem_FindMaterialParam(material, "g_vColorTint")) {
				for (int i = 0; i < 4; ++i) tint[i] = state.tint[i];
				changed = true;
			}
		}
		if (changed) {
			SceneSystem_UpdateMaterial(material);
			++restored;
		}
	}
	if (restored && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen.suppress",
			"restored %u suppressed material(s)", restored);
	g_suppressed.clear();
	std::lock_guard<std::mutex> lock(g_suppressMutex);
	g_suppressPending.clear();
	g_suppressedWantsChroma.clear();
}

} // namespace

void RenderPlayersOnlyGreen_NoteSkipClassMaterial(CMaterial2* material) {
	if (!material) return;
	std::lock_guard<std::mutex> lock(g_suppressMutex);
	if (g_suppressPending.size() < 512)
		g_suppressPending.push_back(material);
}

bool RenderPlayersOnlyGreen_SkipClassWantsChroma(CMaterial2* material) {
	if (!material) return false;
	std::lock_guard<std::mutex> lock(g_suppressMutex);
	return g_suppressedWantsChroma.count(material) != 0;
}

bool RenderPlayersOnlyGreen_PrepareMaterial() {
	CMaterial2* material = SceneSystem_ResolveMaterial(kChromaMaterial);
	if (material) {
		// Keep the plate flat even if a future material revision enables one of these optional
		// inputs. These writes are main-thread-only and happen before the first draw can observe it.
		bool changed = false;
		if (float* fog = SceneSystem_FindMaterialParam(material, "g_bFogEnabled")) {
			*fog = 0.0f; changed = true;
		}
		if (float* tint = SceneSystem_FindMaterialParam(material, "g_flModelTintAmount")) {
			*tint = 0.0f; changed = true;
		}
		if (float* illum = SceneSystem_FindMaterialParam(material, "g_flSelfIllumBrightness")) {
			*illum = 1.0f; changed = true;
		}
		if (float* scale = SceneSystem_FindMaterialParam(material, "g_flSelfIllumScale")) {
			*scale = 1.0f; changed = true;
		}
		// Flatness comes from EMISSION, which does not depend on scene lighting: the self-illum
		// term alone already saturates every plate pixel to 255 green, so the plate is flat no
		// matter what the colour tint is. That makes the tint free to solve a different problem --
		// a BLACK tint renders BLACK wherever this material is bound by a foreign pipeline with no
		// self-illum combo (csgo_simple foliage, decal renderers), measured on de_cache 2026-08-20.
		// A mid-grey tint over the flat green albedo keeps those draws GREEN while the emissive
		// plate stays clamped and unchanged, and brightness stays at 1 so nothing blooms.
		if (float* albedoFactor = SceneSystem_FindMaterialParam(material, "g_flSelfIllumAlbedoFactor")) {
			*albedoFactor = 0.0f; changed = true;
		}
		if (float* color = SceneSystem_FindMaterialParam(material, "g_vColorTint")) {
			color[0] = 0.5f; color[1] = 0.5f; color[2] = 0.5f; color[3] = 0.0f;
			changed = true;
		}
		if (float* selfColor = SceneSystem_FindMaterialParam(material, "g_vSelfIllumTint")) {
			selfColor[0] = 0.0f; selfColor[1] = 1.0f; selfColor[2] = 0.0f; selfColor[3] = 0.0f;
			changed = true;
		}
		if (changed) SceneSystem_UpdateMaterial(material);
	}
	CMaterial2* skipMaterial = SceneSystem_ResolveMaterial(kChromaSkipMaterial);
	if (skipMaterial) {
		// Keep the lit path green too: albedo is already green, so a WHITE tint (not the
		// black one the world chroma uses) is what stops a foreign pipeline going black.
		if (float* fog = SceneSystem_FindMaterialParam(skipMaterial, "g_bFogEnabled")) *fog = 0.0f;
		if (float* tint = SceneSystem_FindMaterialParam(skipMaterial, "g_flModelTintAmount")) *tint = 0.0f;
		if (float* color = SceneSystem_FindMaterialParam(skipMaterial, "g_vColorTint")) {
			color[0] = 1.0f; color[1] = 1.0f; color[2] = 1.0f; color[3] = 0.0f;
		}
		if (float* selfColor = SceneSystem_FindMaterialParam(skipMaterial, "g_vSelfIllumTint")) {
			selfColor[0] = 0.0f; selfColor[1] = 1.0f; selfColor[2] = 0.0f; selfColor[3] = 0.0f;
		}
		SceneSystem_UpdateMaterial(skipMaterial);
	}
	g_skipMaterial.store(skipMaterial, std::memory_order_release);
	g_material.store(material, std::memory_order_release);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen",
			"prepare material='%s' resolved=%d skip='%s' resolved=%d fog=0 selfIllum=1",
			kChromaMaterial, material ? 1 : 0, kChromaSkipMaterial, skipMaterial ? 1 : 0);
	return material != nullptr;
}

void RenderPlayersOnlyGreen_RunMainThreadFrame() {
	// GLOBAL state (scene cvars, the sky dome material, material pokes) belongs to the live preview
	// only. A render job's Players Only render is one extra render inside a frame whose Main render
	// is the full beauty scene, so the job path must never change anything outside its own draws:
	// it works purely through the per-pass scene-hook substitution and the D3D draw skips, and the
	// pass compositor keys the sky by depth (Filmmaker/Render/RenderPassPlan.h).
	const bool desired = g_armed.load(std::memory_order_acquire);
	ReconcileSceneCvars(desired);
	// The map sky is emitted by a dedicated env_sky path rather than the scene-material hooks.
	// Override its dome material with the solid-green sky on the main thread: the sky itself IS
	// the chroma backdrop (a suppressed sky leaves black scene-clear pixels instead). World
	// geometry, depth and gameplay smoke remain untouched.
	SceneSystem_SetPlayersOnlyGreenSkyOverride(desired);
	// Skip-class material suppression: apply pokes reported by the render threads while the
	// pass wants them, put everything back the frame it stops wanting them. Job-wide like the
	// sky override -- renderActive flickers per pass bracket and must not trigger a mid-job
	// restore.
	const bool suppressWanted = desired;
	if (suppressWanted) PumpSuppressedMaterials();
	else if (!g_suppressed.empty()) RestoreSuppressedMaterials();
	if (!g_armed.load(std::memory_order_acquire)) return;
	if (!g_material.load(std::memory_order_acquire))
		RenderPlayersOnlyGreen_PrepareMaterial();
	SceneSystem_SetPlayersOnlyGreenPolicy(g_armed.load(std::memory_order_acquire)
		|| g_renderActive.load(std::memory_order_acquire));
}

void RenderPlayersOnlyGreen_OnLevelInit() {
	// Material handles point into the level's resource system. Drop the handle before the old map
	// is torn down; the next main-thread pump will blocking-resolve the new level's material.
	RestoreSceneCvars();
	SceneSystem_SetPlayersOnlyGreenSkyOverride(false);
	// Level teardown: the suppressed-material pointers die with the level. Clear WITHOUT
	// restoring -- writing into freed material memory is the crash, not the leak.
	g_suppressed.clear();
	{
		std::lock_guard<std::mutex> lock(g_suppressMutex);
		g_suppressPending.clear();
		g_suppressedWantsChroma.clear();
	}
	SceneSystem_RestorePlayersOnlyGreenMaterials();
	g_renderActive.store(false, std::memory_order_release);
	g_renderOrdinal.store(-1, std::memory_order_release);
	g_material.store(nullptr, std::memory_order_release);
	g_skipMaterial.store(nullptr, std::memory_order_release);
	g_invisibleMaterial.store(nullptr, std::memory_order_release);
	SceneSystem_SetPlayersOnlyGreenPolicy(false);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen", "level init: pass/material state reset");
}

void RenderPlayersOnlyGreen_SetArmed(bool armed, const char* reason) {
	const bool was = g_armed.load(std::memory_order_relaxed);
	if (was == armed) return;
	if (armed) {
		ResetCounters();
		// A live green preview is allowed to prepare synchronously on the main thread. The render
		// job calls PrepareMaterial before it registers the dedicated stream as well.
		RenderPlayersOnlyGreen_PrepareMaterial();
	}
	std::strncpy(g_reason, reason ? reason : "", sizeof(g_reason) - 1);
	g_reason[sizeof(g_reason) - 1] = '\0';
	g_armed.store(armed, std::memory_order_release);
	SceneSystem_SetPlayersOnlyGreenPolicy(armed || g_renderActive.load(std::memory_order_acquire));
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.playersonlygreen", "%s reason='%s'",
			armed ? "ARM live" : "DISARM live", g_reason);
}

bool RenderPlayersOnlyGreen_IsArmed() {
	return g_armed.load(std::memory_order_acquire);
}

bool RenderPlayersOnlyGreen_Active() {
	return g_armed.load(std::memory_order_acquire) || RenderPlayersOnlyGreen_RenderActive();
}

void RenderPlayersOnlyGreen_SetRenderPassActive(bool active, int passOrdinal, bool includeSmoke) {
	if (active) {
		const bool wasActive = g_renderActive.load(std::memory_order_acquire);
		const int wasOrdinal = g_renderOrdinal.load(std::memory_order_relaxed);
		const bool wasSmoke = g_includeSmoke.load(std::memory_order_relaxed);
		if (wasActive && wasOrdinal == passOrdinal && wasSmoke == includeSmoke) {
			// EngineThread_BeginFrame primes the policy before SceneSystem::FrameUpdate; the
			// reliable render-thread callback then arrives at the actual pass boundary. Both are
			// intentionally idempotent so the diagnostic channel reports one arm per pass.
			SceneSystem_SetPlayersOnlyGreenPolicy(true);
			return;
		}
		SceneSystem_BeginPlayersOnlyGreenPass();
		g_includeSmoke.store(includeSmoke, std::memory_order_release);
		g_renderOrdinal.store(passOrdinal, std::memory_order_release);
		if (!g_material.load(std::memory_order_acquire))
			// This should already have run during PreparingOutput/SavingState. Keeping the guard here
			// makes a failed job degrade to the normal material rather than a synchronous render load.
			return;
		SceneSystem_SetPlayersOnlyGreenPolicy(true);
	}
	g_renderActive.store(active, std::memory_order_release);
	if (!active) {
		// The reliable end-of-pass callback runs after the scene command lists have completed. Flush
		// deferred material substitutions before the next pass can reuse their descriptor storage.
		SceneSystem_RestorePlayersOnlyGreenMaterials();
		SceneSystem_SetPlayersOnlyGreenPolicy(g_armed.load(std::memory_order_acquire));
	}
	if (MvmDebugLog_Active()) {
		if (active) {
			MvmDebugLog_LinefAlways("render.playersonlygreen",
				"PASS ARM ordinal=%d includeSmoke=%d admitted=%llu replaced=%llu unknown=%llu",
				passOrdinal, includeSmoke ? 1 : 0,
				(unsigned long long)g_admitted.load(std::memory_order_relaxed),
				(unsigned long long)g_replaced.load(std::memory_order_relaxed),
				(unsigned long long)g_unknown.load(std::memory_order_relaxed));
		} else {
			MvmDebugLog_LinefAlways("render.playersonlygreen",
				"PASS DISARM ordinal=%d admitted=%llu replaced=%llu effects=%llu unknown=%llu smoke=%d",
				g_renderOrdinal.load(std::memory_order_relaxed),
				(unsigned long long)g_admitted.load(std::memory_order_relaxed),
				(unsigned long long)g_replaced.load(std::memory_order_relaxed),
				(unsigned long long)g_effects.load(std::memory_order_relaxed),
				(unsigned long long)g_unknown.load(std::memory_order_relaxed),
				g_includeSmoke.load(std::memory_order_relaxed) ? 1 : 0);
		}
	}
}

bool RenderPlayersOnlyGreen_RenderActive() {
	// No recorded render repaints anything any more: a job's Players Only is masked by depth
	// (RenderPassPlan.h), so the chroma repaint belongs to the live workspace view alone.
	if (AfxStreams_CurrentPassRenderKey() >= 0) return false;
	return g_renderActive.load(std::memory_order_acquire);
}

bool RenderPlayersOnlyGreen_ShaderNameIsSkipped(const char* name) {
	if (!name || !*name) return false;
	// Pipelines that only ever draw surface dressing / screen-space darkening, never structure:
	//   foliage        - grass tufts, branch/billboard cards (csgo_foliage.vfx)
	//   static_overlay - map-placed floor/wall overlay decals (grime, scraps, posters); these
	//                    bypass the decal cvars AND the scene-batch colour writes
	//   aoproxy        - ambient-occlusion proxy splats; they multiplicatively darken the
	//                    framebuffer, which reads as shadow blobs on the emission-only plate
	static const char* const kBuiltins[] = { "foliage", "static_overlay", "aoproxy" };
	for (const char* const needle : kBuiltins)
		if (ContainsNoCase(name, needle)) return true;
	std::lock_guard<std::mutex> lock(g_skipShaderMutex);
	for (const std::string& needle : g_skipShaderNeedles)
		if (ContainsNoCase(name, needle.c_str())) return true;
	return false;
}

unsigned RenderPlayersOnlyGreen_SkipShaderGeneration() {
	return g_skipShaderGeneration.load(std::memory_order_acquire);
}

bool RenderPlayersOnlyGreen_IndirectSkipEnabled() {
	return g_indirectSkip.load(std::memory_order_acquire);
}

void RenderPlayersOnlyGreen_SetViewmodelPlayerSide(bool playerSide) {
	g_viewmodelPlayerSide.store(playerSide, std::memory_order_release);
}

bool RenderPlayersOnlyGreen_ViewmodelIsPlayerSide() {
	return g_viewmodelPlayerSide.load(std::memory_order_acquire);
}

CMaterial2* RenderPlayersOnlyGreen_SkipMaterial() {
	return g_skipMaterial.load(std::memory_order_acquire);
}

CMaterial2* RenderPlayersOnlyGreen_Material() {
	return g_material.load(std::memory_order_acquire);
}

CMaterial2* RenderPlayersOnlyGreen_InvisibleMaterial() {
	// Retired experiment kept as a guard value: material stand-ins for skip-class elements were
	// measured to render the substitute's own texture (chroma -> black splats, toolsinvisible ->
	// "INVISIBLE" dev plates). Skip-class elements now keep their original material and are
	// neutralised by the green colour modulate; the restore paths still compare against this in
	// case an older pending entry survives an arm/disarm boundary.
	return g_invisibleMaterial.load(std::memory_order_acquire);
}

bool RenderPlayersOnlyGreen_IsEffectMaterial(const char* materialName) {
	// Decal/blood atlases are not gameplay smoke. Keep them out of the effect allow-list so the
	// scene hook can reject their translucent batches instead of feeding them the opaque chroma
	// material (which is the source of the black splats visible on an otherwise flat plate).
	if (RenderPlayersOnlyGreen_IsDecalMaterial(materialName)) return false;
	return ContainsNoCase(materialName, "smoke")
		|| ContainsNoCase(materialName, "particle")
		|| ContainsNoCase(materialName, "volumetric")
		|| ContainsNoCase(materialName, "effects/")
		|| ContainsNoCase(materialName, "vfx/");
}

bool RenderPlayersOnlyGreen_IsDecalMaterial(const char* materialName) {
	if (!materialName || !*materialName) return false;
	return ContainsNoCase(materialName, "decals/")
		|| ContainsNoCase(materialName, "/decal")
		|| ContainsNoCase(materialName, "_decal")
		|| ContainsNoCase(materialName, "decal_")
		|| ContainsNoCase(materialName, "blood")
		|| ContainsNoCase(materialName, "gore")
		|| ContainsNoCase(materialName, "splatter")
		|| ContainsNoCase(materialName, "bullet_hole")
		|| ContainsNoCase(materialName, "bullet-hole");
}

bool RenderPlayersOnlyGreen_IsDetailOverlayMaterial(const char* materialName) {
	if (!materialName || !*materialName) return false;
	// Evidence-based (mvm_debug render.playersonlygreen.draw, de_cache 2026-08-20): grass tufts
	// are models/de_cache_s2/foliage/ch2_detail_foliage_*, ground rubbish is
	// materials/de_cache/debris/* and models/.../debris/debris_kit_*. `/debris/` is deliberately
	// a PATH segment so blend materials like trim/ch2_trim_wood_painted_debris_01a (real wall
	// geometry) stay on the repaint path. Widen only from new probe evidence.
	// Foliage sprite cards (branches, billboards, leaves) share the same alpha-tested pipeline
	// as the tufts; trunks and bark deliberately stay OUT of this list so trees keep occluding
	// players with chroma-painted geometry.
	return ContainsNoCase(materialName, "detail_foliage")
		|| ContainsNoCase(materialName, "detailsprite")
		|| ContainsNoCase(materialName, "/debris/")
		|| ContainsNoCase(materialName, "/grass/")
		|| ContainsNoCase(materialName, "billboard")
		|| ContainsNoCase(materialName, "_branch")
		|| ContainsNoCase(materialName, "branch_")
		|| ContainsNoCase(materialName, "leaves");
}

bool RenderPlayersOnlyGreen_IsSkippedSceneMaterial(const char* materialName) {
	return RenderPlayersOnlyGreen_IsDecalMaterial(materialName)
		|| RenderPlayersOnlyGreen_IsDetailOverlayMaterial(materialName);
}

PlayersOnlyGreenDomain RenderPlayersOnlyGreen_Classify(const char* materialName) {
	if (!materialName || !*materialName) return PlayersOnlyGreenDomain::Unknown;
	if (RenderPlayersOnlyGreen_IsEffectMaterial(materialName)) return PlayersOnlyGreenDomain::Effect;
	if (StartsWith(materialName, "characters/models/shared/arms/"))
		// The same shared arm/glove assets are used by third-person player meshes. Only explicit
		// first-person variants are viewmodel-owned; the ordinary shared path is player-owned and
		// must retain its normal material/lighting in an isolation plate.
		return IsFirstPersonMaterial(materialName)
			? PlayersOnlyGreenDomain::Viewmodel : PlayersOnlyGreenDomain::PlayerEquipment;
	if (StartsWith(materialName, "characters/")) return PlayersOnlyGreenDomain::Players;
	// The root `weapons/` namespace contains player-side econ models, gloves, stickers and
	// attached equipment. World-model weapons use the reversed `models/weapons/` spelling and are
	// deliberately kept out of the subject plate unless an engine attachment path identifies them.
	if (StartsWith(materialName, "weapons/") || StartsWith(materialName, "compmat_"))
		return PlayersOnlyGreenDomain::PlayerEquipment;
	// Defuse kits are player-owned equipment, but their world-model materials use the reversed
	// `materials/models/weapons/w_models/...` spelling. Keep the kit attached to a player instead
	// of painting it chroma green along with map/world weapon materials.
	if (ContainsNoCase(materialName, "w_models/w_eq_defuser/"))
		return PlayersOnlyGreenDomain::PlayerEquipment;
	if (ContainsNoCase(materialName, "models/weapons/")) return PlayersOnlyGreenDomain::World;
	if (ContainsNoCase(materialName, "models/props/")) return PlayersOnlyGreenDomain::World;
	if (StartsWith(materialName, "materials/") || StartsWith(materialName, "models/"))
		return PlayersOnlyGreenDomain::World;
	return PlayersOnlyGreenDomain::Unknown;
}

bool RenderPlayersOnlyGreen_Admits(PlayersOnlyGreenDomain domain) {
	switch (domain) {
	case PlayersOnlyGreenDomain::Players:
	case PlayersOnlyGreenDomain::PlayerEquipment:
		return true;
	case PlayersOnlyGreenDomain::Viewmodel:
		return RenderPlayersOnlyGreen_ViewmodelIsPlayerSide();
	case PlayersOnlyGreenDomain::Effect:
		// Effects are not material-painted; the normal smoke/volumetric path owns their pixels.
		return true;
	default:
		return false;
	}
}

void RenderPlayersOnlyGreen_NoteDraw(PlayersOnlyGreenDomain domain, bool replaced) {
	if (domain == PlayersOnlyGreenDomain::Effect)
		g_effects.fetch_add(1, std::memory_order_relaxed);
	else if (RenderPlayersOnlyGreen_Admits(domain))
		g_admitted.fetch_add(1, std::memory_order_relaxed);
	if (replaced) g_replaced.fetch_add(1, std::memory_order_relaxed);
}

void RenderPlayersOnlyGreen_NoteUnknown(const char* materialName) {
	g_unknown.fetch_add(1, std::memory_order_relaxed);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("render.playersonlygreen.unknown", "material='%s' replaced=1",
			materialName && *materialName ? materialName : "<null>");
}

void RenderPlayersOnlyGreen_NoteBatchSkipped(const char* source, int materialCount,
	const char* firstMaterialName) {
	const auto skipped = g_decalsSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
	if (MvmDebugLog_Active() && skipped <= 64)
		MvmDebugLog_Linef("render.playersonlygreen.skip",
			"via=%s materials=%d first='%s' total=%llu",
			source ? source : "?", materialCount,
			firstMaterialName && *firstMaterialName ? firstMaterialName : "<none>",
			(unsigned long long)skipped);
}

void RenderPlayersOnlyGreen_KeyColor(float outRgba[4]) {
	if (!outRgba) return;
	outRgba[0] = 0.0f; outRgba[1] = 1.0f; outRgba[2] = 0.0f; outRgba[3] = 1.0f;
}

void RenderPlayersOnlyGreen_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4 && args) ? args->ArgV(3) : "status";
	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "1")) {
		RenderPlayersOnlyGreen_SetArmed(true, "console");
		advancedfx::Message("%s render playersonlygreen: ON -- non-player scene draws are chroma green;"
			" world depth and gameplay smoke remain active.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "0")) {
		RenderPlayersOnlyGreen_SetArmed(false, "console");
		advancedfx::Message("%s render playersonlygreen: off.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("%s render playersonlygreen: %s (renderPass=%d ordinal=%d smoke=%d)\n",
			cmd, RenderPlayersOnlyGreen_IsArmed() ? "ON" : "off",
			RenderPlayersOnlyGreen_RenderActive() ? 1 : 0,
			g_renderOrdinal.load(std::memory_order_relaxed),
			g_includeSmoke.load(std::memory_order_relaxed) ? 1 : 0);
		return;
	}
	if (0 == _stricmp(sub, "indirect")) {
		if (argc >= 5) {
			g_indirectSkip.store(0 != atoi(args->ArgV(4)), std::memory_order_release);
		}
		advancedfx::Message("%s: indirect (GPU-driven scatter: grass tufts, ground litter)"
			" skip is %s.\n", cmd,
			g_indirectSkip.load(std::memory_order_acquire) ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(sub, "skipshader")) {
		const char* op = (argc >= 5) ? args->ArgV(4) : "list";
		if (0 == _stricmp(op, "add") && argc >= 6) {
			{
				std::lock_guard<std::mutex> lock(g_skipShaderMutex);
				g_skipShaderNeedles.emplace_back(args->ArgV(5));
			}
			g_skipShaderGeneration.fetch_add(1, std::memory_order_release);
			advancedfx::Message("%s: skipshader added '%s' (pipelines whose shader name contains it"
				" stop drawing while the green pass is armed).\n", cmd, args->ArgV(5));
			return;
		}
		if (0 == _stricmp(op, "clear")) {
			{
				std::lock_guard<std::mutex> lock(g_skipShaderMutex);
				g_skipShaderNeedles.clear();
			}
			g_skipShaderGeneration.fetch_add(1, std::memory_order_release);
			advancedfx::Message("%s: skipshader dynamic list cleared (builtins stay).\n", cmd);
			return;
		}
		std::lock_guard<std::mutex> lock(g_skipShaderMutex);
		advancedfx::Message("%s: skipshader builtins: foliage, static_overlay, aoproxy; dynamic (%d):\n",
			cmd, (int)g_skipShaderNeedles.size());
		for (const std::string& needle : g_skipShaderNeedles)
			advancedfx::Message("  '%s'\n", needle.c_str());
		advancedfx::Message("usage: %s render playersonlygreen skipshader add <substr> | clear | list\n", cmd);
		return;
	}
	advancedfx::Message("%s render playersonlygreen on|off|status|skipshader|indirect (aliases: playersgreen, greenscreen)\n", cmd);
}

} // namespace Filmmaker
