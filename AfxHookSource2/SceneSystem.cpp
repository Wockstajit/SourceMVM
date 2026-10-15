#include "Filmmaker/World/WorldTransient.h"
#include "SceneSystem.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "SchemaSystem.h"
#include "MirvColors.h"
#include "Filmmaker/Debug/MvmDebugLog.h"
#include "Filmmaker/Movie/RagdollWeapon.h"
#include "Filmmaker/World/LightFx.h"
#include "Filmmaker/World/SunFx.h"
#include "Filmmaker/World/WorldModulation.h" // geometry tint, applied in the draw hook below // scene lighting colour; hook attached from HookSceneSystem
#include "Filmmaker/World/ShadowStageProbe.h"
#include "Filmmaker/World/WorldEntities.h"
#include "Filmmaker/World/WorldSchema.h"
#include "Filmmaker/Render/RenderWorldOnly.h" // World Only pass: subject rejection policy
#include "Filmmaker/Render/RenderPlayersOnlyGreen.h" // Players Only / Green material policy
#include "Filmmaker/Movie/ParticleFx.h" // blood suppression (the UwU easter egg): skip blood decal draws

#include "../Shared/StringTools.h"
#include "../Deps/release/Detours/src/detours.h"
#include "../Deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.h"
#include <unordered_map>
#include <map>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "StreamSettings.h"
#include "../Deps/release/prop/cs2/sdk_src/public/tier0/memalloc.h"
#include <set>
#include <queue>
#include <shared_mutex>

typedef void* (__fastcall * FindMaterial_t)(void* This, CMaterial2*** out, const char* materialName);
FindMaterial_t org_FindMaterial = nullptr;

typedef u_char* (__fastcall * MaterialFindParam_t)(CMaterial2* This, const char * paramName);
MaterialFindParam_t org_MaterialFindParam = nullptr;

typedef void (__fastcall * MaterialUpdate_t)(CMaterial2* This);
MaterialUpdate_t org_MaterialUpdate = nullptr;

CResourceSystem* g_pCResourceSystem = nullptr;

struct CBufferStringWrapper {
	SOURCESDK::CS2::CBufferString buf;
	u_char pad[0xE0 - sizeof(SOURCESDK::CS2::CBufferString)];
};

typedef void* (__fastcall * ForceUpdateSkybox_t)(void* This);
ForceUpdateSkybox_t org_ForceUpdateSkybox = nullptr;

uint32_t g_Skybox_UnkPtr_Offset;

struct CustomSkyState {
	std::string currentSkyName = "";

	MyColor color;
	MyColor colorClouds;
	bool drawClouds = true;
	bool drawSun = true;
	float brightness = 1.0f;
} g_CustomSky;

// Draw jobs on several render workers read and grow this while console commands prune it.
std::mutex g_PickerMutex;
std::map<void *, size_t> g_PickerEntities;

// Written under g_PickerMutex; atomic so the draw path can test them without the lock.
std::atomic_bool g_PickerActive = false;
std::atomic_bool g_PickerCollecting = false;
std::atomic_bool g_PickerPrint = false;

void Picker_Pick(bool wasVisible)
{
	std::lock_guard<std::mutex> lock(g_PickerMutex);
	if (!g_PickerActive)
	{
		g_PickerActive = true;
		g_PickerCollecting = true;
		g_PickerPrint = true;
	}
	else
	{
		if (g_PickerCollecting)
			g_PickerCollecting = false;

		size_t index = 0;

		for (auto it = g_PickerEntities.begin(); it != g_PickerEntities.end(); )
		{
			size_t oldIndex = it->second;

			if ((1 == (oldIndex & 0x1)) == wasVisible)
			{
				it = g_PickerEntities.erase(it);
			}
			else
			{
				it->second = index;
				++index;
				++it;
			}
		}
	}

	g_PickerPrint = true;
}

static void Picker_StopLocked(void)
{
	if(g_PickerActive)
	{
		g_PickerEntities.clear();
		g_PickerActive = false;
		advancedfx::Message("==== Picker stopped. ====\n");
	}
}

void Picker_Stop(void)
{
	std::lock_guard<std::mutex> lock(g_PickerMutex);
	Picker_StopLocked();
}

void PickerPrintReset(){
	if(g_PickerPrint) {
		std::lock_guard<std::mutex> lock(g_PickerMutex);
		g_PickerPrint = false;

		bool determinedEntities = g_PickerEntities.size() <= 1;
		if (determinedEntities) Picker_StopLocked();
	}
}

// 64-bit hash for 32-bit platforms
// Credit
// https://www.ncbi.nlm.nih.gov/IEB/ToolBox/CPP_DOC/lxr/source/src/util/checksum/murmurhash/MurmurHash2.cxx#0140
uint64_t MurmurHash64B ( const void * key, int len, uint64_t seed )
{
  const uint32_t m = 0x5bd1e995;
  const int r = 24;

  uint32_t h1 = uint32_t(seed) ^ len;
  uint32_t h2 = uint32_t(seed >> 32);

  const uint32_t * data = (const uint32_t *)key;

  while(len >= 8)
  {
    uint32_t k1 = *data++;
    k1 *= m; k1 ^= k1 >> r; k1 *= m;
    h1 *= m; h1 ^= k1;
    len -= 4;

    uint32_t k2 = *data++;
    k2 *= m; k2 ^= k2 >> r; k2 *= m;
    h2 *= m; h2 ^= k2;
    len -= 4;
  }

  if(len >= 4)
  {
    uint32_t k1 = *data++;
    k1 *= m; k1 ^= k1 >> r; k1 *= m;
    h1 *= m; h1 ^= k1;
    len -= 4;
  }

  switch(len)
  {
  case 3: h2 ^= ((unsigned char*)data)[2] << 16;
  case 2: h2 ^= ((unsigned char*)data)[1] << 8;
  case 1: h2 ^= ((unsigned char*)data)[0];
      h2 *= m;
  };

  h1 ^= h2 >> 18; h1 *= m;
  h2 ^= h1 >> 22; h2 *= m;
  h1 ^= h2 >> 17; h1 *= m;
  h2 ^= h1 >> 19; h2 *= m;

  uint64_t h = h1;

  h = (h << 32) | h2;

  return h;
}

void CResourceSystem::GetMaterials(GetMaterialsArrayResult* out) {
	// In materialsystem2.dll see below in func with 
	// "NOTE: Use mat_print_materials instead, and then click on the hyperlink for a given material. That will call this concommand with the correctly formatted argument.\n"
	// (**(code **)(*ResourceSystem + 0x128))(ResourceSystem,0x74616d76,local_198, 7);
	typedef void (__fastcall * GetMaterials_t)(void* This, uint64_t magic, GetMaterialsArrayResult* out, uint8_t unk);
	void** vtable = *(void***)(this);
	auto org_GetMaterials = (GetMaterials_t)vtable[32];

	org_GetMaterials(this, 0x74616d76, out, 7);
}

CMaterial2** CResourceSystem::PreCache (const char* name) {
	// resourcesystem.dll
	// "Resource \"%s\" was explicitly loaded with a blocking load.\n"
	// There are 2 precache functions in vtable next to each other, this is first one
	typedef CMaterial2** (__fastcall * PreCacheFn_t)(void* This, CBufferStringWrapper* name, const char* unk);
	void** vtable = *(void***)(this);
	auto org_PreCache = (PreCacheFn_t)vtable[40];

	// client dll
	// see where func is called with "FixupResourceName: Illegal full path passed in (\"%s\")!\n" 
	CBufferStringWrapper wrapper { name };
	wrapper.buf.FixupPathName(0x5C); // back slash
	wrapper.buf.ToLowerFast(0);
	wrapper.buf.FixSlashes(0x2F); // forward slash

	SOURCESDK::CS2::CBufferString extension;
	extension.ExtractFileExtension(wrapper.buf.Get());
	// in case its ever updated see where above mentioned is called, there is function that calculates it
	auto hash = MurmurHash64B(wrapper.buf.Get(), wrapper.buf.Length(), 0xEDABCDEF);

	auto pWrapper = (u_char*)&wrapper;
	memcpy(pWrapper + 0xD0, &hash, 8);
	memcpy(pWrapper + 0xD8, extension.Get(), 8);

	return org_PreCache(this, &wrapper, "");
}

struct SkyOriginal {
    std::string material;
    uint32_t tint = 0;
    float brightness = 1;
    bool materialTaken = false, colorTaken = false;
};
// Entity address AND complete serial-bearing handle, cleared before each new level.
std::map<std::pair<void*, int>, SkyOriginal> g_skyOriginals;
std::mutex g_skyOriginalsMutex;
std::unordered_map<std::string, bool> cachedMaterials = {};
std::atomic<bool> g_playersOnlyGreenPolicy{false};
// Sky/cloud draws are not scene-object material batches, so the green policy needs one narrow
// render-thread bit in addition to its per-element material substitution. It is deliberately
// separate from g_CustomSky.drawClouds: that remains the user's persistent sky preference and is
// never overwritten by a pass.
std::atomic<bool> g_playersOnlyGreenCloudsSuppressed{false};
// `env_sky` is not submitted through the ordinary scene-object material batches. While the green
// pass is active its dome material is OVERRIDDEN with the solid-green sky material instead of the
// sky being disabled: a disabled sky leaves the scene target's black clear in every sky pixel
// (measured 2026-08-20 -- the pass's "black sky"), while a green sky.vfx dome paints the backdrop
// through the engine's own sky path. The override rides the existing ForceUpdateSkybox detour, so
// engine-initiated sky rebuilds (map load, round restart) re-apply it, and the user's own
// mirv_sky choice in g_CustomSky is never overwritten -- it comes back the moment the pass ends.
// The green sky ships PREBUILT (FX/render-materials/prebuilt): it is the native sky_black.vmat_c
// byte-patched `_black`->`_green` with its 4x4 DXT1 cube repainted solid green. That same-length
// rename is why the material lives under materials/skybox/ instead of materials/filmmaker/ -- the
// compiled KV3 blocks are LZ4-compressed, so only equal-length string patches are possible.
// Regenerate after a CS2 update with FX/tools/build-green-sky.py.
bool g_playersOnlyGreenSkyOverride = false;
constexpr const char* kPlayersOnlyGreenSkyMaterial = "materials/skybox/sky_green.vmat";
std::mutex g_playersOnlyGreenRestoreMutex;
std::unordered_map<void*, CMaterial2*> g_playersOnlyGreenPendingRestores;
std::unordered_map<void*, unsigned int> g_playersOnlyGreenPendingColorRestores;


void resetCachedMaterials () {
	{ std::lock_guard<std::mutex> lock(g_skyOriginalsMutex); g_skyOriginals.clear(); g_CustomSky.currentSkyName.clear(); }
	cachedMaterials.clear();

}

// The sky material the dome should currently show: the green-pass override wins while active,
// otherwise the user's mirv_sky choice, otherwise empty (= the map's own sky).
std::string EffectiveSkyMaterialName() {
 const auto transient = Filmmaker::WorldTransient::Read();
	static const std::string s_greenSky = kPlayersOnlyGreenSkyMaterial;
	return g_playersOnlyGreenSkyOverride ? s_greenSky : transient->hasSky ? transient->skyMaterial : g_CustomSky.currentSkyName;
}

void* new_ForceUpdateSkybox(void* This) {
    {
    // Worker jobs can update skies during loading. Serialize snapshot ownership, and
    // release the lock before calling back into the engine.
    std::lock_guard<std::mutex> lock(g_skyOriginalsMutex);
    const auto key = std::make_pair(This, ((CEntityInstance*)This)->GetHandle().ToInt());
    const std::string& desired = EffectiveSkyMaterialName();
    auto& original = g_skyOriginals[key];
    auto materialSlot = (CMaterial2***)((u_char*)This + g_clientDllOffsets.C_EnvSky.m_hSkyMaterial);
    auto tint = (uint32_t*)((u_char*)This + g_clientDllOffsets.C_EnvSky.m_vTintColor);
    auto brightness = (float*)((u_char*)This + g_clientDllOffsets.C_EnvSky.m_flBrightnessScale);
    if (!desired.empty()) {
        if (!original.materialTaken && *materialSlot && **materialSlot) {
            original.material = (**materialSlot)->GetName(); original.materialTaken = true;
            Filmmaker::MvmDebugLog_Linef("environment.baseline", "sky capture ent=%p handle=%x offset=%zx material=%s", This, key.second,
                (size_t)g_clientDllOffsets.C_EnvSky.m_hSkyMaterial, original.material.c_str());
        }
        CMaterial2** material = nullptr; org_FindMaterial(nullptr, &material, desired.c_str());
        if (material && *materialSlot != material) {
            ++*(uint32_t*)((u_char*)material + 0x20); *materialSlot = material;
        }
    } else if (original.materialTaken) {
        CMaterial2** material = nullptr; org_FindMaterial(nullptr, &material, original.material.c_str());
        if (material) {
            *materialSlot = material; original.materialTaken = false;
            Filmmaker::MvmDebugLog_Linef("environment.baseline", "sky restore ent=%p handle=%x material=%s", This, key.second, original.material.c_str());
        }
    }
    if (Filmmaker::WorldTransient::Number("skyTint", g_CustomSky.color.use)) {
        if (!original.colorTaken) {
            original.tint = *tint; original.brightness = *brightness; original.colorTaken = true;
            Filmmaker::MvmDebugLog_Linef("environment.baseline", "sky color capture ent=%p handle=%x offsets=%zx/%zx tint=%08x brightness=%.6f", This, key.second,
                (size_t)g_clientDllOffsets.C_EnvSky.m_vTintColor, (size_t)g_clientDllOffsets.C_EnvSky.m_flBrightnessScale, original.tint, original.brightness);
        }
        auto color = g_CustomSky.color.value;
        color.r = (unsigned char)Filmmaker::WorldTransient::Number("skyColor",color.r,0);
        color.g = (unsigned char)Filmmaker::WorldTransient::Number("skyColor",color.g,1);
        color.b = (unsigned char)Filmmaker::WorldTransient::Number("skyColor",color.b,2);
        *tint = afxUtils::rgbaToHex(color); *brightness = float(Filmmaker::WorldTransient::Number("skyColor",g_CustomSky.brightness*255,3)/255);
    } else if (original.colorTaken) {
        *tint = original.tint; *brightness = original.brightness; original.colorTaken = false;
        Filmmaker::MvmDebugLog_Linef("environment.baseline", "sky color restore ent=%p handle=%x tint=%08x brightness=%.6f", This, key.second, *tint, *brightness);
    }
    if (!original.materialTaken && !original.colorTaken) g_skyOriginals.erase(key);
    }
    return org_ForceUpdateSkybox(This);
}

void updateSkyboxEntities() {
	const std::string effectiveSkyName = EffectiveSkyMaterialName();
	if (effectiveSkyName.size() != 0) {
		if (cachedMaterials.find(effectiveSkyName) == cachedMaterials.end()) {
			if (0 != g_pCResourceSystem->PreCache(effectiveSkyName.c_str())) {
				cachedMaterials.insert({effectiveSkyName, true});
			}
		}
	}

	int highestIndex = GetHighestEntityIndex();
	for(int i = 0; i < highestIndex + 1; i++) {
		if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i)) {
			// Note: there are multiple skybox entities.
			// Seems like the other one is used for fog or smth.
			//
			// MATCH BOTH CLASS NAMES. Measured 2026-08-04 on Mirage via mirv_listentities, the
			// sky entity reports:
			//     debugName "mirage_sky" / className "env_sky" / clientClassName "C_BaseModelEntity"
			// so testing ONLY GetClientClassName()=="C_EnvSky" matched nothing at all and
			// mirv_sky silently did nothing -- the whole skybox changer was dead. Verified by
			// before/after screenshots being byte-identical after a material swap.
			// The client-class spelling is kept in case a future build populates the leaf class.
			const char* entClassName = ent->GetClassName();
			const char* entClientClassName = ent->GetClientClassName();
			const bool isSky =
				(entClassName && 0 == _stricmp("env_sky", entClassName)) ||
				(entClientClassName && 0 == _stricmp("C_EnvSky", entClientClassName));
			if (!isSky) continue;

			// we have to remove pointer to object, so it can update
			// see dissasembly for the update function
			*(void**)((u_char*)ent + g_Skybox_UnkPtr_Offset) = nullptr;
			new_ForceUpdateSkybox(ent);
		}
	}

}

