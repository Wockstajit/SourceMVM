#include "WorldTransient.h"
#include "LightFx.h"
#include "LightTintMath.h"
#include "SunRotationMath.h"
#include "SunFx.h"
#include "WorldEntities.h"
#include "WorldSchema.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../ClientEntitySystem.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/binutils.h"
#include "../../../Deps/release/Detours/src/detours.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace Afx::BinUtils;

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Derived on CS2 build 14174 (2026-08-04) -- see Docs/effects/sun.md 3.3 for the convar-anchor
// chain that produced it, which is what to re-run after an update. Verified unique: exactly one
// match in scenesystem.dll's .text.
//
// FindPatternString consumes TWO hex chars per wildcard, so wildcards are "??" not "?".
constexpr const char* kLightFuncPattern =
	// Build 14182: resolved from the lb_enable_lights ConVar at +0x9239D0;
	// this function reads its +8 pointer and light color at object +0xE4/E8/EC.
	"48 89 54 24 ?? 55 57 41 56 48 83 ec ?? 48 8b 05 ?? ?? ?? ?? 4d 8b f0 48 8b fa 48 8b e9 80 78 58 00";

// Recovered from the live `lb_allow_shadow_rotation` read. This is the engine's own light-data
// builder, not the old UpdateLightShadowMap signature from the forum post. Its second argument
// is the short-lived light build record that the shadow code consumes; the original routine fills
// its direction at +0x184 before returning. We detour this point and rotate that temporary record
// after the engine has filled it, so all subsequent cascade/frustum work sees one coherent light.
constexpr const char* kLightDirectionBuildPattern =
	"48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 "
	"80 ba 03 02 00 00 00 49 8b f8 48 8b da 48 8b f1";

// Parent shadow-record builder. This is the function that gates the helper above on
// lb_allow_shadow_rotation and the light's material/shadow flags. It is diagnostic-only:
// keeping the mutation at the smaller helper means the parent can be observed without
// touching its large stack frame or persistent scene-light object.
constexpr const char* kShadowBuildParentPattern =
	"48 89 54 24 10 55 53 41 54 41 56 48 8d ac 24 ?? ?? ?? ?? "
	"48 81 ec ?? ?? ?? ?? 48 8b da 4c 8b e1";

// Offsets into the scene light object (arg2). Read straight out of the disassembly.
constexpr std::ptrdiff_t kOffColorR = 0xE4;
constexpr std::ptrdiff_t kOffColorG = 0xE8;
constexpr std::ptrdiff_t kOffColorB = 0xEC;

// THE LIGHT DIRECTION -- CONFIRMED BY PROBE, not copied from the reference.
//
// The reference (reference/coding help/world-modulation/734508-light-rotation.md) puts a lone
// `m_vDirectionNormalized` at data+0xA4 and describes the data block at +0xE0, which lines up
// with our disassembly finding colour at +0xE4 (= 0xE0 + 0x4). Probing live light objects for
// unit-length float triples found the primary direction at +0x184. A second normalized value is
// present at +0x190, but every direct attempt to write it (and every save/restore variant) could
// crash the engine during shadow-cascade work. The shipped path therefore writes only the
// validated +0x184 direction and deliberately leaves +0x190 untouched. The engine's own shadow
// builder, when it is entered on a map, receives the same rotated direction through its temporary
// build record; the persistent submit fallback covers maps that skip that optional path.
constexpr std::ptrdiff_t kOffDirection = 0x184;   // primary direction (vec3)
constexpr std::ptrdiff_t kOffBuildDirection = 0x184;
// Baked shadow index on the scene light object (r15+0x74 in the LightBinner builder
// where r15 = light+0xE0). 0xFF = none. See Docs/effects/shadow-separation.md.
constexpr std::ptrdiff_t kOffBakedShadowIndex = 0x154;
constexpr std::ptrdiff_t kOffLightTypeByte = 0x157; // light class byte near the index
constexpr std::ptrdiff_t kOffBakedFlagByte = 0x11D; // tested == 1 beside the index

// Bounded, opt-in. Scans a window of the light object for any consecutive float triple of unit
// length -- a normalized direction is unmistakable that way, so the offset is CONFIRMED rather
// than copied from a struct that may be a build or two stale.
std::atomic<int> g_probeLights{ 0 };
// Research-only: log and optionally force the per-light baked shadow index to 0xFF.
std::atomic<bool> g_bakedIndexLog{ false };
std::atomic<bool> g_bakedIndexForceNone{ false };
std::atomic<unsigned long long> g_bakedIndexWrites{ 0 };
std::mutex g_bakedIndexMutex;
std::unordered_map<void*, unsigned char> g_bakedIndexOriginals;

