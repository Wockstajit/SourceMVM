#include "WorldSchema.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef

#include <string>

// Defined in AfxHookSource2/SchemaSystem.cpp. Forward-declared rather than added to
// SchemaSystem.h to keep this feature from editing a core header; a signature drift becomes a
// link error, which is a safe failure mode.
bool getOffset(std::ptrdiff_t* offset, std::string moduleName, std::string className, std::string fieldName);

namespace Filmmaker {

namespace {

struct FieldSpec {
	const char* className;
	const char* fieldName;
};

// Indexed by WorldField. MUST stay in the same order as the enum -- guarded by the static_assert
// below plus a runtime self-check in WorldSchema_Init.
//
// Every entry is a CLASS NAME + FIELD NAME pair, never an offset. See WorldSchema.h for why.
const FieldSpec kSpecs[kWF_Count] = {
	// --- Night Mode / exposure ---
	{ "C_PostProcessingVolume", "m_flMinExposure" },
	{ "C_PostProcessingVolume", "m_flMaxExposure" },
	{ "C_PostProcessingVolume", "m_bExposureControl" },
	{ "C_PostProcessingVolume", "m_bMaster" },
	{ "C_PostProcessingVolume", "m_flExposureCompensation" },
	{ "C_PostProcessingVolume", "m_flFadeDuration" },
	{ "CPlayer_CameraServices",  "m_hActivePostProcessingVolume" },

	// --- Fog ---
	{ "C_GradientFog", "m_fogColor" },
	{ "C_GradientFog", "m_flFogStartDistance" },
	{ "C_GradientFog", "m_flFogEndDistance" },
	{ "C_GradientFog", "m_flFogMaxOpacity" },
	{ "C_GradientFog", "m_flFogFalloffExponent" },
	{ "C_GradientFog", "m_bHeightFogEnabled" },
	{ "C_GradientFog", "m_flFogStartHeight" },
	{ "C_GradientFog", "m_flFogEndHeight" },
	{ "C_GradientFog", "m_flFogVerticalExponent" },
	{ "C_GradientFog", "m_flFogStrength" },
	{ "C_GradientFog", "m_bIsEnabled" },

	// --- Smoke ---
	{ "C_SmokeGrenadeProjectile", "m_vSmokeColor" },
	{ "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin" },
	{ "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect" },
	{ "C_SmokeGrenadeProjectile", "m_bSmokeVolumeDataReceived" },
	{ "C_SmokeGrenadeProjectile", "m_vSmokeDetonationPos" },
	{ "C_SmokeGrenadeProjectile", "m_VoxelFrameData" },
	{ "C_SmokeGrenadeProjectile", "m_nVoxelFrameDataSize" },
	{ "C_SmokeGrenadeProjectile", "m_nVoxelUpdate" },

	// --- Ragdoll physics (these live on C_BaseEntity, so they apply to any entity) ---
	{ "C_BaseEntity", "m_flGravityScale" },
	{ "C_BaseEntity", "m_bGravityDisabled" },
	{ "C_BaseEntity", "m_flFriction" },
	{ "C_BaseEntity", "m_flElasticity" },
	{ "C_BaseEntity", "m_flTimeScale" },

	// --- Sun / lights ---
	{ "C_LightEntity",   "m_CLightComponent" },
	{ "CLightComponent", "m_Color" },
	{ "CLightComponent", "m_flBrightness" },
	{ "CLightComponent", "m_flBrightnessScale" },
	{ "CLightComponent", "m_nCastShadows" },
	{ "CLightComponent", "m_SkyColor" },
	{ "CLightComponent", "m_flSkyIntensity" },
	{ "CLightComponent", "m_bEnabled" },
	{ "CLightComponent", "m_bUsesBakedShadowing" },
	{ "CLightComponent", "m_nBakedShadowIndex" },
	{ "CLightComponent", "m_bMixedShadows" },

	// --- Weather ---
	{ "C_Precipitation",     "m_flDensity" },
	{ "C_Precipitation",     "m_flParticleInnerDist" },
	{ "C_EnvWind", "m_EnvWindShared" },
	{ "C_EnvWindShared",     "m_iInitialWindDir" },
	{ "C_EnvWindShared",     "m_flInitialWindSpeed" },
	{ "C_EnvWindShared",     "m_iMinWind" },
	{ "C_EnvWindShared",     "m_iMaxWind" },

	// --- Sky fog params ---
	{ "C_EnvSky", "m_nFogType" },
	{ "C_EnvSky", "m_flFogMinStart" },
	{ "C_EnvSky", "m_flFogMinEnd" },
	{ "C_EnvSky", "m_flFogMaxStart" },
	{ "C_EnvSky", "m_flFogMaxEnd" },
	{ "C_EnvSky", "m_bEnabled" },

	// --- Sky orientation (SkyOrient.cpp) ---
	// Generic scene-node fields, not sky-specific: env_sky is an ordinary entity and its cubemap
	// is sampled through its transform.
	{ "C_BaseEntity",    "m_CBodyComponent" },
	{ "CBodyComponent",  "m_pSceneNode" },
	{ "CGameSceneNode",  "m_angRotation" },
	{ "CGameSceneNode",  "m_angAbsRotation" },
	{ "CGameSceneNode",  "m_flScale" },
	{ "CGameSceneNode",  "m_nodeToWorld" },
};

static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == (std::size_t)kWF_Count,
	"kSpecs must have exactly one entry per WorldField");

std::ptrdiff_t g_offsets[kWF_Count] = { 0 };
bool g_initDone = false;
bool g_ready = false;

} // namespace