size_t g_SceneLayer_pszViewPass_Offset = -1;
size_t g_SceneLayer_Flags_Offset = -1;

// SIZE IS 0x70, NOT the 0x68 the 2024 reference records.
//
// MEASURED 2026-08-04 with `mirv_filmmaker world modulate stride`: for ten multi-element batches,
// element 1's sceneObject (+0x18) and material (+0x20) read as plausible pointers ONLY at a 0x70
// step. Every one of the ten agreed; 0x60/0x68/0x90 each hit on one or two samples by chance.
//
// The struct grew, which fits the colour field having moved 0x40 -> 0x50 in a CS2 update. Keeping
// the stale 0x68 meant sceneData[i] for any i > 0 landed mid-object -- that is what crashed CS2
// the moment world modulation tried to iterate a batch.
struct CBaseSceneData {
	char _pad0[0x18];
	void* sceneObject;    // 0x18
	CMaterial2* material; // 0x20
	char _pad1[0x28];
	uint32_t color;       // 0x50
	char _pad2[0x1C];     // -> 0x70
};

namespace {

bool ProbeReadPointerAt(void* base, size_t offset, void** out) {
	if (!base || !out) return false;
	__try {
		*out = *(void**)((unsigned char*)base + offset);
		return true;
	} __except (1) {
		*out = nullptr;
		return false;
	}
}

bool ProbeWritePointerAt(void* base, size_t offset, void* value) {
	if (!base) return false;
	__try {
		*(void**)((unsigned char*)base + offset) = value;
		return true;
	} __except (1) {
		return false;
	}
}

const char* ProbeReadMaterialName(CMaterial2* material) {
	if (!material) return "<null>";
	__try {
		const char* value = material->GetName();
		return value ? value : "?";
	} __except (1) {
		return "<fault>";
	}
}

} // namespace

typedef void (__fastcall * DrawBaseSceneObject_t)(void* This, void* a2, CBaseSceneData* scene_data, int a4, int a5, void* a6, void* a7);

struct DrawDescriptorHook {
	const char* rtti;
	void** vtable = nullptr;
	DrawBaseSceneObject_t original = nullptr;
	size_t owner = 0;
	bool attach = false;
};

// EVERY descriptor scenesystem.dll can submit a drawable batch through. A subject filter is only
// as good as its coverage: a draw that reaches D3D through a descriptor nobody detoured cannot be
// rejected by any filter, and it repaints the isolation plate with the very geometry the pass was
// asked to remove. Measured 2026-08-09 on scoped Mirage -- `filter=2` (PlayersOnly) was armed
// before the pass's keyed clear and the world still drew over it, because only the first two
// entries below were hooked. The leaking shaders named the missing owners directly:
// csgo_foliage -> Clutter, csgo_vertexlitgeneric -> InstancedMesh, aoproxy_splat -> AO proxy.
//
// This is the coverage half of the IWXMVM approach: establish pass-local state, then make sure
// EVERY submission path actually observes it.
//
// MEASURED FAILURE 2026-08-09 -- do not simply re-add the list below without doing the vtable work
// first. An RTTI scan of scenesystem.dll finds these further descriptor classes:
//
//   CSceneObjectDesc, CClutterSceneObjectDesc, CInstancedMeshSceneObjectDesc,
//   CMeshBuilderSceneObjectDesc, CProjectedDecalSceneObjectDesc,
//   CDistanceFieldInstanceSceneObjectDesc, CAggregateRTProxySceneObjectDesc,
//   CAmbientOcclusionProxySceneObjectDesc
//
// Adding all eight to this table detours vtable slot 1 of each, and CS2 died on startup with an
// access violation (`cs2_2026_0809_142517_0_accessviolation`) before it ever showed a window.
// "Slot 1 is the batch draw with THIS signature" was only ever verified for the two entries that
// remain; for an abstract base or an interface that slot can be a pure-call thunk or an unrelated
// virtual, and detouring it corrupts the call. The next attempt must confirm each candidate's slot
// 1 really is the drawable-batch function (the durable Ghidra workspace in
// Docs/agent/render-system-contract.md has scenesystem.dll analyzed; ScanVtable.java dumps a
// vtable) and hook only the confirmed ones.
// RETESTED 2026-08-10, ALONE, and it still crashes -- `.?AVCSceneObjectDesc@@` (slot 1 =
// scenesystem+0x72720, reported unhooked by `mirv_filmmaker render worldonly coverage`) was added
// by itself, with the batch's material pointer read under SEH, to chase a dropped weapon that
// survived a World Only plate. CS2 died during startup again (dump cs2.exe.210764.dmp, and the
// guarded read did not catch it, so the fault is not the batch layout but the call itself).
//
// So the eight-at-once crash was not "too many at once": slot 1 of these classes is simply not the
// same drawable-batch function. Confirming the slot means disassembling it (ScanVtable.java /
// DumpAround.java against the durable Ghidra workspace in Docs/agent/render-system-contract.md),
// NOT installing it and seeing whether the game survives. Do not add another entry here without
// that work.
DrawDescriptorHook g_DrawDescriptorHooks[] = {
	{ ".?AVCBaseSceneObjectDesc@@" },
	{ ".?AVCAnimatableSceneObjectDesc@@" },
};

// ---- draw-coverage report (read-only) ---------------------------------------------------------
// EVERY descriptor class scenesystem.dll can submit a drawable batch through, whether or not we
// detour it. A subject filter can only reject draws that reach a detour, so when an object survives
// a filtered plate the first question is "does it even pass through us?". This table answers that
// from live data instead of from the assumption that the two hooked types cover everything.
//
// Recording it costs one RTTI lookup per class at startup and never changes engine state. It is
// explicitly NOT a list of things to detour: hooking all of these at once was measured to kill CS2
// at startup (see g_DrawDescriptorHooks), because slot 1 is only known to be the batch draw for the
// classes that are hooked today.
struct DescCoverage {
	const char* rtti;
	void** vtable = nullptr;
	void* draw = nullptr;   // vtable[1] as read BEFORE any detour replaced it
	bool covered = false;
};

DescCoverage g_DescCoverage[] = {
	{ ".?AVCBaseSceneObjectDesc@@" },
	{ ".?AVCAnimatableSceneObjectDesc@@" },
	{ ".?AVCAggregateSceneObjectDesc@@" },
	{ ".?AVCSceneObjectDesc@@" },
	{ ".?AVCClutterSceneObjectDesc@@" },
	{ ".?AVCInstancedMeshSceneObjectDesc@@" },
	{ ".?AVCMeshBuilderSceneObjectDesc@@" },
	{ ".?AVCProjectedDecalSceneObjectDesc@@" },
	{ ".?AVCDistanceFieldInstanceSceneObjectDesc@@" },
	{ ".?AVCAggregateRTProxySceneObjectDesc@@" },
	{ ".?AVCAmbientOcclusionProxySceneObjectDesc@@" },
};

constexpr size_t kDescCoverageCount = sizeof(g_DescCoverage) / sizeof(g_DescCoverage[0]);

DrawBaseSceneObject_t FindDrawDescriptorOriginal(void* descriptor) {
	void** vtable = nullptr;
	if (!ProbeReadPointerAt(descriptor, 0, (void**)&vtable) || !vtable) return nullptr;
	for (size_t i = 0; i < sizeof(g_DrawDescriptorHooks) / sizeof(g_DrawDescriptorHooks[0]); ++i) {
		if (g_DrawDescriptorHooks[i].vtable == vtable) {
			return g_DrawDescriptorHooks[g_DrawDescriptorHooks[i].owner].original;
		}
	}
	return nullptr;
}

// Short name of the descriptor class a draw arrived through, for the World Only probe log. Which
// submission path drew a thing is the first question when something survives a filtered plate, and
// answering it from the log beats re-deriving it from a coverage table every time.
// Collects the material names of a drawable batch's elements for the World Only filter. The batch
// is an array with the MEASURED 0x70 stride (see CBaseSceneData); `count` is the engine's element
// count. Bounded and SEH-guarded per element: a descriptor whose layout does not match degrades to
// "fewer names", never to a fault in a render worker.
//
// This exists because the hook can only skip a batch as a WHOLE. Reading element 0 alone and
// skipping on that verdict removed world geometry that shared a batch with a subject -- measured as
// a potted plant rendering as a black silhouette (depth written, colour pass skipped).
constexpr int kMaxBatchNames = 8;

int CollectBatchMaterialNames(CBaseSceneData* sceneData, int count, const char* out[kMaxBatchNames]);

// Blood suppression: a batch whose every material is a blood one (CS2's floor/wall splats). Mixed
// batches draw as usual -- never hide a bullet hole or a wall because it shares a batch.
bool IsBloodOnlyBatch(CBaseSceneData* sceneData, int count) {
	if (!Filmmaker::ParticleFx_BloodSuppressed()) return false;
	const char* names[kMaxBatchNames] = {};
	const int n = CollectBatchMaterialNames(sceneData, count, names);
	if (n <= 0) return false;
	for (int i = 0; i < n; ++i) {
		if (!names[i]) return false;
		char low[160];
		int k = 0;
		for (; k < 159 && names[i][k]; ++k) low[k] = (char)tolower((unsigned char)names[i][k]);
		low[k] = 0;
		if (Filmmaker::MvmDebugLog_Active() && (std::strstr(low, "blood") || std::strstr(low, "decal"))) {
			static std::atomic<int> s_logged{ 0 };
			if (s_logged.fetch_add(1) < 40) Filmmaker::MvmDebugLog_LinefAlways("egg.uwu.decal", "material %s", names[i]);
		}
		if (!std::strstr(low, "blood")) return false;
	}
	return true;
}

int CollectBatchMaterialNames(CBaseSceneData* sceneData, int count, const char* out[kMaxBatchNames]) {
	int n = 0;
	if (!sceneData || count <= 0) return 0;
	const int limit = count < kMaxBatchNames ? count : kMaxBatchNames;
	for (int i = 0; i < limit; ++i) {
		CMaterial2* material = nullptr;
		if (!ProbeReadPointerAt(sceneData + i, offsetof(CBaseSceneData, material), (void**)&material))
			break;
		if (!material) continue;
		out[n++] = ProbeReadMaterialName(material);
	}
	return n;
}

// Decals and detail overlays are submitted as their own scene batches on the maps we have
// measured, and both need the batch DROPPED rather than repainted: a decal's translucent atlas
// painted with chroma_unlit shows splats/halos, and detail foliage/debris ignores the per-element
// material write outright (measured de_cache 2026-08-20 -- replaced=1 in the log, original
// texture on screen). We only reject a batch when every element is in the skip vocabulary and the
// bounded layout walk covered the complete batch; mixed batches continue through the normal
// per-element replacement path so a world surface is never culled.
bool IsEntirelyPlayersOnlyGreenSkippedBatch(CBaseSceneData* sceneData, int count,
	const char** outFirstName) {
	if (outFirstName) *outFirstName = nullptr;
	if (!sceneData || count <= 0 || count > 256) return false;
	for (int i = 0; i < count; ++i) {
		CMaterial2* material = nullptr;
		if (!ProbeReadPointerAt(sceneData + i, offsetof(CBaseSceneData, material), (void**)&material)
			|| !material)
			return false;
		const char* name = ProbeReadMaterialName(material);
		if (!Filmmaker::RenderPlayersOnlyGreen_IsSkippedSceneMaterial(name)) return false;
		if (outFirstName && i == 0) *outFirstName = name;
	}
	return true;
}

const char* DrawDescriptorTag(void* descriptor) {
	void** vtable = nullptr;
	if (!ProbeReadPointerAt(descriptor, 0, (void**)&vtable) || !vtable) return "base";
	for (size_t i = 0; i < sizeof(g_DrawDescriptorHooks) / sizeof(g_DrawDescriptorHooks[0]); ++i) {
		if (g_DrawDescriptorHooks[i].vtable != vtable) continue;
		return i == 1 ? "anim" : "base";
	}
	return "base";
}

// A scene batch is an array and the same material buffer may be shared by render workers. Green
// substitution therefore follows compare-before-write/compare-before-restore: if another worker
// has already changed an element, this worker leaves it alone instead of restoring somebody
// else's pointer. This is the per-element complement of World Only's conservative whole-batch
// rejection.
struct GreenMaterialRestore {
	CMaterial2* original = nullptr;
	bool wrote = false;
	unsigned int originalColor = 0xffffffffu;
	bool wroteColor = false;
};

// Skip-class elements additionally get their per-element modulate colour forced to the pure
// chroma green. This is the mechanism of last resort for draws that measurably IGNORE the
// material pointer (detail grass tufts: replaced=1 in the log, original texture on screen) --
// the colour write IS consumed by those draws (verified live: `world modulate props` tints the
// tufts). The colour is green rather than transparent black because the alpha channel is
// ignored by these pipelines (measured: RGBA 0,0,0,0 rendered BLACK wisps and black grime
// blobs, not nothing): alpha-tested sprites become green-modulated wisps and translucent grime
// decals blend green-over-green, both of which key out, while black does not.
// Byte layout matches the modulate write in the base hook: [r, g, b, a] in memory, so pure
// green with full alpha is 0xFF00FF00 as a little-endian dword. Doubles as the
// never-a-legitimate-original sentinel for compare-before-restore.
constexpr unsigned int kPlayersOnlyGreenInvisibleColor = 0xFF00FF00u;

void DeferPlayersOnlyGreenRestore(CMaterial2** slot, CMaterial2* original) {
	if (!slot || !original) return;
	std::lock_guard<std::mutex> lock(g_playersOnlyGreenRestoreMutex);
	// A descriptor element can be submitted more than once while worker command lists are built.
	// Keep the first true original and never overwrite it with an already-green pointer.
	g_playersOnlyGreenPendingRestores.emplace((void*)slot, original);
}

void DeferPlayersOnlyGreenColorRestore(unsigned int* slot, unsigned int original) {
	if (!slot || original == kPlayersOnlyGreenInvisibleColor) return;
	std::lock_guard<std::mutex> lock(g_playersOnlyGreenRestoreMutex);
	g_playersOnlyGreenPendingColorRestores.emplace((void*)slot, original);
}