// SEH helpers must stay free of C++ objects that need unwinding (MSVC C2712).
bool SehWriteBakedIndex(void* lightObject, unsigned char value) {
	__try {
		*(unsigned char*)((unsigned char*)lightObject + kOffBakedShadowIndex) = value;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool SehReadBakedIndexFields(unsigned char* lightBytes,
	unsigned char* outIndex, unsigned char* outType, unsigned char* outFlag) {
	__try {
		*outIndex = *(unsigned char*)(lightBytes + kOffBakedShadowIndex);
		*outType = *(unsigned char*)(lightBytes + kOffLightTypeByte);
		*outFlag = *(unsigned char*)(lightBytes + kOffBakedFlagByte);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void RestoreBakedIndexOriginals() {
	std::unordered_map<void*, unsigned char> snapshot;
	{
		std::lock_guard<std::mutex> lock(g_bakedIndexMutex);
		snapshot.swap(g_bakedIndexOriginals);
	}
	for (const auto& entry : snapshot) {
		SehWriteBakedIndex(entry.first, entry.second);
	}
	MvmDebugLog_LinefAlways("world.light.bakedindex",
		"restored %zu baked-index originals", snapshot.size());
}

typedef void* (__fastcall* LightSubmit_t)(void* a1, void* lightObject, void* a3);
LightSubmit_t g_original = nullptr;
typedef void (__fastcall* LightDirectionBuild_t)(void* sceneSystem, void* buildRecord, void* source);
LightDirectionBuild_t g_originalDirectionBuild = nullptr;
typedef void (__fastcall* ShadowBuildParent_t)(void* sceneSystem, void* sourceId, void* source, void* unused);
ShadowBuildParent_t g_originalShadowBuildParent = nullptr;

std::string g_unavailableReason;
bool g_installed = false;
bool g_rotationInstalled = false;

// Published from the main thread, read on the submit thread. Plain scalars: a torn read would at
// worst tint one light wrong for one frame, which is not worth a lock on a per-light path.
std::atomic<int>   g_tintR{ 255 }, g_tintG{ 255 }, g_tintB{ 255 };
std::atomic<float> g_amount{ 1.0f };
std::atomic<bool> g_sceneLightsEnabled{true};
std::atomic<bool> g_environmentSunSuppressed{false};
std::atomic<unsigned long long> g_lightsSeen{ 0 };
std::atomic<unsigned long long> g_rotationBuilds{ 0 };
std::atomic<unsigned long long> g_shadowBuildParents{ 0 };
std::atomic<unsigned long long> g_persistentRotationWrites{ 0 };

// Fallback for maps/builds that do not enter the engine's optional shadow-builder path. The
// scene-submit hook is guaranteed to receive the persistent light object; retain one authored
// direction per live pointer so repeated submits do not compound the requested yaw/pitch.
std::mutex g_rotationObjectMutex;
std::unordered_map<void*, SunRotationMath::Vec3> g_rotationObjectBases;

std::atomic<float> g_yaw{ 0.0f }, g_pitch{ 0.0f };
std::atomic<float> g_referenceX{ 0.0f }, g_referenceY{ 0.0f }, g_referenceZ{ 0.0f };
std::atomic<bool> g_referenceValid{ false };
bool g_rotationCvarRequested = false;
bool g_rotationCvarOriginal = false;
SOURCESDK::CS2::ConVarHandle g_rotationHandle;

// Uniform replaces hue while preserving each light's original peak radiance. Filtered multiplies
// the engine-authored channels by the tint. Neither mode can add HDR energy or create glow.
std::atomic<bool> g_uniform{ true };

bool AnyColorOverride() {
	return g_environmentSunSuppressed.load(std::memory_order_relaxed) || !g_sceneLightsEnabled.load(std::memory_order_relaxed) || (float(WorldTransient::Number("lightAmount", g_amount.load(std::memory_order_relaxed), 0)) > 0.0f
		&& (float(WorldTransient::Number("lightColor", g_tintR.load(), 0)) != 255 || float(WorldTransient::Number("lightColor", g_tintG.load(), 1)) != 255 || float(WorldTransient::Number("lightColor", g_tintB.load(), 2)) != 255));
}
bool AnyRotation() { return float(WorldTransient::Number("lightDirection", g_yaw.load(), 0)) != 0.0f || float(WorldTransient::Number("lightDirection", g_pitch.load(), 1)) != 0.0f; }
bool AnyOverride() { return AnyColorOverride() || AnyRotation(); }

bool RotationObjectTracked(void* object) {
	std::lock_guard<std::mutex> lock(g_rotationObjectMutex);
	return g_rotationObjectBases.find(object) != g_rotationObjectBases.end();
}

float SrgbByteToLinear(int value) {
	const float srgb = (float)value / 255.0f;
	return srgb <= 0.04045f
		? srgb / 12.92f
		: powf((srgb + 0.055f) / 1.055f, 2.4f);
}

std::atomic<unsigned long long> g_lightsRotated{ 0 };

// Reports every consecutive float triple in [0x100, 0x1C0) whose length is ~1.0. A normalized
// direction vector is the only thing in a light object that looks like that, so this identifies
// the field instead of trusting a published offset.
void ProbeLight(unsigned char* p) {
	char found[512]; found[0] = 0; size_t used = 0;
	for (std::ptrdiff_t off = 0x100; off + 12 <= 0x1C0; off += 4) {
		const float* v = (const float*)(p + off);
		const double len = sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]);
		if (len > 0.98 && len < 1.02) {
			int n = _snprintf_s(found + used, sizeof(found) - used, _TRUNCATE,
				"  0x%llX=(%.3f %.3f %.3f)|", (unsigned long long)off, v[0], v[1], v[2]);
			if (n > 0) used += n;
			if (used > sizeof(found) - 64) break;
		}
	}
	const float* d = (const float*)(p + kOffDirection);

	// THE MAP'S OWN LIGHT COLOUR, before we touch it. This is the number that decides whether a
	// tint reads as deep or washed out, and it is the thing that differs between the menu map and
	// an in-game map. Log it raw -- these are LINEAR HDR radiances, so values far above 1.0 are
	// normal and are exactly what would swamp an absolute write.
	const float sr = *(const float*)(p + kOffColorR);
	const float sg = *(const float*)(p + kOffColorG);
	const float sb = *(const float*)(p + kOffColorB);

	MvmDebugLog_Linef("world.light",
		"probe obj=%p srcRGB=(%.3f %.3f %.3f) sum=%.3f || unitTriples:%s || 0x184=(%.3f %.3f %.3f)",
		(void*)p, sr, sg, sb, sr + sg + sb, found[0] ? found : " none", d[0], d[1], d[2]);
}

const char* const kSunLightClasses[] = {
	"light_environment", "C_LightEnvironmentEntity",
	"light_directional", "C_LightDirectionalEntity",
	"global_light",      "C_GlobalLight",
	"env_cascade_light",
};

void ProbeLightComponents() {
	const std::ptrdiff_t entityComponent = WorldSchema_Offset(kWF_Light_Component);
	const std::ptrdiff_t usesBaked = WorldSchema_Offset(kWF_LightC_UsesBakedShadowing);
	const std::ptrdiff_t bakedIndex = WorldSchema_Offset(kWF_LightC_BakedShadowIndex);
	const std::ptrdiff_t mixed = WorldSchema_Offset(kWF_LightC_MixedShadows);
	if (!g_pEntityList || !g_GetEntityFromIndex || !entityComponent || !usesBaked
		|| !bakedIndex || !mixed) {
		advancedfx::Warning(
			"world light component probe unavailable: entityComponent=0x%zX "
			"usesBaked=0x%zX bakedIndex=0x%zX mixed=0x%zX.\n",
			(size_t)entityComponent, (size_t)usesBaked, (size_t)bakedIndex, (size_t)mixed);
		return;
	}

	int found = 0;
	for (int index = 0; index <= GetHighestEntityIndex(); ++index) {
		CEntityInstance* entity = nullptr;
		void* component = nullptr;
		bool uses = false;
		int shadowIndex = 0;
		bool isMixed = false;
		const char* debugName = nullptr;
		__try {
			entity = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
			if (!WorldEntity_Matches(entity, kSunLightClasses,
				sizeof(kSunLightClasses) / sizeof(kSunLightClasses[0]))) {
				continue;
			}
			component = *(void**)((unsigned char*)entity + entityComponent);
			if (!component) continue;
			uses = *(bool*)((unsigned char*)component + usesBaked);
			shadowIndex = *(int*)((unsigned char*)component + bakedIndex);
			isMixed = *(bool*)((unsigned char*)component + mixed);
			debugName = entity->GetDebugName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			continue;
		}

		advancedfx::Message(
			"light entity %d component=%p usesBaked=%d bakedIndex=%d mixed=%d debug='%s'\n",
			index, component, uses ? 1 : 0, shadowIndex, isMixed ? 1 : 0,
			debugName ? debugName : "");
		if (MvmDebugLog_Active()) {
			MvmDebugLog_Linef("world.light.component",
				"entity=%d ent=%p component=%p offComponent=0x%zX "
				"offUsesBaked=0x%zX offBakedIndex=0x%zX offMixed=0x%zX "
				"usesBaked=%d bakedIndex=%d mixed=%d debug='%s'",
				index, (void*)entity, component, (size_t)entityComponent,
				(size_t)usesBaked, (size_t)bakedIndex, (size_t)mixed,
				uses ? 1 : 0, shadowIndex, isMixed ? 1 : 0,
				debugName ? debugName : "");
		}
		++found;
	}
	advancedfx::Message(
		"world light component probe: %d sun-light entities, schema offsets "
		"component=0x%zX usesBaked=0x%zX bakedIndex=0x%zX mixed=0x%zX.\n",
		found, (size_t)entityComponent, (size_t)usesBaked,
		(size_t)bakedIndex, (size_t)mixed);
}

// The reference the sun sliders turn about (see LightFx.h). The scene light's direction field points
// TOWARD the sun (measured on Inferno 2026-09-28: elevation +63; reversing it read -63 and made Tilt run
// backwards), so until SunFx has published the sky's sun, the light's own direction stands in.
static SunRotationMath::Vec3 SunReferenceFor(const SunRotationMath::Vec3& lightDirection) {
	if (g_referenceValid.load(std::memory_order_relaxed))
		return { g_referenceX.load(std::memory_order_relaxed), g_referenceY.load(std::memory_order_relaxed),
			g_referenceZ.load(std::memory_order_relaxed) };
	return lightDirection;
}

static bool ReadUnitDirection(void* object, SunRotationMath::Vec3* out) {
	if (!object || !out) return false;
	__try {
		const float* d = (const float*)((unsigned char*)object + kOffDirection);
		out->x = d[0]; out->y = d[1]; out->z = d[2];
		const float length = SunRotationMath::Length(*out);
		return std::isfinite(length) && length >= 0.50f && length <= 1.50f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static bool WriteUnitDirection(void* object, const SunRotationMath::Vec3& value) {
	if (!object) return false;
	__try {
		float* d = (float*)((unsigned char*)object + kOffDirection);
		d[0] = value.x; d[1] = value.y; d[2] = value.z;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static bool RotateSubmittedDirection(void* object, float yaw, float pitch,
	SunRotationMath::Vec3* baseOut = nullptr, SunRotationMath::Vec3* rotatedOut = nullptr) {
	SunRotationMath::Vec3 base{};
	bool inserted = false;
	{
		std::lock_guard<std::mutex> lock(g_rotationObjectMutex);
		auto found = g_rotationObjectBases.find(object);
		if (found != g_rotationObjectBases.end()) {
			base = found->second;
		} else {
			if (!ReadUnitDirection(object, &base)) return false;
			g_rotationObjectBases.emplace(object, base);
			inserted = true;
		}
	}
	const SunRotationMath::Vec3 rotated = SunRotationMath::Rotate(base, yaw, pitch, SunReferenceFor(base));
	if (baseOut) *baseOut = base;
	if (rotatedOut) *rotatedOut = rotated;
	if (WriteUnitDirection(object, rotated)) return true;
	if (inserted) {
		std::lock_guard<std::mutex> lock(g_rotationObjectMutex);
		g_rotationObjectBases.erase(object);
	}
	return false;
}

static bool RestoreSubmittedDirection(void* object) {
	SunRotationMath::Vec3 base{};
	{
		std::lock_guard<std::mutex> lock(g_rotationObjectMutex);
		auto found = g_rotationObjectBases.find(object);
		if (found == g_rotationObjectBases.end()) return false;
		base = found->second;
		g_rotationObjectBases.erase(found);
	}
	return WriteUnitDirection(object, base);
}

void* __fastcall new_LightSubmit(void* a1, void* lightObject, void* a3) {
	if (!lightObject) return g_original(a1, lightObject, a3);

	const unsigned long long lightIndex =
		g_lightsSeen.fetch_add(1, std::memory_order_relaxed) + 1;

	if (g_probeLights.load(std::memory_order_relaxed) > 0) {
		g_probeLights.fetch_sub(1, std::memory_order_relaxed);
		ProbeLight((unsigned char*)lightObject);
	}

	unsigned char* lightBytes = (unsigned char*)lightObject;
	unsigned char bakedIndex = 0xFF;
	unsigned char lightType = 0xFF;
	unsigned char bakedFlag = 0xFF;
	if (!SehReadBakedIndexFields(lightBytes, &bakedIndex, &lightType, &bakedFlag)) {
		bakedIndex = lightType = bakedFlag = 0xFE;
	}
    if (g_bakedIndexLog.load(std::memory_order_relaxed) && MvmDebugLog_Active()) {
        static std::mutex seenMutex;
        static std::unordered_map<void*, unsigned> seen;
        std::lock_guard<std::mutex> lock(seenMutex);
        const unsigned signature = unsigned(lightType) | (unsigned(bakedIndex) << 8) | (unsigned(bakedFlag) << 16);
        auto found = seen.find(lightObject);
        if (seen.size() < 2048 && (found == seen.end() || found->second != signature)) {
            seen[lightObject] = signature;
            MvmDebugLog_LinefAlways("world.light.bakedindex",
                "object=%p typeOffset=0x%zx type=%u indexOffset=0x%zx index=%u flag=%u rgb=(%.4f %.4f %.4f)",
                lightObject, size_t(kOffLightTypeByte), unsigned(lightType), size_t(kOffBakedShadowIndex), unsigned(bakedIndex), unsigned(bakedFlag),
                *(float*)(lightBytes+kOffColorR), *(float*)(lightBytes+kOffColorG), *(float*)(lightBytes+kOffColorB));
        }
    }
	const bool rotationTracked = RotationObjectTracked(lightObject);
	if (!AnyOverride() && !rotationTracked) return g_original(a1, lightObject, a3);

	const float amount = float(WorldTransient::Number("lightAmount", g_amount.load(std::memory_order_relaxed), 0));
	const int tintR = float(WorldTransient::Number("lightColor", g_tintR.load(std::memory_order_relaxed), 0));
	const int tintG = float(WorldTransient::Number("lightColor", g_tintG.load(std::memory_order_relaxed), 1));
	const int tintB = float(WorldTransient::Number("lightColor", g_tintB.load(std::memory_order_relaxed), 2));
	// Panorama's picker reports display/sRGB bytes. Scene light radiance is LINEAR HDR. Treating
	// 60 as 60/255 linear makes that channel about five times too bright and turns saturated
	// magenta into the measured pastel pink. Primary RGB did not expose this because 0 and 255 are
	// fixed points of both transfer functions.
	const float linearR = SrgbByteToLinear(tintR);
	const float linearG = SrgbByteToLinear(tintG);
	const float linearB = SrgbByteToLinear(tintB);

	unsigned char* p = (unsigned char*)lightObject;
	float* pr = (float*)(p + kOffColorR);
	float* pg = (float*)(p + kOffColorG);
	float* pb = (float*)(p + kOffColorB);

	// OVERRIDE FOR THE DURATION OF THE CALL, THEN PUT THE ENGINE'S VALUES BACK.
	//
	// This buffer belongs to the engine and persists across frames, so an in-place `*=` COMPOUNDS:
	// intensity 2 becomes 4 then 8 then 16, and intensity 0 writes a permanent zero that no later
	// value can recover from (0 * anything is still 0). That is exactly what "turn it up, then
	// reset, and it stays broken" was -- the reset multiplied by 1.0 against a buffer we had
	// already destroyed.
	//
	// Saving and restoring around the original call keeps every frame relative to the map's own
	// value, needs no per-object bookkeeping, and leaves nothing to clean up when the feature is off.
	const float sr = *pr, sg = *pg, sb = *pb;
	if (AnyColorOverride()) {
		const LightTintMode mode = float(WorldTransient::Number("lightUniform", g_uniform.load(std::memory_order_relaxed), 0))
			? LightTintMode::Uniform : LightTintMode::Filtered;
		const bool suppressSun = lightType == 0 && g_environmentSunSuppressed.load(std::memory_order_relaxed);
		const LightTintRgb wrote = g_sceneLightsEnabled.load(std::memory_order_relaxed) && !suppressSun ? ApplyLightTint(
			{ sr, sg, sb }, { linearR, linearG, linearB }, amount, mode) : LightTintRgb{0,0,0};
		*pr = wrote.r; *pg = wrote.g; *pb = wrote.b;
		if (MvmDebugLog_Active() && (lightIndex % 2048ull) == 0) {
			MvmDebugLog_Linef("world.light.write",
				"obj=%p offsets=(0x%llx 0x%llx 0x%llx) mode=%s uiRGB=(%d %d %d) "
				"linear=(%.4f %.4f %.4f) amount=%.3f src=(%.3f %.3f %.3f) wrote=(%.3f %.3f %.3f)",
				(void*)p, (unsigned long long)kOffColorR, (unsigned long long)kOffColorG,
				(unsigned long long)kOffColorB,
				float(WorldTransient::Number("lightUniform", g_uniform.load(std::memory_order_relaxed), 0)) ? "uniform" : "filtered",
				tintR, tintG, tintB, linearR, linearG, linearB, amount,
				sr, sg, sb, *pr, *pg, *pb);
		}
	}

	if (AnyRotation()) {
		SunRotationMath::Vec3 base{}, rotated{};
		if (RotateSubmittedDirection(lightObject, float(WorldTransient::Number("lightDirection", g_yaw.load(std::memory_order_relaxed), 0)),
			float(WorldTransient::Number("lightDirection", g_pitch.load(std::memory_order_relaxed), 1)), &base, &rotated)) {
			const unsigned long long writes = g_persistentRotationWrites.fetch_add(1, std::memory_order_relaxed) + 1;
			if (MvmDebugLog_Active() && (writes % 256ull) == 1ull) {
				// Toward-sun azimuth/elevation before and after: yaw must change only the azimuth,
				// pitch only the elevation (SunRotationMath::SunRotation).
				const SunRotationMath::SunAngles from = SunRotationMath::ReferenceAngles(base);
				const SunRotationMath::SunAngles to = SunRotationMath::ReferenceAngles(rotated);
				MvmDebugLog_Linef("world.light.rotation", "sun write=%llu yaw=%.1f pitch=%.1f azimuth %.1f->%.1f elevation %.1f->%.1f reference=%d",
					writes, float(WorldTransient::Number("lightDirection", g_yaw.load(std::memory_order_relaxed), 0)),
					float(WorldTransient::Number("lightDirection", g_pitch.load(std::memory_order_relaxed), 1)),
					from.azimuth, to.azimuth, from.elevation, to.elevation, g_referenceValid.load(std::memory_order_relaxed) ? 1 : 0);
			}
			g_lightsRotated.fetch_add(1, std::memory_order_relaxed);
			if (MvmDebugLog_Active() && writes <= 4ull) {
				MvmDebugLog_Linef("world.light.rotation", "persistent obj=%p offset=0x%llx yaw=%.2f pitch=%.2f",
					lightObject, (unsigned long long)kOffDirection,
					float(WorldTransient::Number("lightDirection", g_yaw.load(std::memory_order_relaxed), 0)), float(WorldTransient::Number("lightDirection", g_pitch.load(std::memory_order_relaxed), 1)));
			}
		}
	} else if (rotationTracked) {
		RestoreSubmittedDirection(lightObject);
	}

	void* result = g_original(a1, lightObject, a3);

	*pr = sr; *pg = sg; *pb = sb;
	return result;
}

// The engine calls this after it has resolved a light's source direction and before it builds
// shadow cascades. `buildRecord` is stack/arena storage owned by the engine for this invocation,
// so changing it here cannot leave a stale persistent object behind. On the current Mirage run
// the optional builder is not reached; the submit hook below is the proven fallback.
static bool RotateBuildDirection(void* buildRecord, float yaw, float pitch,
	float* oldOut, float* newOut) {
	if (!buildRecord) return false;
	__try {
		float* d = (float*)((unsigned char*)buildRecord + kOffBuildDirection);
		const SunRotationMath::Vec3 old = { d[0], d[1], d[2] };
		const float length = SunRotationMath::Length(old);
		if (!std::isfinite(length) || length < 0.50f || length > 1.50f) return false;
		const SunRotationMath::Vec3 rotated = SunRotationMath::Rotate(old, yaw, pitch, SunReferenceFor(old));
		d[0] = rotated.x; d[1] = rotated.y; d[2] = rotated.z;
		if (oldOut) { oldOut[0] = old.x; oldOut[1] = old.y; oldOut[2] = old.z; }
		if (newOut) { newOut[0] = rotated.x; newOut[1] = rotated.y; newOut[2] = rotated.z; }
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void __fastcall new_LightDirectionBuild(void* sceneSystem, void* buildRecord, void* source) {
	if (g_originalDirectionBuild) g_originalDirectionBuild(sceneSystem, buildRecord, source);
	const unsigned long long build = g_rotationBuilds.fetch_add(1, std::memory_order_relaxed) + 1;
	if (!AnyRotation()) return;

	float oldDir[3] = {}, newDir[3] = {};
	const bool wrote = RotateBuildDirection(buildRecord,
		float(WorldTransient::Number("lightDirection", g_yaw.load(std::memory_order_relaxed), 0)), float(WorldTransient::Number("lightDirection", g_pitch.load(std::memory_order_relaxed), 1)),
		oldDir, newDir);
	if (wrote) {
		g_lightsRotated.fetch_add(1, std::memory_order_relaxed);
		if (MvmDebugLog_Active() && (build % 256ull) == 0) {
			MvmDebugLog_Linef("world.light.rotation",
				"build=%llu record=%p source=%p offset=0x%llx yaw=%.2f pitch=%.2f "
				"old=(%.4f %.4f %.4f) new=(%.4f %.4f %.4f)",
				build, buildRecord, source, (unsigned long long)kOffBuildDirection,
				float(WorldTransient::Number("lightDirection", g_yaw.load(std::memory_order_relaxed), 0)), float(WorldTransient::Number("lightDirection", g_pitch.load(std::memory_order_relaxed), 1)),
				oldDir[0], oldDir[1], oldDir[2], newDir[0], newDir[1], newDir[2]);
		}
	}
}

void __fastcall new_ShadowBuildParent(void* sceneSystem, void* sourceId, void* source, void* unused) {
	if (g_originalShadowBuildParent) g_originalShadowBuildParent(sceneSystem, sourceId, source, unused);
	const unsigned long long parent = g_shadowBuildParents.fetch_add(1, std::memory_order_relaxed) + 1;
	if (MvmDebugLog_Active() && parent <= 4ull) {
		MvmDebugLog_Linef("world.light.rotation", "shadow parent entry=%llu scene=%p sourceId=%p source=%p",
			parent, sceneSystem, sourceId, source);
	}
}

} // namespace

void LightFx_Install(void* sceneSystemDll) {
	if (g_installed) return;
	if (!sceneSystemDll) { g_unavailableReason = "scenesystem.dll handle was null."; return; }

	MemRange textRange = MemRange(0, 0);
	{
		ImageSectionsReader reader((HMODULE)sceneSystemDll);
		if (!reader.Eof()) textRange = reader.GetMemRange();
	}
	if (textRange.IsEmpty()) {
		g_unavailableReason = "could not read scenesystem.dll sections.";
		return;
	}

	MemRange found = FindPatternString(textRange, kLightFuncPattern);
	if (found.IsEmpty()) {
		// Loud but NOT fatal: this is one cosmetic feature, and a dead pattern after a CS2 update
		// must not take the whole DLL down with it.
		g_unavailableReason =
			"light-submit pattern not found in scenesystem.dll (CS2 updated?). "
			"Re-derive it via the lb_enable_lights convar anchor - Docs/effects/sun.md 3.3.";
		MvmDebugLog_LinefAlways("world.light", "install FAILED: %s", g_unavailableReason.c_str());
		return;
	}

	g_original = (LightSubmit_t)found.Start;
	MemRange directionFound = FindPatternString(textRange, kLightDirectionBuildPattern);
	if (!directionFound.IsEmpty()) {
		g_originalDirectionBuild = (LightDirectionBuild_t)directionFound.Start;
	} else {
		MvmDebugLog_LinefAlways("world.light.rotation",
			"rotation builder pattern not found; color hook remains available. "
			"Re-derive from lb_allow_shadow_rotation.");
	}
	MemRange parentFound = FindPatternString(textRange, kShadowBuildParentPattern);
	if (!parentFound.IsEmpty()) {
		g_originalShadowBuildParent = (ShadowBuildParent_t)parentFound.Start;
	} else {
		MvmDebugLog_LinefAlways("world.light.rotation", "shadow parent pattern not found; helper diagnostics unavailable");
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_original, new_LightSubmit);
	if (g_originalDirectionBuild)
		DetourAttach(&(PVOID&)g_originalDirectionBuild, new_LightDirectionBuild);
	if (g_originalShadowBuildParent)
		DetourAttach(&(PVOID&)g_originalShadowBuildParent, new_ShadowBuildParent);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_original = nullptr;
		g_originalDirectionBuild = nullptr;
		g_originalShadowBuildParent = nullptr;
		g_unavailableReason = "DetourTransactionCommit failed for the light-submit hook.";
		MvmDebugLog_LinefAlways("world.light", "install FAILED: %s", g_unavailableReason.c_str());
		return;
	}

	g_installed = true;
	// Rotation's primary path is the already-installed scene-light submit hook. The optional
	// engine builder detour improves maps that enter that path, but its absence must not make the
	// working persistent direction control report unavailable.
	g_rotationInstalled = true;
	MvmDebugLog_LinefAlways("world.light", "installed at 0x%p", (void*)found.Start);
	MvmDebugLog_LinefAlways("world.light.rotation", "persistent direction path installed; engine builder %s at 0x%p",
		directionFound.IsEmpty() ? "not found" : "installed",
		directionFound.IsEmpty() ? nullptr : (void*)directionFound.Start);
}

bool LightFx_Available(std::string* outReason) {
	if (g_installed) return true;
	if (outReason) *outReason = g_unavailableReason.empty()
		? std::string("light hook not installed yet.") : g_unavailableReason;
	return false;
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void SyncSunAppearance() {
	SunFx_SetSceneLightTint(
		g_tintR.load(std::memory_order_relaxed),
		g_tintG.load(std::memory_order_relaxed),
		g_tintB.load(std::memory_order_relaxed),
		g_amount.load(std::memory_order_relaxed),
		g_uniform.load(std::memory_order_relaxed));
}

void LightFx_SetTint(int r, int g, int b) {
	g_tintR.store(ClampI(r, 0, 255));
	g_tintG.store(ClampI(g, 0, 255));
	g_tintB.store(ClampI(b, 0, 255));
	SyncSunAppearance();
}

void LightFx_SetAmount(float amount) {
	if (!std::isfinite(amount)) amount = 0.0f;
	if (amount < 0.0f) amount = 0.0f;
	if (amount > 1.0f) amount = 1.0f;
	g_amount.store(amount);
	SyncSunAppearance();
}

void LightFx_SetUniform(bool uniform) {
	g_uniform.store(uniform, std::memory_order_relaxed);
	SyncSunAppearance();
}

bool LightFx_RotationAvailable(std::string* outReason) {
	if (g_rotationInstalled) return true;
	if (outReason) *outReason = std::string(
		"engine light-data builder was not found; rotation is disabled on this CS2 build.");
	return false;
}

static void EnsureShadowRotationEnabled() {
	if (g_rotationCvarRequested || !g_pEngineToClient) return;
	// This is an offline filmmaker control. The engine gates its own coherent light-update path
	// behind this cvar; enabling it is required for the detoured builder to run.
	if (SOURCESDK::CS2::g_pCVar) {
		SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar("lb_allow_shadow_rotation", false);
		// Hidden/development convars are intentionally omitted by FindConVar on released
		// builds. Walk the SDK's bounded registry as the existing HLAE convar bridge does,
		// then clear only the presentation flags so the engine's own command/callback path
		// can see the value.
		if (!handle.IsValid()) {
			for (SOURCESDK::uint32 i = 0; i < SOURCESDK_CS2_MAX_VALID_CVARS; ++i) {
				SOURCESDK::CS2::Cvar_s* candidate = SOURCESDK::CS2::g_pCVar->GetCvar(i);
				if (!candidate) break;
				if (candidate->m_pszName && 0 == std::strcmp(candidate->m_pszName, "lb_allow_shadow_rotation")) {
					candidate->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_HIDDEN | SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY);
					handle.Set(i);
					MvmDebugLog_LinefAlways("world.light.rotation",
						"found hidden cvar index=%u flags=0x%llx type=%d",
						i, (unsigned long long)candidate->m_nFlags, (int)candidate->m_eVarType);
					break;
				}
			}
		}
		if (handle.IsValid()) {
			if (SOURCESDK::CS2::Cvar_s* cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get())) {
				SOURCESDK::CS2::CVValue_t oldValue = {};
				oldValue.m_bValue = cvar->m_Value.m_bValue;
                g_rotationCvarOriginal=oldValue.m_bValue;g_rotationHandle=handle;
                g_rotationCvarRequested=true;
				cvar->m_Value.m_bValue = true;
				SOURCESDK::CS2::g_pCVar->CallChangeCallback(handle, 0, &cvar->m_Value, &oldValue);
				MvmDebugLog_LinefAlways("world.light.rotation",
					"direct cvar set name=lb_allow_shadow_rotation ptr=%p old=%d new=%d",
					(void*)cvar, oldValue.m_bValue ? 1 : 0, cvar->m_Value.m_bValue ? 1 : 0);
			}
		} else {
			MvmDebugLog_LinefAlways("world.light.rotation",
				"direct cvar set skipped: lb_allow_shadow_rotation handle invalid");
		}
	}
	MvmDebugLog_LinefAlways("world.light.rotation",
		"requested lb_allow_shadow_rotation 1 (offline filmmaker control)");
}

static void ReleaseRotationCvar() {
    if(g_rotationCvarRequested && SOURCESDK::CS2::g_pCVar && g_rotationHandle.IsValid()) {
        auto* cvar=SOURCESDK::CS2::g_pCVar->GetCvar(g_rotationHandle.Get());
        if(cvar) {
            SOURCESDK::CS2::CVValue_t oldValue={};oldValue.m_bValue=cvar->m_Value.m_bValue;
            cvar->m_Value.m_bValue=g_rotationCvarOriginal;
            SOURCESDK::CS2::g_pCVar->CallChangeCallback(g_rotationHandle,0,&cvar->m_Value,&oldValue);
        }
    }
    g_rotationCvarRequested=false;
}
void LightFx_OnLevelInit() {
    // Scene objects are level-owned. Never reuse a prior map's pointer snapshot.
    {std::lock_guard<std::mutex> lock(g_rotationObjectMutex);g_rotationObjectBases.clear();}
    ReleaseRotationCvar();
    g_referenceValid.store(false, std::memory_order_relaxed); // the new map publishes its own sun
}
void LightFx_Reset() {
	g_sceneLightsEnabled=true;
	g_tintR.store(255); g_tintG.store(255); g_tintB.store(255);
	g_amount.store(1.0f); g_uniform.store(true);
    g_yaw.store(0.0f); g_pitch.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(g_rotationObjectMutex);
        for(const auto& original:g_rotationObjectBases) WriteUnitDirection(original.first,original.second);
        g_rotationObjectBases.clear();
    }
    ReleaseRotationCvar();
	g_bakedIndexForceNone.store(false, std::memory_order_relaxed);
	RestoreBakedIndexOriginals();
	SyncSunAppearance();
	SunFx_SetSceneLightRotation(LightFx_SunYaw(), LightFx_SunPitch());
}

void LightFx_SetSunYaw(float deg) {
	g_yaw.store(SunRotationMath::WrapYaw(deg));
	EnsureShadowRotationEnabled();
	SunFx_SetSceneLightRotation(LightFx_SunYaw(), LightFx_SunPitch());
}
void LightFx_SetSunPitch(float deg) {
	g_pitch.store(SunRotationMath::ClampPitch(deg));
	EnsureShadowRotationEnabled();
	SunFx_SetSceneLightRotation(LightFx_SunYaw(), LightFx_SunPitch());
}
float LightFx_SunYaw()   { return g_yaw.load(); }
void LightFx_SetSunReference(const float towardSun[3]) {
	if (!towardSun || !(SunRotationMath::Length({ towardSun[0], towardSun[1], towardSun[2] }) > 1.0e-4f)) return;
	g_referenceX.store(towardSun[0], std::memory_order_relaxed);
	g_referenceY.store(towardSun[1], std::memory_order_relaxed);
	g_referenceZ.store(towardSun[2], std::memory_order_relaxed);
	g_referenceValid.store(true, std::memory_order_relaxed);
}
bool LightFx_SunReference(float towardSun[3]) {
	if (!g_referenceValid.load(std::memory_order_relaxed)) return false;
	towardSun[0] = g_referenceX.load(std::memory_order_relaxed);
	towardSun[1] = g_referenceY.load(std::memory_order_relaxed);
	towardSun[2] = g_referenceZ.load(std::memory_order_relaxed);
	return true;
}
float LightFx_SunPitch() { return g_pitch.load(); }

void LightFx_GetTint(int* r, int* g, int* b) {
	if (r) *r = g_tintR.load();
	if (g) *g = g_tintG.load();
	if (b) *b = g_tintB.load();
}
float LightFx_Amount() { return g_amount.load(); }
bool  LightFx_Uniform() { return g_uniform.load(std::memory_order_relaxed); }
bool  LightFx_Enabled()   { return AnyOverride(); }
bool LightFx_SceneLightsEnabled(){return g_sceneLightsEnabled.load();}
void LightFx_SetEnvironmentSunSuppressed(bool suppressed) { g_environmentSunSuppressed.store(suppressed, std::memory_order_relaxed); }
void LightFx_SetSceneLightsEnabled(bool enabled){
    g_sceneLightsEnabled=enabled;
    MvmDebugLog_LinefAlways("world.light","sceneLightsEnabled=%d shadowSwitchesUnchanged=1 bakedLightingUnchanged=1",int(enabled));
}
unsigned long long LightFx_LightsSeen() { return g_lightsSeen.load(); }

void LightFx_AppendStateJson(std::ostringstream& o) {
	int r, g, b; LightFx_GetTint(&r, &g, &b);
	o << ",\"lightR\":" << r << ",\"lightG\":" << g << ",\"lightB\":" << b;
	o << ",\"lightAmount\":" << g_amount.load();
	o << ",\"engineLights\":" << (LightFx_SceneLightsEnabled()?"true":"false");
	o << ",\"sunYaw\":" << g_yaw.load();
	o << ",\"sunPitch\":" << g_pitch.load();
	o << ",\"lightUniform\":" << (g_uniform.load() ? "true" : "false");
	o << ",\"lightOk\":" << (g_installed ? "true" : "false");
	o << ",\"lightRotationOk\":" << (g_rotationInstalled ? "true" : "false");
	o << ",\"lightsSeen\":" << g_lightsSeen.load();
	o << ",\"lightsRotated\":" << g_lightsRotated.load();
	o << ",\"lightRotationBuilds\":" << g_rotationBuilds.load();
	o << ",\"lightShadowBuildParents\":" << g_shadowBuildParents.load();
	o << ",\"lightPersistentRotationWrites\":" << g_persistentRotationWrites.load();
	o << ",\"lightBakedIndexLog\":" << (g_bakedIndexLog.load() ? "true" : "false");
	o << ",\"lightBakedIndexForceNone\":" << (g_bakedIndexForceNone.load() ? "true" : "false");
	o << ",\"lightBakedIndexWrites\":" << g_bakedIndexWrites.load();
}

void LightFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world light <op> ...
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	std::string reason;
	if (!LightFx_Available(&reason) && *op) {
		advancedfx::Message("Scene lighting control unavailable: %s\n", reason.c_str());
		return;
	}

	if (0 == _stricmp(op, "reset")) {
		LightFx_Reset();
		advancedfx::Message("Scene lighting restored.\n");
		return;
	}
	if (0 == _stricmp(op,"enabled")) {
        const char *mode=argc>=5?args->ArgV(4):"";
        if(!strcmp(mode,"1")||!_stricmp(mode,"on"))LightFx_SetSceneLightsEnabled(true);
        else if(!strcmp(mode,"0")||!_stricmp(mode,"off"))LightFx_SetSceneLightsEnabled(false);
        else if(!_stricmp(mode,"toggle"))LightFx_SetSceneLightsEnabled(!LightFx_SceneLightsEnabled());
        else {advancedfx::Message("Usage: %s world light enabled on|off|toggle\n",cmd);return;}
        advancedfx::Message("Engine direct lights %s. Shadow switches and baked material lighting are unchanged.\n",LightFx_SceneLightsEnabled()?"on":"off");return;
    }

	if (0 == _stricmp(op, "probe")) {
		int n = (argc >= 5) ? atoi(args->ArgV(4)) : 6;
		if (n < 1) n = 1;
		if (n > 64) n = 64;   // this logs a line per light and MvmDebugLog flushes per line
		g_probeLights.store(n);
		advancedfx::Message(
			"Probing the next %d light object(s) into mvm_debug channel world.light.\n"
			"Reports every unit-length float triple, which is how the direction field is found.\n", n);
		return;
	}

	if (0 == _stricmp(op, "bakedindex")) {
		const char* mode = argc >= 5 ? args->ArgV(4) : "status";
		if (0 == _stricmp(mode, "log") || 0 == _stricmp(mode, "logging")) {
			const char* v = argc >= 6 ? args->ArgV(5) : "on";
			const bool on = !(0 == _stricmp(v, "off") || 0 == _stricmp(v, "0"));
			g_bakedIndexLog.store(on, std::memory_order_relaxed);
			advancedfx::Message("light bakedindex logging %s (mvm_debug world.light.bakedindex).\n",
				on ? "ON" : "off");
			return;
		}
		if (0 == _stricmp(mode, "force") || 0 == _stricmp(mode, "none")) {
			const char* v = argc >= 6 ? args->ArgV(5) : "on";
			const bool on = !(0 == _stricmp(v, "off") || 0 == _stricmp(v, "0"));
			g_bakedIndexForceNone.store(on, std::memory_order_relaxed);
			if (!on) RestoreBakedIndexOriginals();
			MvmDebugLog_LinefAlways("world.light.bakedindex", "forceNone=%d writes=%llu",
				on ? 1 : 0, g_bakedIndexWrites.load());
			advancedfx::Message(
				"light bakedindex force-none %s (writes scene light +0x%llX = 0xFF).\n",
				on ? "ON" : "off / restored",
				(unsigned long long)kOffBakedShadowIndex);
			return;
		}
		advancedfx::Message(
			"light bakedindex: log=%s forceNone=%s writes=%llu originals=%zu offset=0x%llX\n"
			"Usage: %s world light bakedindex log on|off\n"
			"       %s world light bakedindex force on|off\n",
			g_bakedIndexLog.load() ? "on" : "off",
			g_bakedIndexForceNone.load() ? "on" : "off",
			g_bakedIndexWrites.load(), g_bakedIndexOriginals.size(),
			(unsigned long long)kOffBakedShadowIndex, cmd, cmd);
		return;
	}

	if (0 == _stricmp(op, "component") || 0 == _stricmp(op, "components")) {
		ProbeLightComponents();
		return;
	}

	if (0 == _stricmp(op, "mode")) {
		if (argc >= 5) {
			const char* value = args->ArgV(4);
			LightFx_SetUniform(0 == _stricmp(value, "uniform") || 0 == _stricmp(value, "absolute"));
		}
		advancedfx::Message(
			"Light mode: %s.\n"
			"  uniform  - chosen hue, preserving each light's original peak brightness\n"
			"  filtered - channel filter over the map's authored light colour\n"
			"Neither mode amplifies HDR radiance.\n",
			g_uniform.load() ? "uniform" : "filtered");
		return;
	}

	if (0 == _stricmp(op, "tint")) {
		if (argc >= 7) {
			LightFx_SetTint(atoi(args->ArgV(4)), atoi(args->ArgV(5)), atoi(args->ArgV(6)));
			const bool quiet = (argc >= 8) && (0 == _stricmp(args->ArgV(7), "quiet"));
			int r, g, b; LightFx_GetTint(&r, &g, &b);
			if (!quiet) advancedfx::Message("Light tint %d %d %d.\n", r, g, b);
			return;
		}
		int r, g, b; LightFx_GetTint(&r, &g, &b);
		advancedfx::Message("Light tint is %d %d %d (255 255 255 = untouched).\n", r, g, b);
		return;
	}

	if (0 == _stricmp(op, "amount") || 0 == _stricmp(op, "intensity")) {
		if (argc >= 5) {
			LightFx_SetAmount((float)atof(args->ArgV(4)));
			const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (!quiet) advancedfx::Message("Light tint amount %.2f.\n", LightFx_Amount());
			return;
		}
		advancedfx::Message("Light tint amount is %.2f.\n", LightFx_Amount());
		return;
	}

	if (0 == _stricmp(op, "yaw") || 0 == _stricmp(op, "pitch")) {
		std::string rotationReason;
		if (!LightFx_RotationAvailable(&rotationReason)) {
			advancedfx::Message("Scene light rotation unavailable: %s\n", rotationReason.c_str());
			return;
		}
		const bool isYaw = 0 == _stricmp(op, "yaw");
		if (argc >= 5) {
			if (isYaw) LightFx_SetSunYaw((float)atof(args->ArgV(4)));
			else       LightFx_SetSunPitch((float)atof(args->ArgV(4)));
			const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (!quiet) advancedfx::Message("Scene light %s %.1f deg.\n",
				isYaw ? "yaw" : "pitch", isYaw ? LightFx_SunYaw() : LightFx_SunPitch());
			return;
		}
		advancedfx::Message("Scene light %s is %.1f deg.\n", isYaw ? "yaw" : "pitch",
			isYaw ? LightFx_SunYaw() : LightFx_SunPitch());
		return;
	}

	int r, g, b; LightFx_GetTint(&r, &g, &b);
	advancedfx::Message(
		"mirv_filmmaker world light - tints every light the scene system submits.\n"
		"Usage:\n"
		"  %s world light tint <r> <g> <b>   - 0..255 each; 255 255 255 is untouched\n"
        "  mirv_filmmaker world light enabled on|off|toggle - direct lights only\n"
		"  %s world light amount <0..1>      - blend from authored light to chosen tint\n"
		"  %s world light mode <uniform|filtered>\n"
		"  %s world light components       - inspect live light baked-shadow fields\n"
		"  %s world light yaw <degrees>     - rotate the engine light around Z\n"
		"  %s world light pitch <degrees>   - raise/lower the engine light (-90..90)\n"
		"  %s world light reset\n"
		"Both modes preserve the source light's HDR ceiling instead of adding glow.\n"
		"Current: tint %d %d %d, amount %.2f, mode %s | yaw %.1f pitch %.1f | hook %s, %llu light submissions, %llu rotations.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, r, g, b, LightFx_Amount(),
		g_uniform.load() ? "uniform" : "filtered",
		g_yaw.load(), g_pitch.load(),
		g_installed ? "installed" : "NOT installed",
		LightFx_LightsSeen(), g_lightsRotated.load());
}

} // namespace Filmmaker