void WorldSchema_Init() {
	if (g_initDone) return;
	g_initDone = true;

	int resolved = 0;
	for (int i = 0; i < (int)kWF_Count; ++i) {
		std::ptrdiff_t off = 0;
		// Non-fatal by construction: a miss leaves the slot at 0 and every consumer guards on it.
		if (getOffset(&off, "client.dll", kSpecs[i].className, kSpecs[i].fieldName) && off != 0) {
			g_offsets[i] = off;
			++resolved;
		}
	}

	// A total miss means the schema map was already cleared (WorldSchema_Init ran too late in
	// the startup order) rather than "Valve renamed all 44 fields". Distinguishing the two
	// matters: the first is our bug, the second is update fallout.
	g_ready = (resolved > 0);

	MvmDebugLog_Linef("world.schema", "init resolved=%d/%d ready=%d",
		resolved, (int)kWF_Count, g_ready ? 1 : 0);
	for (int i = 0; i < (int)kWF_Count; ++i) {
		if (0 == g_offsets[i]) {
			MvmDebugLog_Linef("world.schema", "MISS %s::%s",
				kSpecs[i].className, kSpecs[i].fieldName);
		}
	}
}

bool WorldSchema_Ready() { return g_ready; }

std::ptrdiff_t WorldSchema_Offset(WorldField field) {
	if (field < 0 || field >= kWF_Count) return 0;
	return g_offsets[field];
}

const char* WorldSchema_ClassName(WorldField field) {
	if (field < 0 || field >= kWF_Count) return "?";
	return kSpecs[field].className;
}

const char* WorldSchema_FieldName(WorldField field) {
	if (field < 0 || field >= kWF_Count) return "?";
	return kSpecs[field].fieldName;
}

bool WorldSchema_HasAll(const WorldField* fields, std::size_t count) {
	if (!fields) return false;
	for (std::size_t i = 0; i < count; ++i) {
		if (0 == WorldSchema_Offset(fields[i])) return false;
	}
	return true;
}

const char* WorldSchema_FirstMissing(const WorldField* fields, std::size_t count) {
	if (!fields) return "(null field list)";
	for (std::size_t i = 0; i < count; ++i) {
		if (0 == WorldSchema_Offset(fields[i])) return WorldSchema_FieldName(fields[i]);
	}
	return nullptr;
}

} // namespace Filmmaker