// SEH boundary kept free of C++ objects: batch buffers belong to the engine and may be gone by
// the time the deferred restore runs; a stale slot must degrade to "not restored", never fault.
bool GuardedRestorePlayersOnlyGreenColor(unsigned int* slot, unsigned int original) {
	if (!slot) return false;
	__try {
		if (*slot == kPlayersOnlyGreenInvisibleColor) {
			*slot = original;
			return true;
		}
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

int ApplyPlayersOnlyGreenMaterials(CBaseSceneData* sceneData, int count,
	GreenMaterialRestore* saved, const char* source) {
	if (!sceneData || !saved || count <= 0) return 0;
	const int limit = count > 256 ? 256 : count;
	if (count > 256 && Filmmaker::MvmDebugLog_Active()) {
		// Elements past the repaint clamp keep their original material and leak into the plate.
		// Bounded evidence channel for exactly that: which submission path, how big the batch.
		static std::atomic<unsigned> s_clampLogged{0};
		const unsigned n = s_clampLogged.fetch_add(1, std::memory_order_relaxed);
		if (n < 64) {
			CMaterial2* first = nullptr;
			ProbeReadPointerAt(sceneData, offsetof(CBaseSceneData, material), (void**)&first);
			const char* firstName = first ? ProbeReadMaterialName(first) : nullptr;
			Filmmaker::MvmDebugLog_LinefAlways("render.playersonlygreen.clamp",
				"via=%s count=%d limit=256 first='%s'", source ? source : "?",
				count, firstName ? firstName : "<none>");
		}
	}
	CMaterial2* green = Filmmaker::RenderPlayersOnlyGreen_Material();
	CMaterial2* invisible = Filmmaker::RenderPlayersOnlyGreen_InvisibleMaterial();
	CMaterial2* skipChroma = Filmmaker::RenderPlayersOnlyGreen_SkipMaterial();
	int writes = 0;
	// Bounded composition dump: whenever a batch carries at least one skip-class element but was
	// NOT batch-skipped, record what else is riding in it. This is the evidence channel for "why
	// did the whole-batch skip not fire" -- read it before widening the vocabulary.
	bool anySkipClass = false;
	char mixedNames[8][96] = {};
	int mixedCount = 0;
	for (int i = 0; i < limit; ++i) {
		saved[i] = GreenMaterialRestore{};
		CMaterial2* material = nullptr;
		if (!ProbeReadPointerAt(sceneData + i, offsetof(CBaseSceneData, material), (void**)&material))
			continue;
		saved[i].original = material;
		char name[512] = {};
		const bool named = material && SceneSystem_MaterialName(material, name, sizeof(name));
		const char* materialName = named ? name : nullptr;
		const Filmmaker::PlayersOnlyGreenDomain domain =
			Filmmaker::RenderPlayersOnlyGreen_Classify(materialName);
		const bool admitted = Filmmaker::RenderPlayersOnlyGreen_Admits(domain);
		// Skip-class elements in MIXED batches cannot be dropped with the batch and must not be
		// material-swapped either: pipelines that consume the swap render the substitute's actual
		// texture (chroma -> black splats; tools/toolsinvisible -> literal "INVISIBLE" dev plates,
		// measured in job output 2026-08-20), and the grass-tuft pipeline ignores the swap
		// entirely. They keep their ORIGINAL material -- original alpha behaviour intact -- and
		// are neutralised purely by the green colour modulate below.
		const bool skipClass = Filmmaker::RenderPlayersOnlyGreen_IsSkippedSceneMaterial(materialName);
		// Opaque skip-class materials (pump verdict: no alpha-test reference) rejoin the chroma
		// repaint -- that is the only treatment that turns a dark rock/cloth chunk flat green.
		const bool skipWantsChroma = skipClass
			&& Filmmaker::RenderPlayersOnlyGreen_SkipClassWantsChroma(material);
		// Skip-class elements swap to the dedicated skip chroma (green albedo, white tint), not
		// the world chroma: the world one is black-tinted and renders BLACK on the foreign
		// pipelines these elements often use (measured on de_cache foliage 2026-08-20).
		CMaterial2* target = (skipClass && skipChroma) ? skipChroma : green;
		bool replaced = false;
		if (!admitted && (!skipClass || skipWantsChroma)
			&& target && material != green && material != invisible) {
			CMaterial2* current = nullptr;
			if (ProbeReadPointerAt(sceneData + i, offsetof(CBaseSceneData, material), (void**)&current)
				&& current == material
				&& ProbeWritePointerAt(sceneData + i, offsetof(CBaseSceneData, material), target)) {
					saved[i].wrote = true;
					replaced = true;
				++writes;
				if (Filmmaker::RenderPlayersOnlyGreen_RenderActive())
					DeferPlayersOnlyGreenRestore(&sceneData[i].material, material);
			}
		}
		if (skipClass) {
			anySkipClass = true;
			// Report for material-level suppression -- the treatment that reaches cached
			// command-list draws (see RenderPlayersOnlyGreen_NoteSkipClassMaterial).
			Filmmaker::RenderPlayersOnlyGreen_NoteSkipClassMaterial(material);
		}
		if (mixedCount < 8) {
			_snprintf_s(mixedNames[mixedCount], sizeof(mixedNames[mixedCount]), _TRUNCATE,
				"%s%s", skipClass ? "*" : "",
				materialName && *materialName ? materialName : "<unnamed>");
			++mixedCount;
		}
		// The colour write is what actually removes draws that ignore the material pointer (see
		// kPlayersOnlyGreenInvisibleColor). Compare-before-write: never treat our own zero as an
		// element's original.
		if (!admitted && skipClass) {
			const unsigned int currentColor = sceneData[i].color;
			if (currentColor != kPlayersOnlyGreenInvisibleColor) {
				saved[i].originalColor = currentColor;
				sceneData[i].color = kPlayersOnlyGreenInvisibleColor;
				saved[i].wroteColor = true;
				if (Filmmaker::RenderPlayersOnlyGreen_RenderActive())
					DeferPlayersOnlyGreenColorRestore(&sceneData[i].color, currentColor);
			}
		}
		if (domain == Filmmaker::PlayersOnlyGreenDomain::Unknown)
			Filmmaker::RenderPlayersOnlyGreen_NoteUnknown(materialName);
		Filmmaker::RenderPlayersOnlyGreen_NoteDraw(domain, replaced);
		if (Filmmaker::MvmDebugLog_Active() && replaced && writes <= 4)
			Filmmaker::MvmDebugLog_Linef("render.playersonlygreen.draw",
				"via=%s index=%d material='%s' domain=%d replaced=1",
				source ? source : "?", i, materialName && *materialName ? materialName : "<unknown>",
				(int)domain);
	}
	if (anySkipClass && Filmmaker::MvmDebugLog_Active()) {
		static std::atomic<unsigned> s_mixedLogged{0};
		if (s_mixedLogged.fetch_add(1, std::memory_order_relaxed) < 24) {
			char line[900] = {};
			size_t used = 0;
			for (int i = 0; i < mixedCount && used + 100 < sizeof(line); ++i) {
				const int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "%s%s",
					i ? " | " : "", mixedNames[i]);
				if (n > 0) used += (size_t)n;
			}
			Filmmaker::MvmDebugLog_LinefAlways("render.playersonlygreen.mixed",
				"via=%s count=%d shown=%d elems=[%s] (*=skip-class)",
				source ? source : "?", count, mixedCount, line);
		}
	}
	return limit;
}

void RestorePlayersOnlyGreenMaterials(CBaseSceneData* sceneData, int count,
	const GreenMaterialRestore* saved) {
	if (!sceneData || !saved || count <= 0) return;
	// The dedicated pass records scene draws into worker command lists. Its material pointer must
	// remain green until the reliable end-of-pass callback; restoring here would race the command
	// list and make the GPU read the original world material. Live preview keeps immediate restore.
	if (Filmmaker::RenderPlayersOnlyGreen_RenderActive()) return;
	const int limit = count > 256 ? 256 : count;
	CMaterial2* green = Filmmaker::RenderPlayersOnlyGreen_Material();
	CMaterial2* invisible = Filmmaker::RenderPlayersOnlyGreen_InvisibleMaterial();
	CMaterial2* skipChroma = Filmmaker::RenderPlayersOnlyGreen_SkipMaterial();
	if (!green) return;
	for (int i = 0; i < limit; ++i) {
		if (saved[i].wroteColor && sceneData[i].color == kPlayersOnlyGreenInvisibleColor)
			sceneData[i].color = saved[i].originalColor;
		if (!saved[i].wrote || !saved[i].original) continue;
		CMaterial2* current = nullptr;
		if (ProbeReadPointerAt(sceneData + i, offsetof(CBaseSceneData, material), (void**)&current)
			&& (current == green || (skipChroma && current == skipChroma)
				|| (invisible && current == invisible)))
			ProbeWritePointerAt(sceneData + i, offsetof(CBaseSceneData, material), saved[i].original);
	}
}

// sceneData is actually array and can be iterated, a4 is count
// Color and textures can be replace here, but there are some issues with that,
// so we only "toggle" on/off here for now.
void new_DrawBaseSceneObject(void* This, void* a2, CBaseSceneData* sceneData, int a4, int a5, void* a6, void* a7) {
	DrawBaseSceneObject_t original = FindDrawDescriptorOriginal(This);
	if (!original) return;
	// Now that every drawable descriptor routes here, the batch pointer can no longer be assumed
	// non-null: the two original types always supplied one, the newly covered ones are not ours to
	// promise. A descriptor with no batch has no material to classify, so pass it straight through
	// rather than filtering a draw we cannot identify.
	if (!sceneData) return original(This, a2, sceneData, a4, a5, a6, a7);
	// SEH-guarded rather than a direct field read. More than one descriptor class routes here now,
	// and "the batch pointer always has this layout" is an assumption per class, not a fact -- a
	// bad one must degrade to an unfiltered pass-through, never fault in the render worker.
	CMaterial2* batchMaterial = nullptr;
	if (!ProbeReadPointerAt(sceneData, offsetof(CBaseSceneData, material), (void**)&batchMaterial))
		return original(This, a2, sceneData, a4, a5, a6, a7);
	const char* rawMaterialName = batchMaterial ? ProbeReadMaterialName(batchMaterial) : nullptr;
	Filmmaker::RagdollWeapon_ProbeDraw(sceneData, a4, rawMaterialName);
	if (batchMaterial == 0) return original(This, a2, sceneData, a4, a5, a6, a7);
	// Debug only (`world modulate hide`): drop this batch when its material name falls in the chosen hash
	// bucket, so a screenshot bisect can name the material behind a pixel.
	if (Filmmaker::WorldModulation_HideDraw(rawMaterialName)) return;
	if (IsBloodOnlyBatch(sceneData, a4)) { Filmmaker::ParticleFx_NoteBloodDecalSkipped(); return; }

	// ---- World Only capture pass ------------------------------------------------------------
	// Draw-level subject rejection. This hook is the only place every drawable batch passes
	// through, so it is where a "which subjects are in this plate" question has to be answered --
	// but the POLICY lives entirely in Filmmaker::RenderWorldOnly, which this file knows nothing
	// about beyond one boolean. Idle cost is a single relaxed atomic load.
	if (Filmmaker::RenderWorldOnly_Active()) {
		const char* names[kMaxBatchNames] = {};
		const int nameCount = CollectBatchMaterialNames(sceneData, a4, names);
		if (Filmmaker::RenderWorldOnly_RejectsBatchFrom(names, nameCount, DrawDescriptorTag(This)))
			return;
	}

	std::string matName = rawMaterialName ? rawMaterialName : "";

	// Clouds keep their own visibility control.  Sun visibility is owned by SunFx's material
	// contribution (irradiance/opacity), so hiding the sun cannot accidentally cull the sky/cloud
	// draw when the camera crosses the zenith.
	if ((!Filmmaker::WorldTransient::Number("cloudsVisible",g_CustomSky.drawClouds) || g_playersOnlyGreenCloudsSuppressed.load(std::memory_order_acquire))
		&& matName.find("clouds") != -1) return;

	// A recorded Players render decides by the pass schedule; the live preview by its policy bit.
	const bool greenPolicy = Filmmaker::RenderPlayersOnlyGreen_RenderActive()
		|| (SceneSystem_PlayersOnlyGreenPolicyActive() && Filmmaker::RenderPlayersOnlyGreen_IsArmed());
	const char* skipFirstName = nullptr;
	if (greenPolicy && IsEntirelyPlayersOnlyGreenSkippedBatch(sceneData, a4, &skipFirstName)) {
		Filmmaker::RenderPlayersOnlyGreen_NoteBatchSkipped(DrawDescriptorTag(This), a4, skipFirstName);
		return;
	}
	GreenMaterialRestore greenSaved[256] = {};
	const int greenCount = greenPolicy
		? ApplyPlayersOnlyGreenMaterials(sceneData, a4, greenSaved, DrawDescriptorTag(This)) : 0;

	// ---- world / prop / sun-cloud colour modulation ----------------------------------------
	// This hook is the home for scene-draw work, so the write happens here; Filmmaker::
	// WorldModulation owns the categories and the colours (see Docs/effects/README.md 2.1 for why
	// this is the one deliberate hook-sharing exception).
	//
	// OVERRIDE, CALL, RESTORE. a4 is the batch count. The colour is assigned rather than blended,
	// so it would not compound -- but the batch buffer belongs to the engine and may be reused, and
	// leaving our colour in it would tint things we never classified.
	// The whole batch, now that the 0x70 stride is MEASURED rather than assumed (see the struct).
	// `a4` is the count -- ProbeDrawBatch above already names that parameter, from earlier RE here.
	//
	// OVERRIDE, CALL, RESTORE. The colour is assigned rather than blended so it cannot compound,
	// but the batch buffer belongs to the engine and may be reused; leaving our colour behind would
	// tint draws we never classified.
	Filmmaker::WorldModulation_ProbeBatch(sceneData, a4, rawMaterialName);
	unsigned char modRgba[4];
	if (Filmmaker::WorldModulation_Active()
		&& Filmmaker::WorldModulation_ColorFor(rawMaterialName, modRgba)
		&& a4 > 0 && a4 <= 256) {
		unsigned int saved[256];
		for (int i = 0; i < a4; ++i) {
			saved[i] = sceneData[i].color;
			unsigned char* c = (unsigned char*)&sceneData[i].color;
			c[0] = modRgba[0]; c[1] = modRgba[1]; c[2] = modRgba[2]; c[3] = modRgba[3];
		}
		if (Filmmaker::MvmDebugLog_Active()) {
			static unsigned long long s_modWriteIndex = 0;
			if ((++s_modWriteIndex % 2048ull) == 1) {
				Filmmaker::MvmDebugLog_Linef("world.mod.draw.write",
					"material='%s' target=%p colorOffset=0x%llx count=%d "
					"input0=0x%08x wrote=(%u %u %u %u)",
					rawMaterialName ? rawMaterialName : "", (void*)sceneData,
					(unsigned long long)offsetof(CBaseSceneData, color), a4, saved[0],
					(unsigned)modRgba[0], (unsigned)modRgba[1],
					(unsigned)modRgba[2], (unsigned)modRgba[3]);
			}
		}
		original(This, a2, sceneData, a4, a5, a6, a7);
		for (int i = 0; i < a4; ++i) sceneData[i].color = saved[i];
		if (greenCount) RestorePlayersOnlyGreenMaterials(sceneData, greenCount, greenSaved);
		return;
	}

	original(This, a2, sceneData, a4, a5, a6, a7);
	if (greenCount) RestorePlayersOnlyGreenMaterials(sceneData, greenCount, greenSaved);
}

// ---- AGGREGATE SCENE OBJECTS ------------------------------------------------------------------
// Most of a CS2 map draws through CAggregateSceneObjectDesc. It has its own detour because the
// descriptor is shared between render workers, but it still has to obey the same domain filter as
// CBaseSceneObject. In particular, Mirage's electric_cables01 material is a prop aggregate; if
// this path treats every aggregate as World, Props/World scopes drift and the cable renderer can
// remain visible in a subject-only plate.
DrawBaseSceneObject_t g_AggregateOriginal = nullptr;
void** g_AggregateVtable = nullptr;

void __fastcall new_DrawAggregateSceneObject(void* This, void* a2, CBaseSceneData* sceneData, int a4, int a5, void* a6, void* a7) {
	if (!g_AggregateOriginal) return;
	// Debug only (`world modulate hide`): most of a map draws through this aggregate path, so the
	// bucket filter has to cover it as well to name the material behind a pixel.
	if (sceneData) {
		CMaterial2* hideMaterial = nullptr;
		if (ProbeReadPointerAt(sceneData, offsetof(CBaseSceneData, material), (void**)&hideMaterial) && hideMaterial
			&& Filmmaker::WorldModulation_HideDraw(ProbeReadMaterialName(hideMaterial))) return;
	}
	if (g_playersOnlyGreenCloudsSuppressed.load(std::memory_order_acquire) && sceneData) {
		CMaterial2* aggregateMaterial = nullptr;
		if (ProbeReadPointerAt(sceneData, offsetof(CBaseSceneData, material), (void**)&aggregateMaterial)
			&& aggregateMaterial) {
			const char* name = ProbeReadMaterialName(aggregateMaterial);
			if (name && std::strstr(name, "clouds")) return;
		}
	}

	if (sceneData && IsBloodOnlyBatch(sceneData, a4)) { Filmmaker::ParticleFx_NoteBloodDecalSkipped(); return; }

	// World Only also has to cover this path. Aggregates are mostly baked map geometry (which the
	// filter keeps anyway), but "mostly" is not a guarantee -- Mirage's electric_cables01 is a prop
	// aggregate, and a subject filter that only covers one submission path silently repaints the
	// plate with whatever the other path draws. Read-only: the draw is either passed on or skipped.
	if (Filmmaker::RenderWorldOnly_Active()) {
		const char* names[kMaxBatchNames] = {};
		const int nameCount = CollectBatchMaterialNames(sceneData, a4, names);
		if (Filmmaker::RenderWorldOnly_RejectsBatchFrom(names, nameCount, "agg"))
			return;
	}

	// A recorded Players render decides by the pass schedule; the live preview by its policy bit.
	const bool greenPolicy = Filmmaker::RenderPlayersOnlyGreen_RenderActive()
		|| (SceneSystem_PlayersOnlyGreenPolicyActive() && Filmmaker::RenderPlayersOnlyGreen_IsArmed());
	const char* skipFirstName = nullptr;
	if (greenPolicy && IsEntirelyPlayersOnlyGreenSkippedBatch(sceneData, a4, &skipFirstName)) {
		Filmmaker::RenderPlayersOnlyGreen_NoteBatchSkipped("agg", a4, skipFirstName);
		return;
	}
	GreenMaterialRestore greenSaved[256] = {};
	const int greenCount = greenPolicy
		? ApplyPlayersOnlyGreenMaterials(sceneData, a4, greenSaved, "agg") : 0;

	if (!Filmmaker::WorldModulation_Active()) {
		g_AggregateOriginal(This, a2, sceneData, a4, a5, a6, a7);
		if (greenCount) RestorePlayersOnlyGreenMaterials(sceneData, greenCount, greenSaved);
		return;
	}

	// Write and leave it -- NO restore around this call. Aggregate objects are shared between render
	// threads, so a save/restore straddling the original tears: one thread saves another's tint as
	// the "original". The module remembers the true originals and puts them back on reset instead.
	Filmmaker::WorldModulation_AggregateApply(sceneData, a4);
	g_AggregateOriginal(This, a2, sceneData, a4, a5, a6, a7);
	if (greenCount) RestorePlayersOnlyGreenMaterials(sceneData, greenCount, greenSaved);
}

size_t g_SceneObject_pSceneObjectDesc_Offset = -1;

// Upstream asserts 0x68; the current CS2 build measures 0x70 (see CBaseSceneData above).
static_assert(sizeof(CBaseSceneData) == 0x70, "Unexpected CBaseSceneData size.");

enum class SceneObjectDrawPolicy {
	Draw,
	Hide,
	DepthPassesOnly
};

// Written by console commands and the engine thread while render workers read them in the draw
// hooks: atomic so an edit mid-frame is a clean old-or-new value, never a torn one.
std::atomic<SceneObjectDrawPolicy> g_BaseSceneObjectsPolicy{SceneObjectDrawPolicy::Draw};
std::atomic<SceneObjectDrawPolicy> g_AnimatableSceneObjectsPolicy{SceneObjectDrawPolicy::Draw};
std::atomic<SceneObjectDrawPolicy> g_AggregateSceneObjectsPolicy{SceneObjectDrawPolicy::Draw};
std::atomic<SceneObjectDrawPolicy> g_OverlaysPolicy{SceneObjectDrawPolicy::Draw};
std::atomic_int g_iSceneFilterDebug{0};

enum class SceneSemanticGroup : int {
	ViewModel = 0,
	Particles = 1,
	FirstPersonLegs = 2,
	Shells = 3,
	Weapons = 4,
	Players = 5,
	World = 6,
	Sky = 7,
	Smoke = 8,
	Xhair = 9,
	Count = 10
};

std::atomic<SceneObjectDrawPolicy> g_SceneSemanticPolicies[(int)SceneSemanticGroup::Count] = {
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw,
	SceneObjectDrawPolicy::Draw
};

std::atomic_bool g_bSceneFilterSystemActive = false;

void UpdateSceneFilterSystemActive() {
	bool bActive =
		0 < g_iSceneFilterDebug
		|| g_BaseSceneObjectsPolicy != SceneObjectDrawPolicy::Draw
		|| g_AnimatableSceneObjectsPolicy != SceneObjectDrawPolicy::Draw
		|| g_AggregateSceneObjectsPolicy != SceneObjectDrawPolicy::Draw
		|| g_OverlaysPolicy != SceneObjectDrawPolicy::Draw
		|| g_PickerActive;

	for(int i = 0; !bActive && i < (int)SceneSemanticGroup::Count; i++) {
		bActive = g_SceneSemanticPolicies[i] != SceneObjectDrawPolicy::Draw;
	}

	g_bSceneFilterSystemActive = bActive;
}

void ClearSceneFliterSystemPolicies() {
	g_BaseSceneObjectsPolicy = SceneObjectDrawPolicy::Draw;
	g_AnimatableSceneObjectsPolicy = SceneObjectDrawPolicy::Draw;
	g_AggregateSceneObjectsPolicy = SceneObjectDrawPolicy::Draw;
	g_OverlaysPolicy = SceneObjectDrawPolicy::Draw;

	for(int i = 0; i < (int)SceneSemanticGroup::Count; i++) {
		g_SceneSemanticPolicies[i] = SceneObjectDrawPolicy::Draw;
	}

	UpdateSceneFilterSystemActive();
}

void SetupSceneFilterPolicies(const class CStreamSettings & settings) {
	if (settings.Capture == CStreamSettings::Capture_e::BeforeUi) {
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Xhair] = SceneObjectDrawPolicy::Hide;
	}

	switch(settings.ViewModelAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::ViewModel] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::ViewModel] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::ViewModel] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.ParticlesAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Particles] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Particles] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Particles] = SceneObjectDrawPolicy::Draw;
		break;
	}	
	switch(settings.FirstPersonLegsAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::FirstPersonLegs] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::FirstPersonLegs] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::FirstPersonLegs] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.ShellsAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Shells] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Shells] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Shells] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.WeaponsAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Weapons] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Weapons] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Weapons] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.PlayersAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Players] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Players] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Players] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.WorldAction) {
	case CStreamSettings::Action::NoDraw:
		g_AggregateSceneObjectsPolicy = SceneObjectDrawPolicy::Hide;
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::World] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_AggregateSceneObjectsPolicy = SceneObjectDrawPolicy::DepthPassesOnly;
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::World] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_AggregateSceneObjectsPolicy = SceneObjectDrawPolicy::Draw;
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::World] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.SkyAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Sky] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Sky] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Sky] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.SmokeAction) {
	case CStreamSettings::Action::NoDraw:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Smoke] = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Smoke] = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_SceneSemanticPolicies[(int)SceneSemanticGroup::Smoke] = SceneObjectDrawPolicy::Draw;
		break;
	}
	switch(settings.OverlaysAction) {
	case CStreamSettings::Action::NoDraw:
		g_OverlaysPolicy = SceneObjectDrawPolicy::Hide;
		break;
	case CStreamSettings::Action::ZOnly:
		g_OverlaysPolicy = SceneObjectDrawPolicy::DepthPassesOnly;
		break;
	default:
		g_OverlaysPolicy = SceneObjectDrawPolicy::Draw;
		break;
	}

	UpdateSceneFilterSystemActive();
}

enum class SceneObjectFilterClass {
	Base,
	Animatable,
	Aggregate,
	SkyBox,
	SkyBox6Face,
	ProjectedDecal,
	SmokeVolume,
	Particles,
	Unknown
};

std::map<void**,SceneObjectFilterClass> g_VtableToSceneObjectFilterClass;

struct SceneLayerContext {
	const char* ViewName = nullptr;
	const char* ViewPass = nullptr;
	uint32_t Flags = 0;
};

static const char* SceneObjectFilterClassToString(SceneObjectFilterClass filterClass) {
	switch (filterClass) {
	case SceneObjectFilterClass::Base: return "base";
	case SceneObjectFilterClass::Animatable: return "animatable";
	case SceneObjectFilterClass::Aggregate: return "aggregate";
	case SceneObjectFilterClass::SkyBox: return "skybox";
	case SceneObjectFilterClass::SkyBox6Face: return "skybox6face";
	case SceneObjectFilterClass::ProjectedDecal: return "projecteddecal";
	case SceneObjectFilterClass::SmokeVolume: return "smokevolume";
	case SceneObjectFilterClass::Particles: return "particles";
	default: return "unknown";
	}
}

static const char* SceneObjectDrawPolicyToString(SceneObjectDrawPolicy policy) {
	switch (policy) {
	case SceneObjectDrawPolicy::Draw: return "draw";
	case SceneObjectDrawPolicy::Hide: return "hide";
	case SceneObjectDrawPolicy::DepthPassesOnly: return "zonly";
	default: return "unknown";
	}
}

static bool TryParseSceneObjectDrawPolicy(const char* value, SceneObjectDrawPolicy& outPolicy) {
	if (0 == _stricmp(value, "1") || 0 == _stricmp(value, "draw")) {
		outPolicy = SceneObjectDrawPolicy::Draw;
		return true;
	}
	if (0 == _stricmp(value, "0") || 0 == _stricmp(value, "hide")) {
		outPolicy = SceneObjectDrawPolicy::Hide;
		return true;
	}
	if (0 == _stricmp(value, "zonly")) {
		outPolicy = SceneObjectDrawPolicy::DepthPassesOnly;
		return true;
	}
	return false;
}

static const char* SceneSemanticGroupToString(SceneSemanticGroup group) {
	switch (group) {
	case SceneSemanticGroup::ViewModel: return "viewModel";
	case SceneSemanticGroup::Particles: return "particles";
	case SceneSemanticGroup::FirstPersonLegs: return "firstPersonLegs";
	case SceneSemanticGroup::Shells: return "shells";
	case SceneSemanticGroup::Weapons: return "weapons";
	case SceneSemanticGroup::Players: return "players";
	case SceneSemanticGroup::World: return "world";
	case SceneSemanticGroup::Sky: return "sky";
	case SceneSemanticGroup::Smoke: return "smoke";
	default: return "unknown";
	}
}

static bool TryGetSceneSemanticGroup(const char* value, SceneSemanticGroup& outGroup) {
	if (0 == _stricmp(value, "viewModel")) {
		outGroup = SceneSemanticGroup::ViewModel;
		return true;
	}
	if (0 == _stricmp(value, "particles")) {
		outGroup = SceneSemanticGroup::Particles;
		return true;
	}
	if (0 == _stricmp(value, "firstPersonLegs")) {
		outGroup = SceneSemanticGroup::FirstPersonLegs;
		return true;
	}
	if (0 == _stricmp(value, "weapons")) {
		outGroup = SceneSemanticGroup::Weapons;
		return true;
	}
	if (0 == _stricmp(value, "shells")) {
		outGroup = SceneSemanticGroup::Shells;
		return true;
	}
	if (0 == _stricmp(value, "players") || 0 == _stricmp(value, "player")) {
		outGroup = SceneSemanticGroup::Players;
		return true;
	}
	if (0 == _stricmp(value, "world")) {
		outGroup = SceneSemanticGroup::World;
		return true;
	}
	if (0 == _stricmp(value, "sky") || 0 == _stricmp(value, "skybox")) {
		outGroup = SceneSemanticGroup::Sky;
		return true;
	}
	if (0 == _stricmp(value, "smoke")) {
		outGroup = SceneSemanticGroup::Smoke;
		return true;
	}
	return false;
}

static SceneObjectDrawPolicy GetSceneObjectClassPolicy(SceneObjectFilterClass filterClass) {
	switch (filterClass) {
	case SceneObjectFilterClass::Base: return g_BaseSceneObjectsPolicy;
	case SceneObjectFilterClass::Animatable: return g_AnimatableSceneObjectsPolicy;
	case SceneObjectFilterClass::Aggregate: return g_AggregateSceneObjectsPolicy;
	case SceneObjectFilterClass::SkyBox: return SceneObjectDrawPolicy::Draw;
	case SceneObjectFilterClass::SkyBox6Face: return SceneObjectDrawPolicy::Draw;
	case SceneObjectFilterClass::ProjectedDecal: return SceneObjectDrawPolicy::Draw;
	case SceneObjectFilterClass::Unknown:
	default: return SceneObjectDrawPolicy::Draw;
	}
}

static bool SceneObjectFilterClassIsSkyBox(SceneObjectFilterClass filterClass) {
	return filterClass == SceneObjectFilterClass::SkyBox
		|| filterClass == SceneObjectFilterClass::SkyBox6Face;
}

static bool SceneObjectFilterClassIsProjectedDecal(SceneObjectFilterClass filterClass) {
	return filterClass == SceneObjectFilterClass::ProjectedDecal;
}

static bool SceneObjectFilterClassHasKnownSceneDataLayout(SceneObjectFilterClass filterClass) {
	return true;
	/*return filterClass == SceneObjectFilterClass::Base
		|| filterClass == SceneObjectFilterClass::Animatable
		|| filterClass == SceneObjectFilterClass::Aggregate;*/
}

static bool StringContains(const char* value, const char* pattern) {
	return value && pattern && nullptr != strstr(value, pattern);
}

static bool SceneLayerContextIsViewModel(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "ViewModel");
}

static bool SceneLayerContextIsFirstPersonLegs(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "FirstpersonLegs");
}

static bool SceneLayerContextIsSky(const SceneLayerContext& context) {
	return StringContains(context.ViewName, "Csgo3DSkyboxView")
		|| StringContains(context.ViewPass, "3DSkybox")
		|| StringContains(context.ViewPass, "Skybox")
		|| StringContains(context.ViewPass, "Sky");
}

static bool SceneLayerContextIsVisiblity(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "Visibility") // e.g. PlayerVisibility...
		|| StringContains(context.ViewPass, "Occlusion") // e.g. PlayerOcclusion...
	;
}

static bool SceneLayerContextIsPlayers(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "Characters")
		|| StringContains(context.ViewPass, "Player");
}

static bool SceneLayerContextIsShadowPass(const SceneLayerContext& context) {
	return StringContains(context.ViewName, "CSM")
		|| StringContains(context.ViewName, "DrawShadowMapsForLight")
		|| StringContains(context.ViewPass, "DrawShadowMapsForLight");
}

static bool SceneLayerContextIsDecals(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "Decals");
}

static bool SceneLayerContextIsStencil(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "Stencil");

}

static bool SceneLayerContextIsCables(const SceneLayerContext& context) {
	return StringContains(context.ViewPass, "Cables") ;
}

static bool SceneLayerContextIsDepthPassesOnlyExcluded(const SceneLayerContext& context) {
	return SceneLayerContextIsDecals(context)
		|| SceneLayerContextIsCables(context)
		|| SceneLayerContextIsStencil(context)
		|| StringContains(context.ViewPass, "Translucent")
		|| StringContains(context.ViewPass, "Overlay");
}

static bool SceneDataMaterialIsPlayer(SceneObjectFilterClass filterClass, const char* materialName) {
	if (materialName == nullptr) return false;

	return StringContains(materialName, "characters/models/")
		|| StringContains(materialName, "agents/models/")
		|| StringContains(materialName, "models/player/")
		|| (filterClass == SceneObjectFilterClass::Animatable && StringContains(materialName, "compmat_")); // skinned weapons/gloves
}

static bool SceneDataMaterialIsWeapon(SceneObjectFilterClass filterClass, const char* materialName, const CBaseSceneData * pSceneData, const SceneLayerContext& context) {
	if (materialName == nullptr) return false;

	if (StringContains(materialName, "weapons/keychains/") 
		// || StringContains(materialName, "weapons/models/shared/stattrak")
		// || StringContains(materialName, "weapons/models/shared/nametag")
		|| StringContains(materialName, "weapons/models/")
		|| StringContains(materialName, "models/weapons/")
	) {
		return !SceneLayerContextIsPlayers(context);
	}

	if (StringContains(materialName, "compmat_"))
		// && filterClass == SceneObjectFilterClass::Animatable)
	{
		if (nullptr == pSceneData) return false;
		auto sceneObject = pSceneData->sceneObject;
		if (nullptr == sceneObject) return false;
		auto meshInstance = *(u_char**)((u_char*)sceneObject + 0x10);
		if (nullptr == meshInstance) return false;
		auto model = **(u_char***)(meshInstance + 0x8);
		auto modelName = *(const char**)(model + 0x8);

		if (StringContains(modelName, "weapons/models")) {// double check because we might get gloves in case they have skin
			return !SceneLayerContextIsPlayers(context);
			// auto modelState = (*(u_char**)((u_char*)sceneObject + 0x110)) + 0x10;
			// // see 3 fn in vtable for ".?AVNetworkVar_m_modelState@CSkeletonInstance@@"
			// auto ent = *(CEntityInstance**)(modelState - 0x140 - 0x38);
			// auto ownerHandle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(uint32_t*)((u_char*)ent + g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity));
			// if (ownerHandle.IsValid()) return false;
			// return true;
		}
	}

	return false;
}

static bool SceneDataMaterialIsShell(SceneObjectFilterClass filterClass, const char* materialName) {
	if (materialName == nullptr) return false;

	return StringContains(materialName, "materials/models/weapons/shared/shells/")
		|| StringContains(materialName, "weapons/models/shared/shells/");
}

static bool SceneDataIsCables(SceneObjectFilterClass filterClass, const CBaseSceneData * pSceneData, const SceneLayerContext& context) {
	if(filterClass != SceneObjectFilterClass::Particles) return false;
	if(SceneLayerContextIsCables(context)) return true;
	// pSceneObject + 0x10 -> object of ".?AVCParticleSystemDefinition@@"
	// poi(poi(poi(poi(poi(pSceneObject+0x10)+0x90)))-8)+c --> ".?AVC_OP_RopeSpringConstraint@@"
	// poi(poi(pSceneObject+0x10)+0x250) --> // particles/entity/path_particle_cable_static.vpcf
	if (nullptr == pSceneData) return false;
	auto sceneObject = pSceneData->sceneObject;
	auto particleSystemDefinition = *(u_char**)((u_char*)sceneObject + 0x10);
	if (nullptr == particleSystemDefinition) return false;	
	const char * particleDefinitionFilePath = (const char *)*(u_char**)((u_char*)particleSystemDefinition + 0x250);
	return nullptr != particleDefinitionFilePath && StringContains(particleDefinitionFilePath, "particle_cable"); // There are multiple!
}

static SceneSemanticGroup ClassifySceneObject(SceneObjectFilterClass filterClass, const SceneLayerContext& context, const char* materialName, const CBaseSceneData * pSceneData) {
	// Precedence is intentional.
	if (!SceneDataIsCables(filterClass,pSceneData,context) && filterClass == SceneObjectFilterClass::Particles) return SceneSemanticGroup::Particles;
	if (SceneLayerContextIsViewModel(context)) return SceneSemanticGroup::ViewModel;
	if (SceneLayerContextIsFirstPersonLegs(context)) return SceneSemanticGroup::FirstPersonLegs;
	if (SceneDataMaterialIsShell(filterClass, materialName)) return SceneSemanticGroup::Shells;
	if (SceneDataMaterialIsWeapon(filterClass, materialName, pSceneData, context)) return SceneSemanticGroup::Weapons;
	if (SceneDataMaterialIsPlayer(filterClass, materialName)) return SceneSemanticGroup::Players;
	if (SceneLayerContextIsPlayers(context)) return SceneSemanticGroup::Players;
	if (SceneLayerContextIsSky(context)) return SceneSemanticGroup::Sky;
	if (filterClass == SceneObjectFilterClass::SmokeVolume) return SceneSemanticGroup::Smoke;
	return SceneSemanticGroup::World;
}

static bool SceneLayerContextIsDepthPass(const SceneLayerContext& context) {
	if (StringContains(context.ViewName, "CSM")) return true;
	if (StringContains(context.ViewPass, "DrawDepthOpaque")) return true;
	if (StringContains(context.ViewPass, "Depth Prepass")) return true;
	if (StringContains(context.ViewPass, "Depth prepass")) return true;

	return false;
}

static SceneObjectDrawPolicy ApplyLayerAwarePolicy(SceneObjectFilterClass filterClass, const SceneLayerContext & context, const char* materialName, SceneObjectDrawPolicy policy) {
	if (policy != SceneObjectDrawPolicy::Draw) {
		if(SceneLayerContextIsShadowPass(context)) {
			return SceneObjectDrawPolicy::Draw;
		}
	}

	if (policy == SceneObjectDrawPolicy::DepthPassesOnly) {
		if(SceneLayerContextIsDepthPass(context) && !SceneLayerContextIsDepthPassesOnlyExcluded(context)
			|| SceneLayerContextIsVisiblity(context)) {
			return SceneObjectDrawPolicy::Draw;
		}
	}

	return nullptr == materialName && filterClass == SceneObjectFilterClass::Unknown
		&& SceneLayerContextIsStencil(context)
		? SceneObjectDrawPolicy::Draw
		: policy
	;
}

static bool TryGetSceneSemanticPolicy(SceneObjectFilterClass filterClass, const SceneLayerContext & context, const char* materialName, SceneObjectDrawPolicy& outPolicy, const CBaseSceneData * sceneData) {
	SceneSemanticGroup group = ClassifySceneObject(filterClass, context, materialName, sceneData);
	SceneObjectDrawPolicy policy = g_SceneSemanticPolicies[(int)group];
	// Semantic draw is the default state; descriptor-class policies still apply unless a semantic group is hidden / depth-pass-only.
	if (policy == SceneObjectDrawPolicy::Draw) return false;

	outPolicy = ApplyLayerAwarePolicy(filterClass, context, materialName, policy);
	return true;
}

static const char* GetSceneObjectDescName(const CBaseSceneData& sceneData) {
	if (sceneData.sceneObject == nullptr) return nullptr;

	void* desc = *(void**)((unsigned char*)sceneData.sceneObject + g_SceneObject_pSceneObjectDesc_Offset);
	if (desc == nullptr) return nullptr;

 	// See notes in HookSceneSystem about DebugSceneData for vtabel offset:
	void** vtable = *(void***)desc;
	return ((const char* (__fastcall*)(void*))vtable[0])(desc);
}

/* This doesn't seem quite correct, removing feature for now.
static uint32_t GetSceneDataSort(const CBaseSceneData& sceneData) {
	return *(uint32_t*)((const unsigned char*)&sceneData + 0x58);
}*/

/* This is probably correct, but we don't need it for now.
static uint16_t GetSceneDataFlags(const CBaseSceneData& sceneData) {
	return *(uint16_t*)((const unsigned char*)&sceneData + g_SceneData_Flags_Offset);
}*/

static bool DebugPrintSceneData(const char * pContextTitle, SceneObjectFilterClass filterClass, const SceneLayerContext & context, const CBaseSceneData* sceneData, int count) {
	if (g_iSceneFilterDebug <= 0 && !g_PickerActive || count <= 0) return true;

	bool canReadSceneDataFields = sceneData && SceneObjectFilterClassHasKnownSceneDataLayout(filterClass);

	bool hidden = false;

	for (int i = 0; i < count; ++i) {
		const char* materialName = canReadSceneDataFields ? (sceneData[i].material ? sceneData[i].material->GetName() : nullptr) : nullptr;
		const char* descName = canReadSceneDataFields ? GetSceneObjectDescName(sceneData[i]) : nullptr;
		void* sceneObject = canReadSceneDataFields ? sceneData[i].sceneObject : nullptr;

		bool bInList = false;
		bool bIHidden = false;

		if(g_PickerActive && sceneObject) {
			std::lock_guard<std::mutex> lock(g_PickerMutex);
			// A stop can land between the unlocked test above and this lock.
			if (!g_PickerActive) {}
			else if (!g_PickerCollecting)
			{
				auto itEnt = g_PickerEntities.find(sceneObject);
				bInList = g_PickerEntities.end() != itEnt;
				if(bInList){
					bIHidden = bInList && ((itEnt->second) & 0x1) == 1;
				}
			}
			else
			{
				auto itEnt = g_PickerEntities.lower_bound(sceneObject);
				if (itEnt == g_PickerEntities.end() || (sceneObject < itEnt->first))
				{
					itEnt = g_PickerEntities.emplace_hint(itEnt, std::piecewise_construct, std::forward_as_tuple(sceneObject), std::forward_as_tuple(g_PickerEntities.size()));
				}

				bInList = true;
				bIHidden = ((itEnt->second) & 0x1) == 1;
			}
		}

		hidden = hidden || bIHidden;

		if((!bInList || bIHidden || !g_PickerPrint) && g_iSceneFilterDebug <= 0) continue;

		advancedfx::Message(
			"AFXDEBUG: mirv_scene_filter [%s] %s[%i/%i] layer=%s:%s group=%s desc=%s material=%s sceneObject=0x%p, flags=0x%08x\n",
			pContextTitle,
			SceneObjectFilterClassToString(filterClass),
			i,
			count,
			context.ViewName ? context.ViewName : "?",
			context.ViewPass ? context.ViewPass : "?",
			SceneSemanticGroupToString(ClassifySceneObject(filterClass, context, materialName, sceneData)),
			descName ? descName : "?",
			materialName ? materialName : "?",
			sceneObject,
			context.Flags
		);
	}

	return !hidden;
}

static SceneObjectDrawPolicy GetSceneDataPolicy(SceneObjectFilterClass filterClass, const SceneLayerContext & context, const CBaseSceneData * pSceneData) {

	if (SceneObjectFilterClassIsSkyBox(filterClass)) {
		SceneObjectDrawPolicy policy = ApplyLayerAwarePolicy(filterClass, context, nullptr, g_SceneSemanticPolicies[(int)SceneSemanticGroup::Sky]);
		return policy == SceneObjectDrawPolicy::DepthPassesOnly ? SceneObjectDrawPolicy::Hide : policy;
	}

	if (SceneObjectFilterClassIsProjectedDecal(filterClass)) {
		SceneObjectDrawPolicy policy = g_SceneSemanticPolicies[(int)SceneSemanticGroup::World];
		return policy == SceneObjectDrawPolicy::DepthPassesOnly ? SceneObjectDrawPolicy::Hide : policy;
	}

	if(SceneLayerContextIsCables(context)) {
		SceneObjectDrawPolicy policy = g_SceneSemanticPolicies[(int)SceneSemanticGroup::World];
		return policy == SceneObjectDrawPolicy::DepthPassesOnly ? SceneObjectDrawPolicy::Hide : policy;
	}

	if (!SceneObjectFilterClassHasKnownSceneDataLayout(filterClass) || nullptr == pSceneData || pSceneData->material == nullptr) {
		SceneObjectDrawPolicy semanticPolicy;
		if (TryGetSceneSemanticPolicy(filterClass, context, nullptr, semanticPolicy, pSceneData)) return semanticPolicy;
		return ApplyLayerAwarePolicy(filterClass, context, nullptr, GetSceneObjectClassPolicy(filterClass));
	}

	const char* materialName = pSceneData->material->GetName();
	if (materialName == nullptr) {
		SceneObjectDrawPolicy semanticPolicy;
		if (TryGetSceneSemanticPolicy(filterClass, context, nullptr, semanticPolicy, pSceneData)) return semanticPolicy;
		return ApplyLayerAwarePolicy(filterClass, context, nullptr, GetSceneObjectClassPolicy(filterClass));
	}

	// Custom Sky related:
	//
	// There is some z fight going on, when camera is pointed to sun
	// e.g. clouds draw regardless, so have to hide some sun materials too
	// Don't have much time to fix it properly, this does the job 99% of times
	if (!g_CustomSky.drawClouds) {
		std::string matName = materialName;
		if (matName.find("clouds") != std::string::npos || matName.find("sun_disc_glow") != std::string::npos) {
			return SceneObjectDrawPolicy::Hide;
		}
	}	

	SceneObjectDrawPolicy semanticPolicy;
	if (TryGetSceneSemanticPolicy(filterClass, context, materialName, semanticPolicy, pSceneData)) return semanticPolicy;
	return ApplyLayerAwarePolicy(filterClass, context, materialName, GetSceneObjectClassPolicy(filterClass));
}

std::shared_timed_mutex g_BlockedSoftwareCommandListsMutex;
std::set<void *> g_BlockedSoftwareCommandLists;

void ClearThreadSceneLayerContexts(){
	if(g_bSceneFilterSystemActive) {
		g_PickerPrint = false;
		{
			std::unique_lock<std::shared_timed_mutex> lock(g_BlockedSoftwareCommandListsMutex);
			g_BlockedSoftwareCommandLists.clear();
		}
	}
}

extern bool BlockColorDepth(void * pCRenderContextDx11_SoftwareCommandList, bool bColor, bool bDepth);

typedef void (__fastcall * RenderLayerDrawListPart_t)(void * pSceneSystem, void * param_2, void * pCSceneLayer, unsigned char * pDrawingData, unsigned int count, void *param_6);
RenderLayerDrawListPart_t org_RenderLayerDrawListPart = nullptr;

/*
void __fastcall new_RenderLayerDrawListPart(void * pSceneSystem, void * param_2, void * pCSceneLayer, unsigned char * pDrawingData, unsigned int count, void *param_6) {
	if(g_bSceneFilterSystemActive)
	{
		SceneLayerContext context;
		void* pCSceneView = *(void**)((unsigned char*)pCSceneLayer + 0x6f0);
		if (pCSceneView) {
			void** vtable = *(void***)pCSceneView;
			context.ViewName = ((const char* (__fastcall*)(void*))vtable[0])(pCSceneView);
		}
		context.ViewPass = (const char*)pCSceneLayer + g_SceneLayer_pszViewPass_Offset;
		context.Flags = *(uint32_t*)((unsigned char*)pCSceneLayer + g_SceneLayer_Flags_Offset);
		
		{
			std::unique_lock<std::shared_timed_mutex> lock(g_RenderParam4ToSceneLayerContextsMutex);
			auto result = g_RenderParam4ToSceneLayerContexts.emplace(&(pDrawingData[i]), context);
			if(!result.second) result.first->second = context;
		}
	}

	org_RenderLayerDrawListPart(pSceneSystem, param_2, pCSceneLayer, pDrawingData, count, param_6);
}
*/

void BlockColorDepth(void * pThisSoftwareCommandList){
	bool blocked = false;
	{
		std::unique_lock<std::shared_timed_mutex> lock(g_BlockedSoftwareCommandListsMutex);
		blocked = !g_BlockedSoftwareCommandLists.emplace(pThisSoftwareCommandList).second;
	}
	if(!blocked) BlockColorDepth(pThisSoftwareCommandList,true,true);
}

void CheckAndDo_Untoggle_BlockColorDepth(void * pThisSoftwareCommandList) {
	if(g_bSceneFilterSystemActive) {
		bool blocked;
		{
			std::shared_lock<std::shared_timed_mutex> lock(g_BlockedSoftwareCommandListsMutex);
			blocked = g_BlockedSoftwareCommandLists.find(pThisSoftwareCommandList) != g_BlockedSoftwareCommandLists.end();
		}
		if(blocked) {
			std::unique_lock<std::shared_timed_mutex> lock(g_BlockedSoftwareCommandListsMutex);
			auto it = g_BlockedSoftwareCommandLists.find(pThisSoftwareCommandList);
			blocked = it != g_BlockedSoftwareCommandLists.end();
			if(blocked) g_BlockedSoftwareCommandLists.erase(it);
		}
		if(blocked) {
			BlockColorDepth(pThisSoftwareCommandList,false,false);
		}
	}
}

typedef void * (__fastcall * SoftwareCommandList_Commit_t)(void * pThisSoftwareCommandList);
SoftwareCommandList_Commit_t org_SoftwareCommandList_Commit = nullptr;
void * __fastcall new_SoftwareCommandList_Commit(void * pThisSoftwareCommandList) {
	CheckAndDo_Untoggle_BlockColorDepth(pThisSoftwareCommandList);
	return org_SoftwareCommandList_Commit(pThisSoftwareCommandList);
}

void SetContextFromDrawingData(SceneLayerContext & context, void * pDrawingData) {
	void * pSceneView = ((void**)pDrawingData)[0];
	void * pSceneLayer = ((void**)pDrawingData)[1];
	
	void** pSceneViewVtable = *(void***)pSceneView;
	context.ViewName = ((const char* (__fastcall*)(void*))pSceneViewVtable[0])(pSceneView); // see DebugSceneData function for how to know it's fist function in vtable.

	context.ViewPass = (const char*)pSceneLayer + g_SceneLayer_pszViewPass_Offset;

	context.Flags = *(uint32_t*)((unsigned char*)pSceneLayer + g_SceneLayer_Flags_Offset);	
}

// Since the 2026-09-23 build there is a 5th (stack) argument: optional name suffix, formatted as "/%s" when not null.
// It must be forwarded, otherwise the original formats garbage from our stack frame and crashes in tier0.
typedef void (__fastcall * InitDrawingData_t)(unsigned char * pDrawingData,void *pSceneView,void *pSceneLayer,uint32_t unkFlags4,const char *pszNameSuffix);
InitDrawingData_t org_InitDrawingData = nullptr;
void __fastcall new_InitDrawingData(unsigned char * pDrawingData,void *pSceneView,void *pSceneLayer,uint32_t unkFlags4,const char *pszNameSuffix) {
	org_InitDrawingData(pDrawingData,pSceneView,pSceneLayer,unkFlags4,pszNameSuffix);

	if(g_bSceneFilterSystemActive && pDrawingData) {
		void * pCRenderContextDx11_SoftwareCommandList = ((void **)pDrawingData)[4];
		CheckAndDo_Untoggle_BlockColorDepth(pCRenderContextDx11_SoftwareCommandList);
		if(org_SoftwareCommandList_Commit == nullptr) {
			void** vtable = *(void***)pCRenderContextDx11_SoftwareCommandList;
			org_SoftwareCommandList_Commit = (SoftwareCommandList_Commit_t)vtable[11];

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());

			DetourAttach(&(PVOID&)org_SoftwareCommandList_Commit, new_SoftwareCommandList_Commit);

			if(NO_ERROR != DetourTransactionCommit()) {
				ErrorBox("Failed to detour SoftwareCommandList::Commit.");
				return;
			}				
		}

		/*void** pSceneViewVtable = *(void***)pSceneView;
		SceneLayerContext context;
		context.ViewName = ((const char* (__fastcall*)(void*))pSceneViewVtable[0])(pSceneView); // see DebugSceneData function for how to know it's fist function in vtable.
		context.ViewPass = (const char*)pSceneLayer + g_SceneLayer_pszViewPass_Offset;
		context.Flags = *(uint32_t*)((unsigned char*)pSceneLayer + g_SceneLayer_Flags_Offset);*/
		/*{
			std::unique_lock<std::shared_timed_mutex> lock(g_RenderParam4ToSceneLayerContextsMutex);
			auto result = g_RenderParam4ToSceneLayerContexts.emplace(pDrawingData, context);
			if(!result.second) result.first->second = context;
		}*/

		SceneLayerContext context;
		SetContextFromDrawingData(context, pDrawingData);

		if(0 < g_iSceneFilterDebug) {
			advancedfx::Message("AFXDEBUG: InitDrawingData layer=%s:%s flags=0x%08x unkFlags4=0x%08x\n",
				context.ViewName ? context.ViewName : "?",
				context.ViewPass ? context.ViewPass : "?",
				context.Flags,
				unkFlags4
			);
		}

		if (0 == strcmp(context.ViewPass, "CSGOCrosshair")) {
			SceneObjectDrawPolicy policy = g_SceneSemanticPolicies[(int)SceneSemanticGroup::Xhair];
			if (policy == SceneObjectDrawPolicy::Hide) {
				BlockColorDepth(pCRenderContextDx11_SoftwareCommandList);
			}
		}

		if(g_OverlaysPolicy != SceneObjectDrawPolicy::Draw && context.ViewPass && (
			0 == strcmp(context.ViewPass,"OverlaySmoke") // We want to let through "OverlaySmokeUpdate"
			|| StringContains(context.ViewPass,"FlashbangOverlay")
			|| StringContains(context.ViewPass,"Fade To Black")
			)
		) {	
			BlockColorDepth(pCRenderContextDx11_SoftwareCommandList);
		}
	}
}

typedef void (__fastcall * DrawSceneData_t)(void * pDrawingData, CBaseSceneData * pSceneData);
DrawSceneData_t org_DrawSceneData = nullptr;

typedef void (__fastcall * DrawCurrentPrimitives_t)(void * pDrawingData);
DrawCurrentPrimitives_t org_DrawCurrentPrimitives = nullptr;

void __fastcall new_DrawSceneData(void * pDrawingData, CBaseSceneData* pSceneData) {

	if(g_bSceneFilterSystemActive && nullptr != pDrawingData) {
		SceneLayerContext context;
		SetContextFromDrawingData(context, pDrawingData);

		SceneObjectFilterClass filterClass = SceneObjectFilterClass::Unknown;
		if(pSceneData != nullptr && pSceneData->sceneObject) {
			if(void* desc = *(void**)((unsigned char*)pSceneData->sceneObject + g_SceneObject_pSceneObjectDesc_Offset)) {
				void** vtable = *(void***)desc;
				auto it = g_VtableToSceneObjectFilterClass.find(vtable);
				if(it != g_VtableToSceneObjectFilterClass.end())
					filterClass = it->second;
			}
		}
		
		SceneObjectDrawPolicy policy;
		if(DebugPrintSceneData("DrawSceneData", filterClass, context, pSceneData, 1)) {
			policy = GetSceneDataPolicy(filterClass, context, pSceneData);
		} else {
			policy = SceneObjectDrawPolicy::Hide;
		}

		void * pCRenderContextDx11_SoftwareCommandList = ((void **)pDrawingData)[4];
		switch (policy) {
		default:
		case SceneObjectDrawPolicy::Draw:
			break;
		case SceneObjectDrawPolicy::DepthPassesOnly:
			BlockColorDepth(pCRenderContextDx11_SoftwareCommandList, true, false);
			break;
		case SceneObjectDrawPolicy::Hide:
			BlockColorDepth(pCRenderContextDx11_SoftwareCommandList, true, true);
			break;
		}
		org_DrawSceneData(pDrawingData, pSceneData);
		org_DrawCurrentPrimitives(pDrawingData); // Force draw to prevent merging and get correct state.
		switch (policy) {
		default:
		case SceneObjectDrawPolicy::Draw:
			break;
		case SceneObjectDrawPolicy::DepthPassesOnly:
		case SceneObjectDrawPolicy::Hide:
			BlockColorDepth(pCRenderContextDx11_SoftwareCommandList, false, false);
			break;
		}
		return;
	}

	org_DrawSceneData(pDrawingData, pSceneData);
}

/*
void __fastcall new_DrawCurrentPrimitives(void * pDrawingData) {
	if(g_bSceneFilterSystemActive && nullptr != pDrawingData) {
		SceneLayerContext context;
		SetContextFromDrawingData(context, pDrawingData);

		SceneObjectFilterClass filterClass = SceneObjectFilterClass::Unknown;	
			
		SceneObjectDrawPolicy policy;
		if(DebugPrintSceneData("DrawCurrentPrimitives", filterClass, context, nullptr, 1)) {
			policy = GetSceneDataPolicy(filterClass, context, nullptr);
		} else {
			policy = SceneObjectDrawPolicy::Hide;
		}

		switch (policy) {
		default:
		case SceneObjectDrawPolicy::Draw:
			break;
		case SceneObjectDrawPolicy::DepthPassesOnly:
		case SceneObjectDrawPolicy::Hide:
			{
				void * pCRenderContextDx11_SoftwareCommandList = ((void **)pDrawingData)[4];
				ToggleDrawing(pCRenderContextDx11_SoftwareCommandList, true);
				org_DrawCurrentPrimitives(pDrawingData);
				ToggleDrawing(pCRenderContextDx11_SoftwareCommandList, false);
			}
			return;
		}
	}

	org_DrawCurrentPrimitives(pDrawingData);
}
*/

struct FloatColor {
	float r = 0;
	float g = 0;
	float b = 0;
	float a = 0;
};

std::map<std::pair<CMaterial2*, std::string>, FloatColor> defaultCloudColors;

void updateCloudMaterials();
void resetDefaultCloudColors() {
    const bool wanted = g_CustomSky.colorClouds.use;
    g_CustomSky.colorClouds.use = false;
    if (!defaultCloudColors.empty()) updateCloudMaterials();
    defaultCloudColors.clear();
    g_CustomSky.colorClouds.use = wanted;
}

// SEH-guarded engine calls. During a level load the material array contains half-constructed
// objects whose internal fields are garbage (-1, 0x16, ...); calling materialsystem2 on one
// faults INSIDE the engine (crash.veh 2026-08-07: two demo-load crashes at
// materialsystem2+0x9e1af from different reset commands). A pointer check cannot see that, so
// the engine call itself is wrapped: a faulting material is treated as "no such param" and
// skipped. These helpers hold no C++ objects (SEH requirement).
namespace {

float* GuardedFindParamRaw(CMaterial2* material, const char* paramName) {
	__try {
		return (float*)org_MaterialFindParam(material, paramName);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool GuardedUpdateRaw(CMaterial2* material) {
	__try {
		org_MaterialUpdate(material);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool GuardedNameRaw(CMaterial2* material, char* buf, size_t bufLen) {
	__try {
		const char* n = material->GetName();
		if (!n) return false;
		strncpy_s(buf, bufLen, n, _TRUNCATE);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

} // namespace

namespace {

CMaterial2** GuardedFindMaterialRaw(const char* name) {
	__try {
		CMaterial2** handle = nullptr;
		org_FindMaterial(nullptr, &handle, name);
		return handle;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

} // namespace

CMaterial2* SceneSystem_ResolveMaterial(const char* name) {
	if (!name || !name[0] || !org_FindMaterial || !g_pCResourceSystem) return nullptr;
	// Blocking precache first (hard rule: substitutes must be resident before any draw uses
	// them), then resolve the live handle. Same sequence the skybox swap uses.
	if (cachedMaterials.find(name) == cachedMaterials.end()) {
		if (0 != g_pCResourceSystem->PreCache(name)) cachedMaterials.insert({name, true});
	}
	CMaterial2** handle = GuardedFindMaterialRaw(name);
	if (!handle || (uintptr_t)handle == (uintptr_t)-1) return nullptr;
	// Pin: bump the handle's strong count exactly like new_ForceUpdateSkybox does, so a level
	// transition cannot free the material while a render job still swaps draws to it.
	*(uint32_t*)((u_char*)handle + 0x20) += 1;
	CMaterial2* material = *handle;
	return ((uintptr_t)material == (uintptr_t)-1) ? nullptr : material;
}

void SceneSystem_SetPlayersOnlyGreenPolicy(bool active) {
	const bool was = g_playersOnlyGreenPolicy.exchange(active, std::memory_order_acq_rel);
	g_playersOnlyGreenCloudsSuppressed.store(active, std::memory_order_release);
	if (was != active && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("render.playersonlygreen",
			"scenesystem policy=%d", active ? 1 : 0);
}

bool SceneSystem_PlayersOnlyGreenPolicyActive() {
	return g_playersOnlyGreenPolicy.load(std::memory_order_acquire);
}

void SceneSystem_SetPlayersOnlyGreenSkyOverride(bool active) {
	// Main-thread-only, like every other entity/material pump. Reconciled every frame by the
	// green module, so the equal-state early-out is what keeps this one bool compare per frame.
	if (g_playersOnlyGreenSkyOverride == active) return;
	g_playersOnlyGreenSkyOverride = active;
	// One-shot apply/restore through the shared skybox updater (precache + material write). The
	// ForceUpdateSkybox detour re-applies the override on every engine-initiated sky rebuild, so
	// no per-frame reassert is needed.
	updateSkyboxEntities();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("render.playersonlygreen.sky",
			"override=%d material='%s'", active ? 1 : 0, kPlayersOnlyGreenSkyMaterial);
}

void SceneSystem_BeginPlayersOnlyGreenPass() {
	// A cancelled pass or a map transition can leave a registry from an earlier command stream.
	// Flush it before accepting a new pass so a stale slot can never be restored with a new level's
	// material pointer.
	SceneSystem_RestorePlayersOnlyGreenMaterials();
}

void SceneSystem_RestorePlayersOnlyGreenMaterials() {
	CMaterial2* green = Filmmaker::RenderPlayersOnlyGreen_Material();
	std::unordered_map<void*, CMaterial2*> pending;
	std::unordered_map<void*, unsigned int> pendingColors;
	{
		std::lock_guard<std::mutex> lock(g_playersOnlyGreenRestoreMutex);
		pending.swap(g_playersOnlyGreenPendingRestores);
		pendingColors.swap(g_playersOnlyGreenPendingColorRestores);
	}
	// Colour slots live in the same engine-owned batch buffers as the material slots;
	// compare-before-restore so a reused buffer never gets a stale colour written back.
	for (const auto& entry : pendingColors)
		GuardedRestorePlayersOnlyGreenColor(
			reinterpret_cast<unsigned int*>(entry.first), entry.second);
	if (!green || pending.empty()) return;
	CMaterial2* invisible = Filmmaker::RenderPlayersOnlyGreen_InvisibleMaterial();
	CMaterial2* skipChroma = Filmmaker::RenderPlayersOnlyGreen_SkipMaterial();
	unsigned restored = 0;
	for (const auto& entry : pending) {
		CMaterial2** slot = reinterpret_cast<CMaterial2**>(entry.first);
		CMaterial2* current = nullptr;
		if (ProbeReadPointerAt(slot, 0, (void**)&current)
			&& (current == green || (invisible && current == invisible))
			&& ProbeWritePointerAt(slot, 0, entry.second))
			++restored;
	}
	if (restored && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("render.playersonlygreen",
			"deferred material restore slots=%u pending=%u", restored,
			(unsigned)pending.size());
}

float* SceneSystem_FindMaterialParam(CMaterial2* material, const char* paramName) {
	if (!material || (uintptr_t)material == (uintptr_t)-1 || !paramName || !org_MaterialFindParam)
		return nullptr;
	return GuardedFindParamRaw(material, paramName);
}

void SceneSystem_UpdateMaterial(CMaterial2* material) {
	if (!material || (uintptr_t)material == (uintptr_t)-1 || !org_MaterialUpdate) return;
	GuardedUpdateRaw(material);
}

bool SceneSystem_MaterialName(CMaterial2* material, char* buf, size_t bufLen) {
	if (!material || (uintptr_t)material == (uintptr_t)-1 || !buf || !bufLen) return false;
	buf[0] = 0;
	return GuardedNameRaw(material, buf, bufLen);
}

void updateCloudMaterials() {
	if (!g_pCResourceSystem) return;
	GetMaterialsArrayResult materials = {0};
	g_pCResourceSystem->GetMaterials(&materials);

	for (int i = 0; i < materials.count; i++) {
		// During a level load this array is walked while the resource system is still growing
		// it; freed/pending slots carry a -1 sentinel instead of a pointer. Dereferencing one
		// was a real crash (ACCESS_VIOLATION reading 0xFFFFFFFFFFFFFFFF in materialsystem2,
		// caught by crash.veh 2026-08-07 during the demo-start effects reset).
		auto ppMat = materials.pArrMaterials[i];
		if (!ppMat || (uintptr_t)ppMat == (uintptr_t)-1) continue;
		auto pMat = *ppMat;
		if (!pMat || (uintptr_t)pMat == (uintptr_t)-1) continue;
		char nameBuf[512];
		if (!SceneSystem_MaterialName(pMat, nameBuf, sizeof(nameBuf)) || !nameBuf[0]) continue;
		std::string matName = nameBuf;
		if (matName.find("clouds") != -1) {
			auto colorParam = SceneSystem_FindMaterialParam(pMat, "g_vColorTint");
			if (0 != colorParam) {
                const auto colorKey = std::make_pair(pMat, matName);
                if (!Filmmaker::WorldTransient::Number("cloudTint",g_CustomSky.colorClouds.use) && defaultCloudColors.find(colorKey) == defaultCloudColors.end()) continue;
				if (defaultCloudColors.find(colorKey) == defaultCloudColors.end()) {
					defaultCloudColors[colorKey] = FloatColor { colorParam[0], colorParam[1], colorParam[2], colorParam[3] };
				}

				if (Filmmaker::WorldTransient::Number("cloudTint",g_CustomSky.colorClouds.use)) {
					colorParam[0] = float(Filmmaker::WorldTransient::Number("cloudColor",g_CustomSky.colorClouds.value.r,0)) / 255.0f;
					colorParam[1] = float(Filmmaker::WorldTransient::Number("cloudColor",g_CustomSky.colorClouds.value.g,1)) / 255.0f;
					colorParam[2] = float(Filmmaker::WorldTransient::Number("cloudColor",g_CustomSky.colorClouds.value.b,2)) / 255.0f;
					colorParam[3] = 1.0f; // this doesnt seem to do anything
				} else if (defaultCloudColors.find(colorKey) != defaultCloudColors.end()) {
					auto defaultColor = defaultCloudColors[colorKey];
					colorParam[0] = defaultColor.r;
					colorParam[1] = defaultColor.g;
					colorParam[2] = defaultColor.b;
					colorParam[3] = defaultColor.a;
                    defaultCloudColors.erase(colorKey);
				}

				SceneSystem_UpdateMaterial(pMat);
			}
		}
	}

}

bool SceneSystem_GetSkyColor(bool clouds,int rgba[4]) {
    const auto& color=clouds?g_CustomSky.colorClouds:g_CustomSky.color;
    rgba[0]=color.value.r;rgba[1]=color.value.g;rgba[2]=color.value.b;
    rgba[3]=clouds?255:(int)std::lround(g_CustomSky.brightness*255);
    if (!color.use) for(int i=0;i<4;++i) rgba[i]=255;
    return color.use;
}
void SceneSystem_SetSkyColor(bool clouds,bool enabled,const int rgba[4]) {
    { std::lock_guard<std::mutex> lock(g_skyOriginalsMutex);
    auto& color=clouds?g_CustomSky.colorClouds:g_CustomSky.color;
    if(enabled) {
        advancedfx::CFakeCommandArgs args("color");
        for(int i=0;i<3;++i) args.AddArg(std::to_string(rgba[i]).c_str());
        color.setColor(&args);
    }
    color.use=enabled;
    if(!clouds)g_CustomSky.brightness=(std::max)(.000001f,rgba[3]/255.f);
    }
    if(clouds) updateCloudMaterials(); else updateSkyboxEntities();
}
bool SceneSystem_CloudsVisible() {return g_CustomSky.drawClouds;}
void SceneSystem_SetCloudsVisible(bool visible) {g_CustomSky.drawClouds=visible;}
void SceneSystem_SetSkyMaterial(const char* material) {
    {std::lock_guard<std::mutex> lock(g_skyOriginalsMutex);g_CustomSky.currentSkyName = material ? material : "";}
    updateSkyboxEntities();
}

CON_COMMAND(mirv_sky, "")
{
	auto argc = args->ArgC();
	auto arg0 = args->ArgV(0);

	if(2 <= argc)
	{
		auto arg1 = args->ArgV(1);
		if(!_stricmp(arg1,"material"))
		{
			if (3 <= argc)
			{
				auto arg2 = args->ArgV(2);

				if (!_stricmp(arg2, "default")) {
					SceneSystem_SetSkyMaterial("");
					return;
				}

				std::string filePath = arg2;
				if (StringEndsWith(arg2, "_c")) {
					filePath.erase(filePath.size() - 2);
				}
				SceneSystem_SetSkyMaterial(filePath.c_str());
				return;
			}

			advancedfx::Message(
				"Usage:\n"
				"%s %s  <sRelativePathToFile> - Set skybox material. It has to be re-applied if map changes.\n"
				"Current value: %s\n",
				arg0, arg1,
				g_CustomSky.currentSkyName.size() == 0 ? "default" : g_CustomSky.currentSkyName.c_str()
			);
			return;
		}

		if(!_stricmp(arg1, "color")) {
			if (3 <= argc && !_stricmp(args->ArgV(2), "default"))
			{
				{ std::lock_guard<std::mutex> lock(g_skyOriginalsMutex); g_CustomSky.color.use = false; }
				updateSkyboxEntities();
				return;
			}

			if (6 <= argc)
			{
				advancedfx::CSubCommandArgs subArgs(args, 2);
                bool changed = false;
                { std::lock_guard<std::mutex> lock(g_skyOriginalsMutex);
				if (g_CustomSky.color.setColor(&subArgs)) {
					g_CustomSky.brightness = atoi(args->ArgV(5)) / 255.0;
					if (g_CustomSky.brightness < 0.000001f) g_CustomSky.brightness = 0.000001f;
                    changed = true;
				}
                }
                if(changed) updateSkyboxEntities();

				return;
			}

			advancedfx::Message(
				"Usage:\n"
				"%s %s <iR> <iG> <iB> <iA> | default - Set color override in RGBA format, values from 0 to 255.\n"
				"Current value: %s\n",
				arg0, arg1,
				g_CustomSky.color.use ? g_CustomSky.color.userValue.c_str() : "default"
			);
			return;
		}

		if(!_stricmp(arg1, "clouds")) {
			if (3 <= argc)
			{
				auto arg2 = args->ArgV(2);

				if (!_stricmp(arg2, "color")) {
					if (4 <= argc && !_stricmp(args->ArgV(3), "default"))
					{
						g_CustomSky.colorClouds.use = false;
						updateCloudMaterials();
						return;
					}

					if (6 == argc)
					{
						advancedfx::CSubCommandArgs subArgs(args, 3);
						if (g_CustomSky.colorClouds.setColor(&subArgs)) {
							updateCloudMaterials();
							return;
						}

					}

					advancedfx::Message(
						"Usage:\n"
						"%s %s %s <iR> <iG> <iB> | default - Set color override. RGB format, values from 0 to 255. It has to be re-applied if map changes.\n"
						"Current value: %s\n"
						, arg0, arg1, arg2
						, g_CustomSky.colorClouds.use ? g_CustomSky.colorClouds.userValue.c_str() : "default"
					);
					return;
				}

				if (!_stricmp(arg2, "draw")) {
					if (4 <= argc)
					{
						g_CustomSky.drawClouds = 0 != atoi(args->ArgV(3));
						return;
					}

					advancedfx::Message(
						"Usage:\n"
						"%s %s %s 0|1 - Enable (1) / disable (0) drawing of clouds.\n"
						"Current value: %s\n"
						, arg0, arg1, arg2
						, g_CustomSky.drawClouds ? "1" : "0"
					);
					return;
				}

			}
			advancedfx::Message(
				"Usage:\n"
				"%s %s color <iR> <iG> <iB> | default - Set color override. RGB format, values from 0 to 255. It has to be re-applied if map changes.\n"
				"%s %s draw 0|1 - Enable (1) / disable (0) drawing of clouds.\n"
				, arg0, arg1
				, arg0, arg1
			);
			return;
		}

		if (!_stricmp(arg1, "sun")) {
			if (3 <= argc && !_stricmp(args->ArgV(2), "draw")) {
				if (4 <= argc) {
					g_CustomSky.drawSun = 0 != atoi(args->ArgV(3));
					// Keep the legacy command on the same source of truth as the Filmmaker Sun control.
					// The draw hook no longer culls by this flag, which avoids the old cloud/zenith glitch.
					Filmmaker::SunFx_SetVisible(g_CustomSky.drawSun);
					return;
				}
				advancedfx::Message(
					"Usage:\n"
					"%s %s draw 0|1 - Enable (1) / disable (0) the independent sun glow sprite.\n"
					"Current value: %s\n", arg0, arg1, g_CustomSky.drawSun ? "1" : "0");
				return;
			}
			advancedfx::Message("Usage:\n%s %s draw 0|1 - Enable (1) / disable (0) the independent sun glow sprite.\n",
				arg0, arg1);
			return;
		}

	}

	advancedfx::Message(
		"Usage:\n"
		"%s color <iR> <iG> <iB> <iA> | default - Set color override. RGBA format, values from 0 to 255.\n"
		"%s material <sRelativePathToFile> | default - Set skybox material. It has to be re-applied if map changes.\n"
		"%s clouds [...] - Control clouds.\n"
		"%s sun draw 0|1 - Control the sun glow independently from clouds.\n"
		"Note: see wiki on GitHub for this command for details.\n"
		, arg0
		, arg0
		, arg0
		, arg0
	);
}

CON_COMMAND(__mirv_scene_filter, "")
{
	auto argc = args->ArgC();
	auto arg0 = args->ArgV(0);

	if (3 <= argc)
	{
		auto arg1 = args->ArgV(1);
		auto arg2 = args->ArgV(2);
		SceneObjectDrawPolicy policy;
		SceneSemanticGroup semanticGroup;

		if (!_stricmp(arg1, "base"))
		{
			if (TryParseSceneObjectDrawPolicy(arg2, policy))
			{
				g_BaseSceneObjectsPolicy = policy;
				UpdateSceneFilterSystemActive();
				return;
			}
		}

		if (!_stricmp(arg1, "animatable"))
		{
			if (TryParseSceneObjectDrawPolicy(arg2, policy))
			{
				g_AnimatableSceneObjectsPolicy = policy;
				UpdateSceneFilterSystemActive();
				return;
			}
		}

		if (!_stricmp(arg1, "aggregate"))
		{
			if (TryParseSceneObjectDrawPolicy(arg2, policy))
			{
				g_AggregateSceneObjectsPolicy = policy;
				UpdateSceneFilterSystemActive();
				return;
			}
		}

		if (!_stricmp(arg1, "overlays"))
		{
			if (TryParseSceneObjectDrawPolicy(arg2, policy))
			{
				g_OverlaysPolicy = policy == SceneObjectDrawPolicy::Hide
					? SceneObjectDrawPolicy::Hide
					: SceneObjectDrawPolicy::Draw;
				UpdateSceneFilterSystemActive();
				return;
			}
		}		

		if (TryGetSceneSemanticGroup(arg1, semanticGroup))
		{
			if (TryParseSceneObjectDrawPolicy(arg2, policy))
			{
				g_SceneSemanticPolicies[(int)semanticGroup] = policy;
				UpdateSceneFilterSystemActive();
				return;
			}
		}

		if (!_stricmp(arg1, "debug"))
		{
			g_iSceneFilterDebug = atoi(arg2);
			if (g_iSceneFilterDebug < 0) g_iSceneFilterDebug = 0;
			UpdateSceneFilterSystemActive();
			return;
		}

		if (!_stricmp(arg1, "picker"))
		{
			if (!_stricmp(arg2, "ent") && 4 <= argc)
			{
				//curBaseFx->Console_DisableFastPathRequired();

				bool value = 0 != atoi(args->ArgV(3));
				Picker_Pick(value);
				UpdateSceneFilterSystemActive();
				return;
			}
			else
			if (!_stricmp(arg2, "stop"))
			{
				Picker_Stop();
				UpdateSceneFilterSystemActive();
				return;
			}
		}		
	}

	advancedfx::Message(
		"Usage:\n"
		"%s base draw|hide|zonly|0|1 - Controls CBaseSceneObjectDesc entries in the hooked scene draw path.\n"
		"%s animatable draw|hide|zonly|0|1 - Controls CAnimatableSceneObjectDesc entries in the hooked scene draw path.\n"
		"%s aggregate draw|hide|zonly|0|1 - Controls CAggregateSceneObjectDesc entries in the hooked scene draw path.\n"
		"%s smoke draw|hide|0|1 - Controls CSmokeVolumeObjectDesc by patching its vtable while hidden.\n"
		"%s overlays draw|hide|0|1 - Controls overlays pass.\n"
		"%s viewModel draw|hide|zonly - Controls viewmodel layers.\n"
		"%s particles draw|hide|zonly - Controls viewmodel effect layers.\n"
		"%s firstPersonLegs draw|hide|zonly - Controls first person legs layers.\n"
		"%s weapons draw|hide|zonly - Controls weapons layers.\n"
		"%s players draw|hide|zonly - Controls player character layers.\n"
		"%s world draw|hide|zonly - Controls layers not matched by another semantic group.\n"
		"%s sky draw|hide|zonly - Controls 3D skybox layers matched in the scene draw path.\n"
		"%s debug <iCount> - Print up to iCount entries for each hooked scene draw call. Use 0 to disable.\n"
		"%s picker ent 0|1 - Tell picker if entity is visible (1) or not (0). (Or start picking with 1.)\n"
		"%s picker stop - Stop picking.\n"
		"Current values: base=%s animatable=%s aggregate=%s smoke=%s overlays=%s viewModel=%s particles=%s firstPersonLegs=%s players=%s world=%s sky=%s debug=%i\n"
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, arg0
		, SceneObjectDrawPolicyToString(g_BaseSceneObjectsPolicy)
		, SceneObjectDrawPolicyToString(g_AnimatableSceneObjectsPolicy)
		, SceneObjectDrawPolicyToString(g_AggregateSceneObjectsPolicy)
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::Smoke])
		, SceneObjectDrawPolicyToString(g_OverlaysPolicy)
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::ViewModel])
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::Particles])
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::FirstPersonLegs])
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::Players])
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::World])
		, SceneObjectDrawPolicyToString(g_SceneSemanticPolicies[(int)SceneSemanticGroup::Sky])
		, g_iSceneFilterDebug.load()
	);
}

void HookMaterialSystem(HMODULE materialSystemDll) {
	// 1 xref, called in the middle of 23 function of vtable for CMaterialSystem2
	//
	// LAB_18003d0a1                                   XREF[1]:     18003d077 (j)   
    // 18003d0a1 49  8b  d4       MOV        RDX ,R12
    // 18003d0a4 48  8b  cd       MOV        RCX ,RBP
    // 18003d0a7 e8  84  4d       CALL       FUN_180011e30 <------
    //           fd  ff
    // 18003d0ac 4c  8b  e8       MOV        R13 ,RAX
    // 18003d0af 4c  8b  fd       MOV        R15 ,RBP
    // 18003d0b2 48  85  c0       TEST       RAX ,RAX
    // 18003d0b5 0f  84  16       JZ         LAB_18003d1d1
    //           01  00  00
    // 18003d0bb 0f  10  44       MOVUPS     XMM0 ,xmmword ptr [RSP  + local_38[0] ]
    //           24  30
    // 18003d0c0 c6  40  38  00    MOV        byte ptr [RAX  + 0x38 ],0x0
    // 18003d0c4 0f  11  00       MOVUPS     xmmword ptr [RAX ],XMM0
    // 18003d0c7 8b  86  68       MOV        EAX ,dword ptr [RSI  + 0x568 ]
    //           05  00  00
    // 18003d0cd 89  44  24  78    MOV        dword ptr [RSP  + local_res10 ],EAX
    // 18003d0d1 3b  86  78       CMP        EAX ,dword ptr [RSI  + 0x578 ]
    //           05  00  00
    //
	if (auto addr = getAddress(materialSystemDll, "E8 ?? ?? ?? ?? 4C 8B E8 4C 8B FD 48 85 C0")) {
		org_MaterialFindParam = (MaterialFindParam_t)(addr + 5 + *(int32_t*)(addr + 1));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));
	
	// function call in 30 function of vtable for CMaterial
	if (auto addr = getAddress(materialSystemDll, "f6 41 ?? 04 ?? ?? e8 ?? ?? ?? ?? 80 63 ?? fb")) {
		org_MaterialUpdate = (MaterialUpdate_t)(addr + 11 + *(int32_t*)(addr + 7));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));

	// calls 80th function for CResourceSystem, and has 0x74616d76
	// The 80th for CResourceSystem has following strings:
	// "WARNING: %s resource '%s' (%016llX) requested but is not in the system. (Missing from from a manifest?)\n"
	// "ERROR: %s resource '%s' (%016llX) requested is not loaded and may have been deleted.\n"
	org_FindMaterial = (FindMaterial_t)getVTableFn(materialSystemDll, 14, ".?AVCMaterialSystem2@@");
	if (0 == org_FindMaterial) ErrorBox(MkErrStr(__FILE__, __LINE__));
}

void SceneSystem_ReportDrawCoverage() {
	HMODULE sceneSystemDll = GetModuleHandleA("scenesystem.dll");
	advancedfx::Message("scenesystem drawable-batch coverage (scenesystem.dll=0x%p):\n",
		(void*)sceneSystemDll);
	int covered = 0, missing = 0;
	for (size_t i = 0; i < kDescCoverageCount; ++i) {
		const DescCoverage& d = g_DescCoverage[i];
		if (!d.vtable) {
			advancedfx::Message("   %-44s vtable NOT FOUND\n", d.rtti);
			continue;
		}
		const uintptr_t rva = sceneSystemDll && d.draw
			? (uintptr_t)d.draw - (uintptr_t)sceneSystemDll : 0;
		advancedfx::Message("   %-44s vtable=0x%p draw=0x%p (+0x%llx) %s\n",
			d.rtti, (void*)d.vtable, d.draw, (unsigned long long)rva,
			d.covered ? "HOOKED" : "not hooked");
		if (d.covered) ++covered; else ++missing;
	}
	advancedfx::Message("   => %d hooked, %d NOT hooked. Draws submitted through an unhooked"
		" descriptor cannot be rejected by any subject filter.\n", covered, missing);
}

void GetSceneObjectVtable(HMODULE dll, const char * mangledClass, SceneObjectFilterClass filterClass) {
	if (void ** vtable = (void **)Afx::BinUtils::FindClassVtable(dll, mangledClass, 0, 0)) {
		// vtable[0] - GetDesc()
		// vtable[1] - DrawArray()
		// vtable[3] - DrawSingle()
		g_VtableToSceneObjectFilterClass[vtable] = filterClass;
	} else {
		ErrorBox(MkErrStr(__FILE__, __LINE__));
		ErrorBox(mangledClass);
	}
}

void GetClientDllSceneObjectVtable(HMODULE clientDll) {
	GetSceneObjectVtable(clientDll, ".?AVCSmokeVolumeSceneObjectDesc@@", SceneObjectFilterClass::SmokeVolume);
}

void GetParticlesDllSceneObjectVtable(HMODULE particlesDll) {
	GetSceneObjectVtable(particlesDll, ".?AVCParticleObjectDesc@@", SceneObjectFilterClass::Particles);
}

void HookSceneSystem(HMODULE sceneSystemDll) {
	// Resolve the aggregate descriptor BEFORE the table so the table can step around it. The
	// aggregate keeps its own detour (it also carries the world-modulation/tint write), and the
	// RT-proxy variant added to the table may well share that same Draw implementation. Detouring
	// one function twice is not a duplicate hook, it is a corrupted trampoline.
	g_AggregateVtable = (void**)Afx::BinUtils::FindClassVtable(
		sceneSystemDll, ".?AVCAggregateSceneObjectDesc@@", 0, 0);
	DrawBaseSceneObject_t aggregateDraw =
		g_AggregateVtable ? (DrawBaseSceneObject_t)g_AggregateVtable[1] : nullptr;

	// Snapshot every descriptor's slot-1 pointer BEFORE any DetourAttach rewrites the variables it
	// is compared against. Read-only; see SceneSystem_ReportDrawCoverage.
	for (size_t i = 0; i < kDescCoverageCount; ++i) {
		g_DescCoverage[i].vtable =
			(void**)Afx::BinUtils::FindClassVtable(sceneSystemDll, g_DescCoverage[i].rtti, 0, 0);
		g_DescCoverage[i].draw = g_DescCoverage[i].vtable ? g_DescCoverage[i].vtable[1] : nullptr;
		g_DescCoverage[i].covered =
			g_DescCoverage[i].draw && aggregateDraw && g_DescCoverage[i].draw == (void*)aggregateDraw;
	}

	const size_t hookCount = sizeof(g_DrawDescriptorHooks) / sizeof(g_DrawDescriptorHooks[0]);
	for (size_t i = 0; i < hookCount; ++i) {
		DrawDescriptorHook& hook = g_DrawDescriptorHooks[i];
		hook.vtable = (void**)Afx::BinUtils::FindClassVtable(sceneSystemDll, hook.rtti, 0, 0);
		if (!hook.vtable) continue;
		hook.original = (DrawBaseSceneObject_t)hook.vtable[1];
		// Already covered, with its own filter, by new_DrawAggregateSceneObject.
		if (aggregateDraw && hook.original == aggregateDraw) {
			// Clear the vtable too: an entry that is never detoured must also never be a lookup
			// answer in FindDrawDescriptorOriginal, or a future change could route it to entry 0's
			// unrelated Draw.
			hook.vtable = nullptr;
			hook.original = nullptr;
			continue;
		}
		hook.owner = i;
		for (size_t previous = 0; previous < i; ++previous) {
			if (g_DrawDescriptorHooks[previous].original == hook.original) {
				hook.owner = g_DrawDescriptorHooks[previous].owner;
				break;
			}
		}
		hook.attach = hook.owner == i;
		for (size_t c = 0; c < kDescCoverageCount; ++c)
			if (g_DescCoverage[c].draw && g_DescCoverage[c].draw == (void*)hook.original)
				g_DescCoverage[c].covered = true;
	}
	if (!g_DrawDescriptorHooks[0].original) ErrorBox(MkErrStr(__FILE__, __LINE__));

	DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

	for (size_t i = 0; i < hookCount; ++i) {
		if (g_DrawDescriptorHooks[i].attach)
			DetourAttach(&(PVOID&)g_DrawDescriptorHooks[i].original, new_DrawBaseSceneObject);
	}

	if(NO_ERROR != DetourTransactionCommit()) {
		ErrorBox("Failed to detour SceneSystem functions.");
		return;
	}

	// mirv_streams scene filter (upstream advancedfx): its own detours, after ours.

	GetSceneObjectVtable(sceneSystemDll, ".?AVCBaseSceneObjectDesc@@", SceneObjectFilterClass::Base);
	GetSceneObjectVtable(sceneSystemDll, ".?AVCAnimatableSceneObjectDesc@@", SceneObjectFilterClass::Animatable);
	GetSceneObjectVtable(sceneSystemDll, ".?AVCAggregateSceneObjectDesc@@", SceneObjectFilterClass::Aggregate);
	GetSceneObjectVtable(sceneSystemDll, ".?AVCSkyBoxObjectDesc@@", SceneObjectFilterClass::SkyBox);
	GetSceneObjectVtable(sceneSystemDll, ".?AVC6FaceSkyboxObjectDesc@@", SceneObjectFilterClass::SkyBox6Face);
	GetSceneObjectVtable(sceneSystemDll, ".?AVCProjectedDecalSceneObjectDesc@@", SceneObjectFilterClass::ProjectedDecal);

/*
// References "RenderLayerDrawListPart"
void FUN_1800ea9e0(longlong pSceneSystem,longlong *param_2,uint *pCSceneLayer,ulonglong param_4,
                  uint count,int *param_6)

{
	// [...]
	
    FUN_18009c590(param_4,uVar6,param_3,param_5); // <-- This is the InitDrawingData function
    local_118 = 0;

    if (0 < *param_6) {
      do {
        puVar25 = *(uint **)(*(longlong *)(param_6 + 2) + (longlong)puVar22);
        puVar23 = puVar25 + 2;
        local_f8 = puVar25;
        if (*(CThreadedJob **)((longlong)local_d8 + (longlong)puVar22) == (CThreadedJob *)0x0) {
          FUN_1800e7590(0,param_3,puVar23,*puVar25); // This is the DebugSceneData function
        }
        else {
          CThreadedJob::WaitOrImmediatelyExecute
                    (*(CThreadedJob **)((longlong)local_d8 + (longlong)puVar22));
        }
        uVar29 = *puVar25;
        uVar24 = (ulonglong)uVar29;
        if ((param_3[0x12] & 0x28) == 0) {
          if (uVar29 != 0) {
            do {
              uVar29 = puVar23[0x18];
              bVar12 = (byte)uVar29 & 3;
              bVar17 = *(byte *)(*(longlong *)(puVar23 + 6) + 0x9a) & 3;
              if (bVar17 < bVar12) {
                bVar17 = bVar12;
              }
              if (bVar17 != 0) {
                plVar9 = *(longlong **)(*(longlong *)(puVar23 + 6) + 0x18);
                (**(code **)(*plVar9 + 0x18))
                          (plVar9,*(undefined8 *)(param_4 + 0x20),puVar23,uVar6,param_3,bVar17);
              }
              if (((byte)uVar29 & 8) == 0) {
                FUN_18009c880(param_4,puVar23); // <--- This is the DrawSceneData function we are after, it's also called in a deeper tree further bellow from FUN_1800eb800 / RenderLayerDrawListPart
              }
              else {
                FUN_18009cca0(param_4); // <-- This is DrawCurrentPrimitives function we are after
              }
              puVar23 = puVar23 + 0x1c;
              uVar24 = uVar24 - 1;
              puVar25 = local_f8;
            } while (uVar24 != 0);
          }
        }
        else if (uVar29 != 0) {
			// ...
		}
	}

    if (0 < *param_6)
        ...
        uVar26 = *puVar22;
        uVar21 = (ulonglong)uVar26;
        if ((pCSceneLayer[0x12] & 8) == 0) {
          if (uVar26 != 0) {
            do {
              uVar26 = puVar20[0x18];
              bVar9 = (byte)uVar26 & 3;
              bVar14 = *(byte *)(*(longlong *)(puVar20 + 6) + 0x9a) & 3;
              if (bVar14 < bVar9) {
                bVar14 = bVar9;
              }
              if (bVar14 != 0) {
                plVar7 = *(longlong **)(*(longlong *)(puVar20 + 6) + 0x18);
                (**(code **)(*plVar7 + 0x18))
                          (plVar7,*(undefined8 *)(param_4 + 0x20),puVar20,uVar4,pCSceneLayer,bVar14)
                ;
              }
              if (((byte)uVar26 & 8) == 0) {
                FUN_1800971c0(param_4,puVar20); // <--- This is the DrawSceneData function we are after, it's also called in a deeper tree further bellow from FUN_180096ed0
              }
              else {
                FUN_1800975e0(param_4); // <-- This is DrawCurrentPrimitives function we are after
              }
              puVar20 = puVar20 + 0x1a;
              uVar21 = uVar21 - 1;
              puVar22 = viewDrawList;
            } while (uVar21 != 0);
          }
        }
        else if (uVar26 != 0) { ... }
		...
    }
...
CTSListBase::Push((CTSListBase *)(pSceneSystem + 0xed0),(TSLNodeBase_t *)(puVar22 + -2));
...
CTSListBase::Push((CTSListBase *)(pSceneSystem + 0xed0),(TSLNodeBase_t *)(piVar5 + -2));
...
}

void FUN_1800971c0(longlong param_1,longlong param_2)
{
...
  cVar2 = FUN_1800978a0(param_1);
  if (cVar2 == '\0') {
    FUN_1800975e0(param_1);
  }
...
}

// DebugSceneData function
//
// - "Batchsort"
// - "Fullsort"
// - "shaded pass"
// - "depth pass"
// - "%s:%s after sort (%s %s)\n" (recommended)
// - "null material"
// - "C:\\buildworker\\csgo_rel_win64\\build\\src\\scenesystem\\scenesystem.cpp"
void FUN_1800e7590(undefined8 param_1,longlong pSceneLayer,longlong param_3,uint unkMaybeDictSize)
{
  // ...
  if (param_4 != 0) {
    uVar1 = *(uint *)(pSceneLayer + 0x48); // --> g_SceneLayer_Flags_Offset == 0x48
    uVar13 = (ulonglong)param_4;
	// ...
        uVar2 = *(uint *)(pSceneLayer + 0x48);
        uVar9 = (**(code **)**(undefined8 **)(pSceneLayer + 0x6f0))(); // pSceneLayer + 0x6f0 == pSceneView
        pcVar8 = "shaded pass";
        if ((uVar1 & 0x1000000) != 0) {
          pcVar8 = "depth pass";
        }
        pcVar12 = "Batchsort";
        if ((uVar2 & 1) != 0) {
          pcVar12 = "Fullsort";
        }
        Warning("%s:%s after sort (%s %s)\n",uVar9,pSceneLayer + 0x4b8,pcVar12,pcVar8); // --> 0x4b8 is g_SceneLayer_pszViewPass_Offset
	// ...
		plVar7 = (longlong *)(pSceneData + 0x18); // plVar7 == &(pSceneData->SceneObject)
			//...
		    uVar9 = (**(code **)**(undefined8 **)(*plVar7 + 0x18))(); // 0x18 is g_SceneObject_pSceneObjectDesc_Offset (pSceneData->SceneObject->SceneObjectDesc::GetName)
            Warning("  [%i] %s %s %s IndexCount: %d\n",(int)plVar7[8],uVar9,pcVar12,pcVar8,uVar11);
	// ...
}

// DrawSceneData function
void FUN_18009c880(longlong param_1,longlong param_2)
{
	// ...
  *(undefined8 *)(param_1 + 0x38) = *(undefined8 *)(*(longlong *)(param_2 + 0x18) + 0x18);
  if ((*(byte *)(param_2 + 0x62) & 8) == 0) {
    uVar6 = *(undefined8 *)(param_2 + 0x20);
  }
  *(undefined8 *)(param_1 + 0x40) = uVar6;
  uVar7 = *(ushort *)(param_1 + 0x48);
  if (*(ushort *)(param_1 + 0x48) <= *(ushort *)(param_2 + 0x5e)) {
    uVar7 = *(ushort *)(param_2 + 0x5e);
  }
  *(ushort *)(param_1 + 0x48) = uVar7;
  *(undefined8 *)(param_1 + 0x50) = *(undefined8 *)(param_2 + 0x38);
  *(undefined2 *)(param_1 + 0x4a) = *(undefined2 *)(param_2 + 0x62); // g_SceneData_Flags_Offset
}
*/
	//org_RenderLayerDrawListPart = (RenderLayerDrawListPart_t)getAddress(sceneSystemDll, "4c 89 4c 24 20 4c 89 44 24 18 48 89 54 24 10 48 89 4c 24 08 55 53 56 57 41 57 48 8d 6c 24 e0 48 81 ec 20 01 00 00");
	//if (0 == org_RenderLayerDrawListPart) ErrorBox(MkErrStr(__FILE__, __LINE__));

	// The drawing data flags byte moved (0x230 -> 0x250 in 2026-09-23 build), so it's wildcarded in the patterns below.
	org_InitDrawingData = (InitDrawingData_t)getAddress(sceneSystemDll, "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 ec ?? ?? 00 00 0f b6 81 ?? ?? 00 00 48 8b d9 4c 8b fa");
	if (0 == org_InitDrawingData) ErrorBox(MkErrStr(__FILE__, __LINE__));

	org_DrawSceneData = (DrawSceneData_t)getAddress(sceneSystemDll, "48 89 5c 24 20 55 48 83 ec 30 f6 81 ?? ?? 00 00 40");
	if (0 == org_DrawSceneData) ErrorBox(MkErrStr(__FILE__, __LINE__));

	org_DrawCurrentPrimitives = (DrawCurrentPrimitives_t)getAddress(sceneSystemDll, "4c 8b dc 53 48 81 ec d0 00 00 00 83 79 30 01 48 8b d9 0f 8c ?? ?? ?? ?? 48 8b 49 20 48 8d 15 ?? ?? ?? ??");
	if (0 == org_DrawCurrentPrimitives) ErrorBox(MkErrStr(__FILE__, __LINE__));

	// See notes in HookSceneSystem about DebugSceneData above:
	g_SceneLayer_pszViewPass_Offset = 0x4b8;
	g_SceneLayer_Flags_Offset = 0x48;
	g_SceneObject_pSceneObjectDesc_Offset = 0x18;

	// See notes in HookSceneSystem about DrawSceneData above:
	//g_SceneData_Flags_Offset = 0x62;

	if(//org_RenderLayerDrawListPart &&
		org_InitDrawingData && org_DrawSceneData && org_DrawCurrentPrimitives) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		//DetourAttach(&(PVOID&)org_RenderLayerDrawListPart, new_RenderLayerDrawListPart);
		DetourAttach(&(PVOID&)org_InitDrawingData, new_InitDrawingData);
		DetourAttach(&(PVOID&)org_DrawSceneData, new_DrawSceneData);
		//DetourAttach(&(PVOID&)org_DrawCurrentPrimitives, new_DrawCurrentPrimitives);

		// Non-fatal: the hooks installed after this one (lighting, tint) do not depend on it.
		if(NO_ERROR != DetourTransactionCommit()) {
			ErrorBox("Failed to detour SceneSystem functions.");
		}
	}

	// Scene lighting colour. This function is the home for scenesystem.dll hooks, so the detour is
	// attached from here; all of the policy lives in the module. Deliberately AFTER the commit
	// above and non-fatal on failure -- a dead pattern in one cosmetic feature must not take the
	// draw hooks down with it.
	Filmmaker::LightFx_Install(sceneSystemDll);
	// SST->CSM copy boundary probe. Own transaction conceptually (shares no policy with LightFx);
	// installed after LightFx and non-fatal if the pattern is missing after a CS2 update.
	Filmmaker::ShadowStageProbe_Install(sceneSystemDll);

	// Aggregate geometry tint. Its own transaction, deliberately after the draw hooks and
	// non-fatal: a missing RTTI name after a CS2 update costs one cosmetic feature, not the DLL.
	// The vtable was resolved at the top of this function so the descriptor table could avoid
	// double-detouring a shared Draw; re-use that answer rather than looking it up twice.
	if (!g_AggregateVtable) {
		Filmmaker::MvmDebugLog_LinefAlways("world.mod",
			"aggregate hook: CAggregateSceneObjectDesc vtable not found (CS2 updated?)");
	} else {
		g_AggregateOriginal = (DrawBaseSceneObject_t)g_AggregateVtable[1];
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)g_AggregateOriginal, new_DrawAggregateSceneObject);
		if (NO_ERROR != DetourTransactionCommit()) {
			g_AggregateOriginal = nullptr;
			Filmmaker::MvmDebugLog_LinefAlways("world.mod", "aggregate hook: DetourTransactionCommit failed");
		} else {
			Filmmaker::MvmDebugLog_LinefAlways("world.mod",
				"aggregate hook installed (original 0x%p)", (void*)g_AggregateOriginal);
		}
	}
}

void SceneSystem_RefreshTransientSky() { updateSkyboxEntities(); updateCloudMaterials(); }
