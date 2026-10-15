#include "WorldTransient.h"
#include "WorldModulation.h"
#include "MaterialTintOwnership.h"
#include "WorldTintMath.h"

#include "../Debug/MvmDebugLog.h"
#include "../../SceneSystem.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Filmmaker {

namespace {

constexpr int kCount = (int)ModCategory::Count;

// Published from the main thread, read on the render thread. Packed so a category's colour is
// read in ONE atomic load -- three separate channel atomics could tear into a colour that was
// never set, which on a full-screen tint is a visible flash.
std::atomic<unsigned int> g_rgb[kCount] = {
	{ 0xFFFFFFu }, { 0xFFFFFFu }, { 0xFFFFFFu }, { 0xFFFFFFu }
};
std::atomic<bool> g_on[kCount] = { {false}, {false}, {false}, {false} };
std::atomic<bool> g_anyOn{ false };
// 0 = untouched, 1 = full tint.
std::atomic<float> g_strength{ 1.0f };

struct MaterialTintSnapshot {
	CMaterial2* material = nullptr;
	float* param = nullptr;
	float authored[4] = { 1, 1, 1, 1 };
	float applied[4] = { 1, 1, 1, 1 };
};

struct CharacterSheenSnapshot {
	CMaterial2* material = nullptr;
	float* param = nullptr;
	float authored[4] = { 1, 1, 1, 1 };
	float applied[4] = { 1, 1, 1, 1 };
};

struct CharacterModelTintSnapshot {
	CMaterial2* material = nullptr;
	float* param = nullptr;
	float authored = 1.0f;
	float applied = 1.0f;
};

std::unordered_map<std::string, MaterialTintSnapshot> g_materialSnapshots;

// Every float4 we have written -> the authored value it replaced. CS2 regenerates materials (weapon
// skin composites, streamed compmats) under NEW names by copying the CURRENT parameter of their
// source, so a copy made while the tint is on carries our tinted value. Without this map such a
// copy was snapshotted with the tint as its "authored" colour: tinted twice while on, and "restored"
// to the tint on reset -- the weapons that stayed purple after Reset. Keys are exact bit patterns.
struct Float4Key {
	uint32_t v[4];
	bool operator==(const Float4Key& o) const { return 0 == std::memcmp(v, o.v, sizeof(v)); }
};
struct Float4KeyHash {
	size_t operator()(const Float4Key& k) const {
		size_t h = 1469598103934665603ull;
		for (uint32_t x : k.v) { h ^= x; h *= 1099511628211ull; }
		return h;
	}
};
struct AuthoredValue { float v[4]; };
std::unordered_map<Float4Key, AuthoredValue, Float4KeyHash> g_appliedToAuthored;

Float4Key KeyOf(const float f[4]) { Float4Key k; std::memcpy(k.v, f, sizeof(k.v)); return k; }

void RememberApplied(const float applied[4], const float authored[4]) {
	if (g_appliedToAuthored.size() > 8192) g_appliedToAuthored.clear(); // bounded; a fresh apply refills it
	AuthoredValue a; std::memcpy(a.v, authored, sizeof(a.v));
	g_appliedToAuthored[KeyOf(applied)] = a;
}

// The authored value behind `current` when `current` is one of our writes.
bool AuthoredForApplied(const float current[4], float out[4]) {
	auto it = g_appliedToAuthored.find(KeyOf(current));
	if (it == g_appliedToAuthored.end()) return false;
	std::memcpy(out, it->second.v, sizeof(it->second.v));
	return true;
}
int g_carriedSnapshots = 0;  // diagnostics: copies whose authored value was recovered
int g_orphanRestores = 0;
std::unordered_map<std::string, CharacterSheenSnapshot> g_characterSheenSnapshots;
std::unordered_map<std::string, CharacterModelTintSnapshot> g_characterModelTintSnapshots;
unsigned long long g_lastMaterialApplyMs = 0;
unsigned long long g_materialStableSinceMs = 0;
uint64_t g_observedMaterialCount = 0;
uint64_t g_lastLoggedMaterialCount = 0;
bool g_materialDirty = false;
MaterialTintOwnership g_materialTintOwnership;
std::atomic<unsigned long long> g_fallbackDraws{ 0 };

constexpr unsigned long long kMaterialSettleMs = 750;
constexpr unsigned long long kMaterialReassertMs = 2000;

void RecomputeAny() {
	bool any = false;
	for (int i = 0; i < kCount; ++i) if (g_on[i].load()) { any = true; break; }
	g_anyOn.store(any);
}

bool Contains(const char* hay, const char* needle) {
	return hay && needle && strstr(hay, needle) != nullptr;
}

ModCategory Classify(const char* name) {
	// Sun and clouds first: they live under materials/effects/ too, so an "effects" test ahead of
	// this one would swallow them and the sky would follow the particle tint.
	if (Contains(name, "sun") || Contains(name, "clouds") || Contains(name, "skybox"))
		return ModCategory::SunClouds;
	if (Contains(name, "effects") || Contains(name, "particle"))
		return ModCategory::Effects;
	// Props vs world is not reliably separable from the material name alone -- CS2 uses the same
	// materials on both -- so both land in World and Props is offered as a separate knob only for
	// the material families that are clearly models.
	if (Contains(name, "models/"))
		return ModCategory::Props;
	return ModCategory::World;
}

const char* CategoryKey(ModCategory c) {
	switch (c) {
	case ModCategory::World:     return "world";
	case ModCategory::Props:     return "props";
	case ModCategory::SunClouds: return "sunclouds";
	case ModCategory::Effects:   return "effects";
	default: return "?";
	}
}

bool ParseCategory(const char* s, ModCategory* out) {
	for (int i = 0; i < kCount; ++i) {
		if (0 == _stricmp(s, CategoryKey((ModCategory)i))) { *out = (ModCategory)i; return true; }
	}
	return false;
}

int ClampI(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

bool ColorForCategory(ModCategory c, unsigned char outRgba[4]) {
	const int i = (int)c;
	if (i < 0 || i >= kCount || !WorldTransient::Number("worldTint", g_on[i].load(std::memory_order_relaxed), i)) return false;

	const unsigned int packed = g_rgb[i].load(std::memory_order_relaxed);
	const float s = float(WorldTransient::Number("worldStrength", g_strength.load(std::memory_order_relaxed)));
	auto mix = [s](unsigned int channel) -> unsigned char {
		const int v = (int)(255.0f + ((float)channel - 255.0f) * s + 0.5f);
		return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
	};
	outRgba[0] = mix(unsigned(WorldTransient::Number("worldColors", (packed >> 16) & 0xFF, i * 3 + 0)));
	outRgba[1] = mix(unsigned(WorldTransient::Number("worldColors", (packed >> 8) & 0xFF, i * 3 + 1)));
	outRgba[2] = mix(unsigned(WorldTransient::Number("worldColors", packed & 0xFF, i * 3 + 2)));
	outRgba[3] = 255;
	return true;
}

bool UsesMaterialPath(ModCategory c) {
	return c == ModCategory::World || c == ModCategory::Props || c == ModCategory::SunClouds;
}

bool AnyMaterialTintEnabled() {
	return WorldTransient::Number("worldTint", g_on[(int)ModCategory::World].load(std::memory_order_relaxed), 0)
		|| WorldTransient::Number("worldTint", g_on[(int)ModCategory::Props].load(std::memory_order_relaxed), 1)
		|| WorldTransient::Number("worldTint", g_on[(int)ModCategory::SunClouds].load(std::memory_order_relaxed), 2);
}

// POD-only wrappers: MSVC does not permit __try in functions that need C++ object unwinding.
static bool ReadFloat4Raw(const float* p, float out[4]) {
	__try {
		for (int i = 0; i < 4; ++i) out[i] = p[i];
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteFloat4Raw(float* p, const float value[4]) {
	__try {
		for (int i = 0; i < 4; ++i) p[i] = value[i];
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadFloatRaw(const float* p, float* out) {
	__try {
		*out = *p;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteFloatRaw(float* p, float value) {
	__try {
		*p = value;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Float4Equal(const float a[4], const float b[4]) {
	return 0 == std::memcmp(a, b, sizeof(float) * 4);
}

static bool FloatEqual(float a, float b) {
	return 0 == std::memcmp(&a, &b, sizeof(float));
}

// Material streaming mutates the resource-system array asynchronously during level load. Every
// engine call is an AV boundary: a half-constructed entry is skipped, never allowed to kill CS2.
// These wrappers stay POD-only because MSVC forbids __try in functions that unwind C++ objects.
static bool GetMaterialsRaw(GetMaterialsArrayResult* out) {
	__try {
		g_pCResourceSystem->GetMaterials(out);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadMaterialAtRaw(CMaterial2*** slots, uint64_t index, CMaterial2** out) {
	__try {
		*out = (slots && slots[index]) ? *slots[index] : nullptr;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return false; }
}

static bool CopyMaterialNameRaw(CMaterial2* material, char* out, size_t capacity) {
	__try {
		if (!out || capacity < 2) return false;
		const char* source = material ? material->GetName() : nullptr;
		if (!source) { out[0] = '\0'; return true; }
		size_t i = 0;
		for (; i + 1 < capacity && source[i]; ++i) out[i] = source[i];
		out[i] = '\0';
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { if (out && capacity) out[0] = '\0'; return false; }
}

static bool FindMaterialParamRaw(CMaterial2* material, const char* name, float** out) {
	__try {
		*out = SceneSystem_FindMaterialParam(material, name);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return false; }
}

static bool UpdateMaterialRaw(CMaterial2* material) {
	__try {
		SceneSystem_UpdateMaterial(material);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsCharacterMaterial(const char* name) {
	return Contains(name, "characters/models/") || Contains(name, "agents/models/");
}

// csgo_character.vfx has two colour paths beyond g_vColorTint. Some gloves explicitly ship with
// g_flModelTintAmount=0, which disables model tint, and armor/sleeves can light glossy lobes
// through the separate g_flSheenTintColor. These are supported named parameters, not offsets.
bool ApplyCharacterMaterialExtras(CMaterial2* material, const std::string& name,
	const float tintSrgb[3], bool restore, bool* outChanged, int* faults) {
	*outChanged = false;
	if (!IsCharacterMaterial(name.c_str())) return true;

	float* sheenParam = nullptr;
	float* modelAmountParam = nullptr;
	if (!FindMaterialParamRaw(material, "g_flSheenTintColor", &sheenParam)
		|| !FindMaterialParamRaw(material, "g_flModelTintAmount", &modelAmountParam)) {
		++*faults;
		return false;
	}

	bool wroteSheen = false;
	bool wroteAmount = false;
	CharacterSheenSnapshot* sheenSnap = nullptr;
	CharacterModelTintSnapshot* amountSnap = nullptr;
	float logAmountAuthored = -1.0f;
	float logAmountApplied = -1.0f;

	if (sheenParam) {
		auto it = g_characterSheenSnapshots.find(name);
		if (restore) {
			float current[4];
			const CharacterSheenSnapshot* restoreSnap = nullptr;
			if (ReadFloat4Raw(sheenParam, current)) {
				if (it != g_characterSheenSnapshots.end()
					&& it->second.material == material && it->second.param == sheenParam
					&& Float4Equal(current, it->second.applied)) {
					restoreSnap = &it->second;
				} else {
					for (const auto& entry : g_characterSheenSnapshots) {
						if (Float4Equal(current, entry.second.applied)) {
							restoreSnap = &entry.second;
							break;
						}
					}
				}
				if (restoreSnap && !Float4Equal(current, restoreSnap->authored)
					&& WriteFloat4Raw(sheenParam, restoreSnap->authored)) wroteSheen = true;
			}
			if (it != g_characterSheenSnapshots.end()) g_characterSheenSnapshots.erase(it);
		} else {
			if (it == g_characterSheenSnapshots.end()
				|| it->second.material != material || it->second.param != sheenParam) {
				float current[4];
				if (!ReadFloat4Raw(sheenParam, current)) { ++*faults; return false; }
				CharacterSheenSnapshot snap;
				snap.material = material;
				snap.param = sheenParam;
				const CharacterSheenSnapshot* inherited = nullptr;
				for (const auto& entry : g_characterSheenSnapshots) {
					if (Float4Equal(current, entry.second.applied)) { inherited = &entry.second; break; }
				}
				std::memcpy(snap.authored, inherited ? inherited->authored : current,
					sizeof(snap.authored));
				it = g_characterSheenSnapshots.insert_or_assign(name, snap).first;
			}
			sheenSnap = &it->second;
			const float value[4] = {
				ComposeWorldSrgbTint(sheenSnap->authored[0], tintSrgb[0]),
				ComposeWorldSrgbTint(sheenSnap->authored[1], tintSrgb[1]),
				ComposeWorldSrgbTint(sheenSnap->authored[2], tintSrgb[2]),
				sheenSnap->authored[3]
			};
			std::memcpy(sheenSnap->applied, value, sizeof(value));
			float current[4];
			if (!ReadFloat4Raw(sheenParam, current)) { ++*faults; return false; }
			if (!Float4Equal(current, value)) {
				if (!WriteFloat4Raw(sheenParam, value)) { ++*faults; return false; }
				wroteSheen = true;
			}
		}
	}

	if (modelAmountParam) {
		auto it = g_characterModelTintSnapshots.find(name);
		if (restore) {
			float current = 0.0f;
			const CharacterModelTintSnapshot* restoreSnap = nullptr;
			if (ReadFloatRaw(modelAmountParam, &current)) {
				if (it != g_characterModelTintSnapshots.end()
					&& it->second.material == material && it->second.param == modelAmountParam
					&& FloatEqual(current, it->second.applied)) {
					restoreSnap = &it->second;
				} else {
					for (const auto& entry : g_characterModelTintSnapshots) {
						if (FloatEqual(current, entry.second.applied)) {
							restoreSnap = &entry.second;
							break;
						}
					}
				}
				if (restoreSnap && !FloatEqual(current, restoreSnap->authored)
					&& WriteFloatRaw(modelAmountParam, restoreSnap->authored)) wroteAmount = true;
				if (restoreSnap) {
					logAmountAuthored = restoreSnap->authored;
					logAmountApplied = restoreSnap->applied;
				}
			}
			if (it != g_characterModelTintSnapshots.end()) g_characterModelTintSnapshots.erase(it);
		} else {
			if (it == g_characterModelTintSnapshots.end()
				|| it->second.material != material || it->second.param != modelAmountParam) {
				float current = 0.0f;
				if (!ReadFloatRaw(modelAmountParam, &current)) { ++*faults; return false; }
				CharacterModelTintSnapshot snap;
				snap.material = material;
				snap.param = modelAmountParam;
				const CharacterModelTintSnapshot* inherited = nullptr;
				for (const auto& entry : g_characterModelTintSnapshots) {
					if (FloatEqual(current, entry.second.applied)) { inherited = &entry.second; break; }
				}
				snap.authored = inherited ? inherited->authored : current;
				it = g_characterModelTintSnapshots.insert_or_assign(name, snap).first;
			}
			amountSnap = &it->second;
			logAmountAuthored = amountSnap->authored;
			const bool tintChangesColor = tintSrgb[0] < 0.99999f
				|| tintSrgb[1] < 0.99999f || tintSrgb[2] < 0.99999f;
			amountSnap->applied = tintChangesColor ? 1.0f : amountSnap->authored;
			logAmountApplied = amountSnap->applied;
			float current = 0.0f;
			if (!ReadFloatRaw(modelAmountParam, &current)) { ++*faults; return false; }
			if (!FloatEqual(current, amountSnap->applied)) {
				if (!WriteFloatRaw(modelAmountParam, amountSnap->applied)) { ++*faults; return false; }
				wroteAmount = true;
			}
		}
	}

	if (!wroteSheen && !wroteAmount) return true;
	if (!UpdateMaterialRaw(material)) {
		if (wroteSheen && sheenSnap) WriteFloat4Raw(sheenParam, sheenSnap->authored);
		if (wroteAmount && amountSnap) WriteFloatRaw(modelAmountParam, amountSnap->authored);
		++*faults;
		return false;
	}
	*outChanged = true;
	if (MvmDebugLog_Active()) {
		static unsigned s_characterLog = 0;
		if ((s_characterLog++ % 64u) == 0) {
			MvmDebugLog_Linef("world.mod.material.character",
				"name='%s' mat=%p sheenPtr=%p sheenChanged=%d modelAmountPtr=%p "
				"modelAmountAuthored=%.3f modelAmountWrote=%.3f modelAmountChanged=%d restore=%d",
				name.c_str(), (void*)material, (void*)sheenParam, wroteSheen ? 1 : 0,
				(void*)modelAmountParam, logAmountAuthored, logAmountApplied,
				wroteAmount ? 1 : 0, restore ? 1 : 0);
		}
	}
	return true;
}

// ---- MATERIAL PARAMETER PROBE ---------------------------------------------------------------
// Aggregate geometry ignores CMeshDrawPrimitive_t::color. Before touching another unknown
// aggregate field, ask the material system whether the loaded map materials expose a supported
// colour parameter. This is the same read-only mechanism already proven by CloudFx and SunFx.
void ProbeMaterialParams(int maxLines) {
	if (maxLines < 1) maxLines = 1;
	if (maxLines > 128) maxLines = 128;

	GetMaterialsArrayResult materials = { 0 };
	if (!g_pCResourceSystem) {
		MvmDebugLog_LinefAlways("world.mod.material", "probe unavailable: resource system is null");
		return;
	}
	if (!GetMaterialsRaw(&materials)) {
		MvmDebugLog_LinefAlways("world.mod.material", "probe skipped: GetMaterials faulted");
		return;
	}

	const char* params[] = {
		"g_vColorTint", "g_vColor", "g_vTint", "g_vBaseColor", "g_vDiffuseColor"
	};
	int hits = 0;
	int logged = 0;
	for (uint64_t i = 0; i < materials.count; ++i) {
		CMaterial2* mat = nullptr;
		if (!ReadMaterialAtRaw(materials.pArrMaterials, i, &mat) || !mat) continue;
		char name[512];
		if (!CopyMaterialNameRaw(mat, name, sizeof(name)) || !name[0]) continue;

		for (const char* paramName : params) {
			float* p = nullptr;
			if (!FindMaterialParamRaw(mat, paramName, &p)) continue;
			if (!p) continue;
			++hits;
			if (logged >= maxLines) continue;

			float v[4] = { 0, 0, 0, 0 };
			const bool readable = ReadFloat4Raw(p, v);
			MvmDebugLog_LinefAlways("world.mod.material",
				"mat=%p name='%s' category=%s param=%s ptr=%p %s value=(%.4f %.4f %.4f %.4f)",
				(void*)mat, name, CategoryKey(Classify(name)), paramName, (void*)p,
				readable ? "READ-OK" : "READ-FAULT", v[0], v[1], v[2], v[3]);
			++logged;
		}
	}

	MvmDebugLog_LinefAlways("world.mod.material",
		"probe summary materials=%llu parameterHits=%d logged=%d limit=%d",
		(unsigned long long)materials.count, hits, logged, maxLines);
}

// Every live world/prop/sky material and whether the tint can reach it. A material with no
// g_vColorTint is skipped by ApplyMaterialTints, so the surfaces it draws keep their authored colour:
// this lists them so a missing tint can be traced to a shader family instead of guessed at.
void ProbeUntinted(int maxLines) {
	if (maxLines < 1) maxLines = 1;
	if (maxLines > 2000) maxLines = 2000;
	GetMaterialsArrayResult materials = { 0 };
	if (!g_pCResourceSystem || !GetMaterialsRaw(&materials)) {
		MvmDebugLog_LinefAlways("world.mod.untinted", "probe unavailable");
		return;
	}
	int total = 0, tinted = 0, untinted = 0, skippedCategory = 0, faulted = 0, logged = 0;
	int nullEntries = 0, nameFault = 0, nameless = 0, namelessTintable = 0;
	for (uint64_t i = 0; i < materials.count; ++i) {
		CMaterial2* mat = nullptr;
		if (!ReadMaterialAtRaw(materials.pArrMaterials, i, &mat) || !mat) { ++nullEntries; continue; }
		char name[512];
		if (!CopyMaterialNameRaw(mat, name, sizeof(name))) { ++nameFault; continue; }
		if (!name[0]) {
			// ApplyMaterialTints skips these silently. Do they carry a tint parameter?
			++nameless;
			float* q = nullptr;
			if (FindMaterialParamRaw(mat, "g_vColorTint", &q) && q) {
				++namelessTintable;
				if (namelessTintable <= 12) {
					float v[4] = {}; ReadFloat4Raw(q, v);
					MvmDebugLog_LinefAlways("world.mod.untinted", "NAMELESS mat=%p g_vColorTint=(%.3f %.3f %.3f %.3f)",
						(void*)mat, v[0], v[1], v[2], v[3]);
				}
			}
			continue;
		}
		++total;
		const ModCategory category = Classify(name);
		if (!UsesMaterialPath(category)) { ++skippedCategory; continue; }
		float* p = nullptr;
		if (!FindMaterialParamRaw(mat, "g_vColorTint", &p)) { ++faulted; continue; }
		if (p) { ++tinted; continue; }
		++untinted;
		if (logged < maxLines) {
			// Other colour-like parameters this material might expose instead (name lookups only).
			static const char* kAlternatives[] = {
				"g_vColor", "g_vTint", "g_vBaseColor", "g_vDiffuseColor", "g_vColorTint2", "g_vColorTint1",
				"g_vColorTintA", "g_vColorTintB", "g_vLayer1Tint", "g_vLayer2Tint", "g_vMaskedColor",
				"g_flTintAmount", "g_flModelTintAmount", "g_vTexCoordScale", "g_fTextureColorBrightness"
			};
			char found[256]; found[0] = 0;
			for (const char* alt : kAlternatives) {
				float* q = nullptr;
				if (FindMaterialParamRaw(mat, alt, &q) && q) {
					strncat_s(found, sizeof(found), alt, _TRUNCATE);
					strncat_s(found, sizeof(found), " ", _TRUNCATE);
				}
			}
			MvmDebugLog_LinefAlways("world.mod.untinted", "category=%s mat=%p name='%s' has:[%s]",
				CategoryKey(category), (void*)mat, name, found);
			++logged;
		}
	}
	MvmDebugLog_LinefAlways("world.mod.untinted",
		"summary materials=%d tintable=%d NOT-tintable=%d otherCategory=%d faulted=%d logged=%d | array=%llu nullEntries=%d nameFault=%d NAMELESS=%d namelessTintable=%d",
		total, tinted, untinted, skippedCategory, faulted, logged,
		(unsigned long long)materials.count, nullEntries, nameFault, nameless, namelessTintable);
}

void ApplyMaterialTints(bool restoreAll) {
	GetMaterialsArrayResult materials = { 0 };
	if (!g_pCResourceSystem) return;
	if (!GetMaterialsRaw(&materials)) {
		MvmDebugLog_Linef("world.mod.material", "apply skipped: GetMaterials faulted");
		return;
	}
	g_lastMaterialApplyMs = GetTickCount64();
	const bool countChanged = g_lastLoggedMaterialCount != materials.count;
	g_lastLoggedMaterialCount = materials.count;

	int applied = 0;
	int restored = 0;
	int faults = 0;
	bool loggedWrite = false;
	int ownershipOverflow = 0;
	std::unordered_set<std::string> liveNames;
	g_materialTintOwnership.BeginUpdate();
	for (uint64_t i = 0; i < materials.count; ++i) {
		CMaterial2* mat = nullptr;
		if (!ReadMaterialAtRaw(materials.pArrMaterials, i, &mat)) { ++faults; continue; }
		if (!mat) continue;
		char rawName[512];
		if (!CopyMaterialNameRaw(mat, rawName, sizeof(rawName))) { ++faults; continue; }
		if (!rawName[0]) continue;
		// Aggregate world geometry, model-backed props/foliage, and HDR sky layers all ignore or
		// incompletely honor the per-draw colour. Use their supported named shader parameter and
		// reserve the per-draw path for transient particle/effect draws.
		const ModCategory category = Classify(rawName);
		if (!UsesMaterialPath(category)) continue;
		const std::string name(rawName);
		liveNames.insert(name);
		float* p = nullptr;
		if (!FindMaterialParamRaw(mat, "g_vColorTint", &p)) { ++faults; continue; }

		unsigned char rgba[4] = { 255, 255, 255, 255 };
		const bool shouldApply = !restoreAll && ColorForCategory(category, rgba);
		auto it = g_materialSnapshots.find(name);

		if (shouldApply) {
			// Character accessories are not guaranteed to expose g_vColorTint. Apply the
			// shader's secondary tint controls even when the primary parameter is absent;
			// otherwise boots, gloves, and armor plates keep their authored colour.
			const bool character = IsCharacterMaterial(name.c_str());
			const float linearTint[3] = {
				WorldSrgbByteToLinear(rgba[0]),
				WorldSrgbByteToLinear(rgba[1]),
				WorldSrgbByteToLinear(rgba[2])
			};
			const float characterTint[3] = {
				(float)rgba[0] / 255.0f,
				(float)rgba[1] / 255.0f,
				(float)rgba[2] / 255.0f
			};
			const float* shaderTint = characterTint;
			bool extrasChanged = false;
			if (!ApplyCharacterMaterialExtras(mat, name, shaderTint, false, &extrasChanged, &faults))
				continue;
			if (extrasChanged) ++applied;
			if (!p) continue;
			// A map transition can load a new material object under a familiar name. Resnapshot when
			// either identity changes. Some streamed compmat materials inherit the CURRENT parameter
			// from a tinted source object but receive a new generated name. If the new value exactly
			// matches ANY float4 we last applied, carry that source's original authored value forward
			// instead of snapshotting the tint as authored and multiplying it a second time. A
			// different value is engine-owned and is snapshotted normally, so authored colour is never
			// copied across an unrelated replacement.
			if (it == g_materialSnapshots.end() || it->second.material != mat || it->second.param != p) {
				MaterialTintSnapshot snap;
				snap.material = mat;
				snap.param = p;
				float current[4];
				if (!ReadFloat4Raw(p, current)) continue;
				// A copy that inherited one of our writes gets its source's authored value back.
				if (AuthoredForApplied(current, snap.authored)) {
					++g_carriedSnapshots;
					if (MvmDebugLog_Active())
						MvmDebugLog_Linef("world.mod.material.carry",
							"name='%s' inherited=(%.4f %.4f %.4f) authored=(%.4f %.4f %.4f)", name.c_str(),
							current[0], current[1], current[2], snap.authored[0], snap.authored[1], snap.authored[2]);
				} else {
					std::memcpy(snap.authored, current, sizeof(snap.authored));
				}
				it = g_materialSnapshots.insert_or_assign(name, snap).first;
			}

			MaterialTintSnapshot& snap = it->second;
			// Valve's shader declarations mark g_vColorTint as `SrgbRead(true)`: the material
			// parameter is stored in display/sRGB space and the shader converts it once when it
			// binds the uniform. Keep that contract here. The selected UI colour is multiplied in
			// linear space, then encoded back to sRGB before writing the material.
			const float value[4] = {
				// Character materials already use g_flModelTintAmount as the shader's uniform
				// model-colour path, so write the selected sRGB colour directly. For map/sky
				// materials preserve their authored tint while applying the selected colour in
				// linear space.
				character ? shaderTint[0] : ComposeWorldSrgbTint(snap.authored[0], characterTint[0]),
				character ? shaderTint[1] : ComposeWorldSrgbTint(snap.authored[1], characterTint[1]),
				character ? shaderTint[2] : ComposeWorldSrgbTint(snap.authored[2], characterTint[2]),
				snap.authored[3]
			};
			std::memcpy(snap.applied, value, sizeof(value));
			RememberApplied(value, snap.authored);
			float current[4];
			if (!ReadFloat4Raw(p, current)) continue;
			if (Float4Equal(current, value)) {
				if (!g_materialTintOwnership.Add(name.c_str())) ++ownershipOverflow;
				continue;
			}
			const bool wrote = WriteFloat4Raw(p, value);
			if (wrote) {
				if (UpdateMaterialRaw(mat)) {
					++applied;
					if (!g_materialTintOwnership.Add(name.c_str())) ++ownershipOverflow;
					if (!loggedWrite && MvmDebugLog_Active()) {
						MvmDebugLog_Linef("world.mod.material.write",
							"category=%s mat=%p name='%s' param=g_vColorTint ptr=%p "
							"uiRGB=(%u %u %u) linear=(%.4f %.4f %.4f) "
							"authored=(%.4f %.4f %.4f %.4f) wrote=(%.4f %.4f %.4f %.4f)",
							CategoryKey(category), (void*)mat, name.c_str(), (void*)p,
							(unsigned)rgba[0], (unsigned)rgba[1], (unsigned)rgba[2],
							linearTint[0], linearTint[1], linearTint[2],
							snap.authored[0], snap.authored[1], snap.authored[2], snap.authored[3],
							value[0], value[1], value[2], value[3]);
						loggedWrite = true;
					}
				}
				else { WriteFloat4Raw(p, snap.authored); ++faults; }
			}
			continue;
		}

		const float restoreTint[3] = { 1.0f, 1.0f, 1.0f };
		bool extrasChanged = false;
		if (!ApplyCharacterMaterialExtras(mat, name, restoreTint, true, &extrasChanged, &faults))
			continue;
		if (extrasChanged) ++restored;
		if (!p) continue;
		if (it == g_materialSnapshots.end()) {
			// Never snapshotted under this name, yet it may hold one of our writes: a regenerated copy
			// (weapon skin composite) made while the tint was on. Put its source's authored value back.
			float current[4], authored[4];
			if (ReadFloat4Raw(p, current) && AuthoredForApplied(current, authored)
				&& WriteFloat4Raw(p, authored)) {
				if (UpdateMaterialRaw(mat)) { ++restored; ++g_orphanRestores; }
				else ++faults;
				if (MvmDebugLog_Active())
					MvmDebugLog_Linef("world.mod.material.orphan", "restored name='%s' to (%.4f %.4f %.4f)",
						name.c_str(), authored[0], authored[1], authored[2]);
			}
			continue;
		}
		float current[4];
		const MaterialTintSnapshot* restoreSnap = nullptr;
		if (ReadFloat4Raw(p, current)) {
			if (it->second.material == mat && it->second.param == p
				&& Float4Equal(current, it->second.applied)) {
				restoreSnap = &it->second;
			}

			if (restoreSnap && !Float4Equal(current, restoreSnap->authored)
				&& WriteFloat4Raw(p, restoreSnap->authored)) {
				if (UpdateMaterialRaw(mat)) ++restored;
				else ++faults;
			}
			// Same name, new object (the composite was rebuilt) still holding one of our writes.
			float authored[4];
			if (!restoreSnap && AuthoredForApplied(current, authored) && WriteFloat4Raw(p, authored)) {
				if (UpdateMaterialRaw(mat)) { ++restored; ++g_orphanRestores; }
				else ++faults;
			}
		}
		g_materialSnapshots.erase(it);
	}

	// Material objects from the previous map are intentionally forgotten without dereferencing
	// their pointers. Anything still live remains in GetMaterials and was handled above.
	for (auto it = g_materialSnapshots.begin(); it != g_materialSnapshots.end();) {
		if (restoreAll || liveNames.find(it->first) == liveNames.end())
			it = g_materialSnapshots.erase(it);
		else
			++it;
	}
	for (auto it = g_characterSheenSnapshots.begin(); it != g_characterSheenSnapshots.end();) {
		if (restoreAll || liveNames.find(it->first) == liveNames.end())
			it = g_characterSheenSnapshots.erase(it);
		else
			++it;
	}
	for (auto it = g_characterModelTintSnapshots.begin();
		it != g_characterModelTintSnapshots.end();) {
		if (restoreAll || liveNames.find(it->first) == liveNames.end())
			it = g_characterModelTintSnapshots.erase(it);
		else
			++it;
	}
	g_materialTintOwnership.Publish();
	g_materialDirty = false;
	if (applied || restored || faults || countChanged || ownershipOverflow) {
		MvmDebugLog_Linef("world.mod.material",
			"%s materials=%llu applied=%d restored=%d faults=%d ownershipOverflow=%d snapshots=%llu carried=%d orphanRestores=%d",
			restoreAll ? "restore" : "apply", (unsigned long long)materials.count,
			applied, restored, faults, ownershipOverflow,
			(unsigned long long)g_materialSnapshots.size(), g_carriedSnapshots, g_orphanRestores);
	}
}

// ---- STRIDE PROBE -------------------------------------------------------------------------
// CBaseSceneData is recorded as size 0x68 from the 2024 reference, but its colour field moved
// 0x40 -> 0x50 in a CS2 update, so the struct GREW and 0x68 can no longer be right. Indexing the
// batch on a wrong stride crashed CS2 instantly.
//
// Rather than guess again: for a batch with count > 1, read what WOULD be element 1's
// sceneObject (+0x18) and material (+0x20) under each candidate stride and report which ones look
// like real pointers. The right stride is the one where both read as plausible heap addresses.
std::atomic<int> g_strideProbe{ 0 };
const unsigned kStrides[] = { 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90 };

// POD-only: MSVC rejects __try in a function needing unwinding.
static bool ReadPtrRaw(const unsigned char* p, unsigned long long* out) {
	__try { *out = *(const unsigned long long*)p; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A user-mode heap pointer on x64: non-null, 8-aligned, and inside the canonical lower half.
static bool PlausiblePtr(unsigned long long v) {
	return v != 0 && (v & 7ull) == 0 && v > 0x10000ull && v < 0x7FFFFFFFFFFFull;
}

// ---- AGGREGATE GEOMETRY --------------------------------------------------------------------
// Layout from the reference dump, re-stated with what this repo has already MEASURED:
//
//   CMeshDrawPrimitive_t   stride 0x70   (measured here; the 2024 dump says 0x68 and is stale --
//                                         see the CBaseSceneData note in SceneSystem.cpp)
//     +0x18  sceneObject    -> CAggregateSceneObject for an aggregate batch
//     +0x20  material
//     +0x50  colour         <- IGNORED by the aggregate path, which is the whole problem
//
//   CAggregateSceneObject
//     +0x120 instance count (int)
//     +0x128 instance data  (CAggregateSceneObjectData*)
//
//   CAggregateSceneObjectData   stride 0x40
//     +0x38  rgba
//
// The two aggregate strides are NOT measured yet -- they come straight from the reference. Every
// read is therefore validated before it is trusted, and `world modulate aggprobe` reports the
// candidate field pairs so the numbers can be re-measured after a CS2 update instead of guessed.
constexpr size_t kMeshDrawStride  = 0x70;
constexpr size_t kMeshDrawObject  = 0x18;

// MEASURED 2026-08-04, CS2 build 10847: the reference's 0x120/0x128 hold nothing plausible on this
// build -- eight consecutive aggregate batches all reported their only candidate pair at 0x148.
// Same stale-dump failure as the 0x68 stride.
//
// RUNTIME-SETTABLE ON PURPOSE (`world modulate agglayout`). Pinning a struct offset that has already
// moved twice into a compile-time constant means a rebuild per hypothesis; as atomics, a candidate
// can be tried over netcon in seconds. The defaults are the measured values, so nothing depends on
// the command being issued.
std::atomic<size_t> g_aggCountOff { 0x160 };
std::atomic<size_t> g_aggDataOff  { 0x168 };
std::atomic<size_t> g_aggDataStride { 0x68 };
std::atomic<size_t> g_aggRgbaOff  { 0x38 };

// STICKY WRITES -- the restore does NOT straddle the original call.
//
// It used to, mirroring how the per-draw path works, and that was wrong here: an aggregate object
// is SHARED BETWEEN RENDER THREADS. The probe logs show the same object address handled by two
// threads in the same frame. With save -> write -> original -> restore, thread B saves thread A's
// tint as if it were the map's colour, A restores the real one, B then puts the tint back
// permanently. The visible result is a floor that flickers black and colours that stick after
// reset -- exactly the reported symptom.
//
// Instead: write once and leave it. Remember each slot's FIRST-SEEN value, and put those back only
// when the user turns modulation off. Concurrent threads then write the same value to the same
// slot, which is harmless no matter how they interleave.
struct OrigSlot {
	unsigned int original;   // the map's own colour, recorded the first time we ever touched it
	unsigned int applied;    // what we last wrote -- lets the restore verify the slot is still ours
};
std::mutex g_origMutex;
std::unordered_map<unsigned int*, OrigSlot> g_orig;

static bool ReadU64Raw(const void* p, unsigned long long* out) {
	__try { *out = *(const unsigned long long*)p; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ReadU32Raw(const void* p, unsigned int* out) {
	__try { *out = *(const unsigned int*)p; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WriteU32Raw(void* p, unsigned int v) {
	__try { *(unsigned int*)p = v; return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// An instance count that is neither zero nor absurd. Arbitrary heap bytes clear this rarely, and
// pairing it with a plausible data pointer is what stops a stale offset from scribbling on
// something that is not an aggregate at all.
static bool PlausibleCount(unsigned int n) { return n >= 1 && n <= 65536; }

std::atomic<int> g_aggProbe{ 0 };
std::atomic<int> g_aggScan{ 0 };
std::atomic<unsigned long long> g_aggInstances{ 0 };

// AGGREGATE WRITES ARE OFF UNTIL THE LAYOUT IS PROVEN.
//
// Writing with count=0x160 / data=0x168 / stride=0x40 / rgba=0x38 CRASHED CS2 immediately
// (cs2_2026_0804_190007_0_accessviolation.mdmp). Those offsets came from a probe that accepted
// `data[0]+0x38 == 0xFFFFFFFF` as proof it had found the instance-colour array -- one sample, and
// 0xFFFFFFFF is far too common a dword to identify anything.
//
// The write did not fault where it happened. It landed in a real engine array and corrupted it, and
// the game died later somewhere else -- the exact valid-but-wrong-memory case __try cannot catch,
// and the same trap LightFx already documents. A guard that only catches unmapped pages is no guard
// at all here.
//
// So: read-only `aggscan` first (it finds the colour array by the SPACING of 0xFFFFFFFF runs, which
// is a structural fingerprint rather than a single value), and writes stay disabled until a stride
// is confirmed across many batches. Turn on with `world modulate aggwrite 1` once it is.
// STILL OFF. The stride IS confirmed now (0x68, from the repeating-0xFFFFFFFF spacing) and writing
// it no longer crashes -- 6.9M instance writes ran clean. But the field at +0x34 / +0x38 is NOT a
// diffuse colour: writing anything other than 0xFFFFFFFF turns the world BLACK rather than tinting
// it, identically for both offsets. Setting aggwrite 0 restores it fully, so the damage is not
// permanent, but a black map is not a feature.
//
// Do not ship this on until the actual colour field is identified. Whatever lives here reads more
// like a lighting or occlusion term where 0xFFFFFFFF means "full" -- see Docs/effects/world-modulation.md.
std::atomic<bool> g_aggWrite{ false };

} // namespace

bool WorldModulation_Active() {
	return g_anyOn.load(std::memory_order_relaxed)
		|| g_strideProbe.load(std::memory_order_relaxed) > 0
		|| g_aggProbe.load(std::memory_order_relaxed) > 0;
}

void WorldModulation_ProbeStride(int batches) {
	if (batches < 1) batches = 1;
	if (batches > 40) batches = 40;
	g_strideProbe.store(batches);
}

void WorldModulation_ProbeBatch(const void* sceneData, int count, const char* matName) {
	if (g_strideProbe.load(std::memory_order_relaxed) <= 0) return;
	if (count < 2) return;   // a single-element batch says nothing about the stride
	g_strideProbe.fetch_sub(1);

	const unsigned char* base = (const unsigned char*)sceneData;
	char line[512]; size_t used = 0; line[0] = 0;
	for (unsigned st : kStrides) {
		unsigned long long so = 0, mat = 0;
		const bool ok = ReadPtrRaw(base + st + 0x18, &so) && ReadPtrRaw(base + st + 0x20, &mat);
		const bool good = ok && PlausiblePtr(so) && PlausiblePtr(mat);
		int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
			"0x%X:%s ", st, good ? "OK" : "--");
		if (n > 0) used += (size_t)n;
	}
	MvmDebugLog_LinefAlways("world.mod",
		"stride probe count=%d mat='%s' base=%p | %s", count, matName ? matName : "?",
		sceneData, line);
}


void WorldModulation_ProbeAggregate(int batches) {
	if (batches < 1) batches = 1;
	if (batches > 40) batches = 40;
	g_aggProbe.store(batches);
}

// Finds the instance array by looking for the POINTER first, then a count beside it.
//
// The first version of this probe scanned 8-aligned (count, ptr-at-+8) pairs over 0x100..0x160 and
// reported exactly one candidate, count at 0x148 -- but a lone `n=1` next to a pointer is also what
// a refcount looks like, and the window was too narrow to know if anything better existed. This
// version widens the window, anchors on a pointer that is actually READABLE (garbage that merely
// looks like an address gets rejected), checks BOTH plausible count slots beside it, and dumps the
// dword at data+rgbaOff -- an untinted instance colour reads 0xFFFFFFFF, which is the giveaway that
// the array really is what we think it is.
static void ProbeAggregateObject(const unsigned char* obj) {
	const size_t rgbaOff = g_aggRgbaOff.load();
	char line[900]; size_t used = 0; line[0] = 0;
	int hits = 0;
	for (size_t off = 0x40; off + 8 <= 0x300 && hits < 6; off += 8) {
		unsigned long long p = 0;
		if (!ReadU64Raw(obj + off, &p) || !PlausiblePtr(p)) continue;

		unsigned int first = 0;
		if (!ReadU32Raw((const void*)p, &first)) continue;         // must be readable memory
		unsigned int atRgba = 0;
		ReadU32Raw((const void*)(p + rgbaOff), &atRgba);

		unsigned int c8 = 0, c4 = 0;
		ReadU32Raw(obj + off - 8, &c8);
		ReadU32Raw(obj + off - 4, &c4);
		if (!PlausibleCount(c8) && !PlausibleCount(c4)) continue;

		int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
			"[data@0x%llX cnt-8=%u cnt-4=%u [0]=%08X [+0x%llX]=%08X] ",
			(unsigned long long)off, c8, c4, first, (unsigned long long)rgbaOff, atRgba);
		if (w > 0) { used += (size_t)w; ++hits; }
	}
	MvmDebugLog_LinefAlways("world.mod",
		"agg probe obj=%p candidates: %s", (const void*)obj, line[0] ? line : "none");
}

// Finds the instance-colour array by STRUCTURE, not by a single value.
//
// An untinted instance colour reads 0xFFFFFFFF, so the colour array shows 0xFFFFFFFF repeating at a
// FIXED SPACING -- and that spacing IS the stride. One 0xFFFFFFFF proves nothing (that is what
// crashed the game); a run of them at even intervals is unmistakable. Purely read-only.
static void ScanAggregateObject(const unsigned char* obj) {
	char line[900]; size_t used = 0; line[0] = 0;
	int reported = 0;
	for (size_t off = 0x40; off + 8 <= 0x300 && reported < 4; off += 8) {
		unsigned long long p = 0;
		if (!ReadU64Raw(obj + off, &p) || !PlausiblePtr(p)) continue;
		unsigned int probe = 0;
		if (!ReadU32Raw((const void*)p, &probe)) continue;
		unsigned int c = 0;
		ReadU32Raw(obj + off - 8, &c);
		if (!PlausibleCount(c)) continue;

		// Where does 0xFFFFFFFF land in the first 1 KB of this array?
		char hits[240]; size_t hu = 0; hits[0] = 0; int nHits = 0;
		for (size_t d = 0; d < 0x400 && nHits < 10; d += 4) {
			unsigned int v = 0;
			if (!ReadU32Raw((const unsigned char*)p + d, &v)) break;
			if (v != 0xFFFFFFFFu) continue;
			int w = _snprintf_s(hits + hu, sizeof(hits) - hu, _TRUNCATE, "%llX,", (unsigned long long)d);
			if (w > 0) { hu += (size_t)w; ++nHits; }
		}
		if (nHits < 2) continue;   // need a RUN, not a lone match

		int w = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
			"[ptr@0x%llX cnt=%u FFs@%s] ", (unsigned long long)off, c, hits);
		if (w > 0) { used += (size_t)w; ++reported; }
	}
	MvmDebugLog_LinefAlways("world.mod",
		"agg scan obj=%p %s", (const void*)obj, line[0] ? line : "no repeating FFFFFFFF found");
}

int WorldModulation_AggregateApply(void* batchArray, int count) {
	int written = 0;
	if (!batchArray || count < 1 || count > 4096) return 0;

	const bool probing = g_aggProbe.load(std::memory_order_relaxed) > 0;
	const bool scanning = g_aggScan.load(std::memory_order_relaxed) > 0;

	// Aggregates ARE the static world, so they take the World colour; there is no per-instance
	// material to classify against and nothing else would be right.
	unsigned char rgba[4];
	const bool tinting = WorldModulation_ColorFor("", rgba)
		&& g_aggWrite.load(std::memory_order_relaxed);
	if (!tinting && !probing && !scanning) return 0;

	// STRENGTH blends the tint toward WHITE, not toward each slot's own colour. White is the neutral
	// modulation value (an untinted instance reads 0xFFFFFFFF), so 0 really is "no tint" -- and
	// because the target is then the same constant for every slot, the fast path below stays
	// lock-free. Blending from each original instead would force a map lookup per instance per frame.
	const float strength = g_strength.load(std::memory_order_relaxed);
	auto mix = [strength](unsigned char c) -> unsigned int {
		const float v = 255.0f + (c - 255.0f) * strength;
		const int i = (int)(v + 0.5f);
		return (unsigned)(i < 0 ? 0 : (i > 255 ? 255 : i));
	};
	const unsigned int packed = tinting
		? (0xFF000000u | (mix(rgba[2]) << 16) | (mix(rgba[1]) << 8) | mix(rgba[0]))
		: 0u;

	const unsigned char* base = (const unsigned char*)batchArray;
	for (int i = 0; i < count; ++i) {
		unsigned long long objAddr = 0;
		if (!ReadU64Raw(base + (size_t)i * kMeshDrawStride + kMeshDrawObject, &objAddr)) break;
		if (!PlausiblePtr(objAddr)) continue;
		const unsigned char* obj = (const unsigned char*)objAddr;

		if (probing && i == 0) {
			g_aggProbe.fetch_sub(1);
			ProbeAggregateObject(obj);
		}
		if (scanning && i == 0) {
			g_aggScan.fetch_sub(1);
			ScanAggregateObject(obj);
		}
		if (!tinting) continue;

		unsigned int n = 0; unsigned long long dataAddr = 0;
		if (!ReadU32Raw(obj + g_aggCountOff.load(std::memory_order_relaxed), &n)) continue;
		if (!ReadU64Raw(obj + g_aggDataOff.load(std::memory_order_relaxed), &dataAddr)) continue;
		// Both must look right. A stale offset that passes only one of these would be writing into
		// whatever happens to live there, and __try does not protect against valid-but-wrong memory.
		if (!PlausibleCount(n) || !PlausiblePtr(dataAddr)) continue;

		unsigned char* data = (unsigned char*)dataAddr;
		const size_t stride = g_aggDataStride.load(std::memory_order_relaxed);
		const size_t rgbaOff = g_aggRgbaOff.load(std::memory_order_relaxed);
		for (unsigned int j = 0; j < n; ++j) {
			unsigned int* slot = (unsigned int*)(data + (size_t)j * stride + rgbaOff);
			unsigned int cur = 0;
			if (!ReadU32Raw(slot, &cur)) break;

			// FAST PATH, and the reason this is thread-safe without a lock: once a slot already
			// holds the target, every thread agrees there is nothing to do. Steady state takes no
			// lock at all -- only a colour change or a newly streamed-in object does.
			if (cur == packed) continue;

			{
				std::lock_guard<std::mutex> lk(g_origMutex);
				auto it = g_orig.find(slot);
				if (it == g_orig.end()) {
					OrigSlot s; s.original = cur; s.applied = packed;
					g_orig.emplace(slot, s);
				} else {
					// Keep the FIRST original. `cur` here is our own previous tint, not the map's.
					it->second.applied = packed;
				}
			}
			if (!WriteU32Raw(slot, packed)) break;
			++written;
		}
	}

	g_aggInstances.fetch_add((unsigned long long)written, std::memory_order_relaxed);
	return written;
}

void WorldModulation_AggregateRestoreAll() {
	std::lock_guard<std::mutex> lk(g_origMutex);
	for (auto& kv : g_orig) {
		// VERIFY THE SLOT IS STILL OURS. These pointers are into engine objects that can be freed
		// and the memory handed to something else entirely on a map change. Writing blindly would
		// be the same valid-but-wrong-memory corruption that crashed CS2 earlier. If the slot no
		// longer holds exactly what we wrote, it is not ours any more and we leave it alone.
		unsigned int cur = 0;
		if (!ReadU32Raw(kv.first, &cur)) continue;
		if (cur != kv.second.applied) continue;
		WriteU32Raw(kv.first, kv.second.original);
	}
	g_orig.clear();
}

namespace {
std::atomic<unsigned> g_hideBits{ 0 };   // 0 = off
std::atomic<unsigned> g_hideValue{ 0 };
unsigned NameHash(const char* s) {
	unsigned h = 2166136261u;
	for (; s && *s; ++s) { h ^= (unsigned char)*s; h *= 16777619u; }
	return h;
}
} // namespace

bool WorldModulation_HideDraw(const char* materialName) {
	const unsigned bits = g_hideBits.load(std::memory_order_relaxed);
	if (!bits || !materialName || !materialName[0]) return false;
	const unsigned mask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	return (NameHash(materialName) & mask) == g_hideValue.load(std::memory_order_relaxed);
}

// Names of the live world/prop/sky materials in a hash bucket, for the hide bisect.
static void LogBucketNames(unsigned bits, unsigned value, int maxLines) {
	GetMaterialsArrayResult materials = { 0 };
	if (!g_pCResourceSystem || !GetMaterialsRaw(&materials)) return;
	const unsigned mask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	int logged = 0, matched = 0;
	for (uint64_t i = 0; i < materials.count; ++i) {
		CMaterial2* mat = nullptr;
		if (!ReadMaterialAtRaw(materials.pArrMaterials, i, &mat) || !mat) continue;
		char name[512];
		if (!CopyMaterialNameRaw(mat, name, sizeof(name)) || !name[0]) continue;
		if ((NameHash(name) & mask) != value) continue;
		++matched;
		if (logged++ < maxLines) {
			float* p = nullptr; FindMaterialParamRaw(mat, "g_vColorTint", &p);
			MvmDebugLog_LinefAlways("world.mod.bucket", "%s hasColorTint=%d", name, p ? 1 : 0);
		}
	}
	MvmDebugLog_LinefAlways("world.mod.bucket", "bucket bits=%u value=%u matched=%d", bits, value, matched);
}

bool WorldModulation_ColorFor(const char* materialName, unsigned char outRgba[4]) {
	if (!g_anyOn.load(std::memory_order_relaxed)) return false;
	const ModCategory c = Classify(materialName);
	if (IsCharacterMaterial(materialName) && MvmDebugLog_Active()) {
		static std::atomic<unsigned> s_characterDrawLog{ 0 };
		const unsigned n = s_characterDrawLog.fetch_add(1, std::memory_order_relaxed);
		if (n < 64) {
			MvmDebugLog_Linef("world.mod.character.draw",
				"material='%s' category=%s owns=%d", materialName, CategoryKey(c),
				(UsesMaterialPath(c) && g_materialTintOwnership.Owns(materialName)) ? 1 : 0);
		}
	}
	// Suppress the draw override only when the latest main-thread material scan proved that this
	// exact material exposes g_vColorTint and owns the tint. Streamed/instanced submaterials that
	// were not observed keep the CBaseSceneData fallback, so they cannot escape during a map load.
	if (UsesMaterialPath(c) && g_materialTintOwnership.Owns(materialName)) return false;
	if (!ColorForCategory(c, outRgba)) return false;
	if (UsesMaterialPath(c) && MvmDebugLog_Active()) {
		const unsigned long long index =
			g_fallbackDraws.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((index % 2048ull) == 1) {
			MvmDebugLog_Linef("world.mod.fallback",
				"material='%s' category=%s materialOwns=0 rgba=(%u %u %u %u) draw=%llu",
				materialName ? materialName : "", CategoryKey(c),
				(unsigned)outRgba[0], (unsigned)outRgba[1], (unsigned)outRgba[2],
				(unsigned)outRgba[3], index);
		}
	}
	return true;
}

static bool FloatEqual(float a, float b) {
	return 0 == std::memcmp(&a, &b, sizeof(float));
}

void WorldModulation_SetTint(ModCategory c, int r, int g, int b) {
	const int i = (int)c;
	if (i < 0 || i >= kCount) return;
	g_rgb[i].store(((unsigned)ClampI(r) << 16) | ((unsigned)ClampI(g) << 8) | (unsigned)ClampI(b));
	g_on[i].store(true);
	RecomputeAny();
	if (UsesMaterialPath(c)) g_materialDirty = true;
}

void WorldModulation_SetEnabled(ModCategory c, bool on) {
	const int i = (int)c;
	if (i < 0 || i >= kCount) return;
	g_on[i].store(on);
	RecomputeAny();
	if (UsesMaterialPath(c)) g_materialDirty = true;
	if (!on) WorldModulation_AggregateRestoreAll();
}

void WorldModulation_SetStrength(float s) {
	if (s < 0.0f) s = 0.0f;
	if (s > 1.0f) s = 1.0f;
	g_strength.store(s);
	if (AnyMaterialTintEnabled()) g_materialDirty = true;
}
float WorldModulation_Strength() { return g_strength.load(); }

void WorldModulation_GetTint(ModCategory c, int* r, int* g, int* b, bool* enabled) {
    const auto p=g_rgb[(int)c].load();*r=(p>>16)&255;*g=(p>>8)&255;*b=p&255;*enabled=g_on[(int)c].load();
}
void WorldModulation_OnLevelInit() {
    ApplyMaterialTints(true);WorldModulation_AggregateRestoreAll();
    g_materialStableSinceMs=0;g_observedMaterialCount=0;g_materialDirty=true;
}
void WorldModulation_Reset() {
    ApplyMaterialTints(true);
	for (int i = 0; i < kCount; ++i) { g_rgb[i].store(0xFFFFFFu); g_on[i].store(false); }
	g_strength.store(1.0f);
	RecomputeAny();
	g_materialDirty = true;
	// Sticky writes mean nothing puts the map's own colours back on its own.
	WorldModulation_AggregateRestoreAll();
}

void WorldModulation_RefreshTransient() { g_materialDirty = true; }
void WorldModulation_RunFrame() {
	const bool materialTintOn = AnyMaterialTintEnabled();
	if (!materialTintOn && !g_materialDirty && g_materialSnapshots.empty()) return;
	if (!g_pCResourceSystem) return;

	GetMaterialsArrayResult materials = { 0 };
	if (!GetMaterialsRaw(&materials)) return;
	const unsigned long long now = GetTickCount64();
	if (materials.count != g_observedMaterialCount) {
		g_observedMaterialCount = materials.count;
		g_materialStableSinceMs = now;
		g_materialDirty = true;
		MvmDebugLog_Linef("world.mod.material", "stream count=%llu; waiting %llu ms",
			(unsigned long long)materials.count, kMaterialSettleMs);
		return;
	}
	if (!g_materialStableSinceMs) {
		g_materialStableSinceMs = now;
		return;
	}
	if (now - g_materialStableSinceMs < kMaterialSettleMs) return;

	// Do not call into materialsystem2 while its async level-load list is still changing. This
	// debounce fixes the measured tick-1 crash; the periodic pass handles same-count replacement.
	if (g_materialDirty || now - g_lastMaterialApplyMs >= kMaterialReassertMs)
		ApplyMaterialTints(!materialTintOn);
}

void WorldModulation_AppendStateJson(std::ostringstream& o) {
	o << ",\"mod_strength\":" << g_strength.load();
	for (int i = 0; i < kCount; ++i) {
		const unsigned int p = g_rgb[i].load();
		o << ",\"mod_" << CategoryKey((ModCategory)i) << "\":\""
		  << ((p >> 16) & 0xFF) << " " << ((p >> 8) & 0xFF) << " " << (p & 0xFF) << "\"";
		o << ",\"mod_" << CategoryKey((ModCategory)i) << "_on\":"
		  << (g_on[i].load() ? "true" : "false");
	}
}

void WorldModulation_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world modulate <category> <r> <g> <b> | off | reset
	const char* op = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(op, "status")) {
		std::ostringstream state;
		state << "{\"active\":" << (WorldModulation_Active() ? "true" : "false");
		WorldModulation_AppendStateJson(state);
		state << '}';
		advancedfx::Message("world modulation: %s\n", state.str().c_str());
		return;
	}

	if (0 == _stricmp(op, "hide")) {
		// hide off | hide <bits> <value>: debug bisect -- drop every draw whose material name hashes into the bucket.
		if (argc >= 5 && 0 != _stricmp(args->ArgV(4), "off")) {
			g_hideValue.store((unsigned)atoi(args->ArgV(5 <= argc - 1 ? 5 : 4)));
			g_hideBits.store((unsigned)atoi(args->ArgV(4)));
		} else {
			g_hideBits.store(0);
		}
		advancedfx::Message("mirv_filmmaker world modulate hide: %s (bits %u value %u)\n",
			g_hideBits.load() ? "ON" : "off", g_hideBits.load(), g_hideValue.load());
		return;
	}
	if (0 == _stricmp(op, "bucket")) {
		LogBucketNames((unsigned)atoi(argc >= 5 ? args->ArgV(4) : "0"), (unsigned)atoi(argc >= 6 ? args->ArgV(5) : "0"), 400);
		advancedfx::Message("Bucket names written to mvm_debug channel world.mod.bucket.\n");
		return;
	}
	if (0 == _stricmp(op, "untinted")) {
		const int n = (argc >= 5) ? atoi(args->ArgV(4)) : 400;
		ProbeUntinted(n);
		advancedfx::Message("Materials the world tint cannot reach were written to mvm_debug channel world.mod.untinted (up to %d).\n", n);
		return;
	}

	if (0 == _stricmp(op, "matprobe")) {
		const int n = (argc >= 5) ? atoi(args->ArgV(4)) : 64;
		ProbeMaterialParams(n);
		advancedfx::Message(
			"Read-only loaded-material colour-parameter probe written to mvm_debug channel "
			"world.mod.material (up to %d detail lines).\n", n);
		return;
	}

	if (0 == _stricmp(op, "stride")) {
		const int n = (argc >= 5) ? atoi(args->ArgV(4)) : 8;
		WorldModulation_ProbeStride(n);
		advancedfx::Message(
			"Probing the next %d multi-element batch(es) into mvm_debug channel world.mod.\n"
			"Reports which candidate stride reads as real pointers -- that is the array step.\n", n);
		return;
	}

	// Retune the aggregate struct offsets live. These have already moved twice between the reference
	// dump and this build, so each candidate is one netcon command instead of one rebuild.
	if (0 == _stricmp(op, "agglayout")) {
		if (argc >= 8) {
			g_aggCountOff.store((size_t)strtoull(args->ArgV(4), nullptr, 0));
			g_aggDataOff.store((size_t)strtoull(args->ArgV(5), nullptr, 0));
			g_aggDataStride.store((size_t)strtoull(args->ArgV(6), nullptr, 0));
			g_aggRgbaOff.store((size_t)strtoull(args->ArgV(7), nullptr, 0));
		}
		advancedfx::Message(
			"agg layout: count=0x%llX data=0x%llX stride=0x%llX rgba=0x%llX | %llu instances tinted\n"
			"Set with: %s world modulate agglayout <countOff> <dataOff> <stride> <rgbaOff>\n",
			(unsigned long long)g_aggCountOff.load(), (unsigned long long)g_aggDataOff.load(),
			(unsigned long long)g_aggDataStride.load(), (unsigned long long)g_aggRgbaOff.load(),
			g_aggInstances.load(), cmd);
		return;
	}

	if (0 == _stricmp(op, "strength")) {
		if (argc >= 5) {
			WorldModulation_SetStrength((float)atof(args->ArgV(4)));
			const bool quiet = (argc >= 6) && (0 == _stricmp(args->ArgV(5), "quiet"));
			if (quiet) return;
		}
		advancedfx::Message("World tint strength %.2f (0 = untouched, 1 = full tint).\n",
			WorldModulation_Strength());
		return;
	}

	if (0 == _stricmp(op, "aggscan")) {
		const int n = (argc >= 5) ? atoi(args->ArgV(4)) : 8;
		if (n >= 1) g_aggScan.store(n > 40 ? 40 : n);
		advancedfx::Message(
			"Read-only scan of the next %d aggregate batch(es) -> mvm_debug channel world.mod.\n"
			"Reports where 0xFFFFFFFF repeats in each candidate array; the SPACING is the stride.\n", n);
		return;
	}

	if (0 == _stricmp(op, "aggwrite")) {
		if (argc >= 5) g_aggWrite.store(0 != atoi(args->ArgV(4)));
		advancedfx::Message(
			"Aggregate writes: %s. Off by default -- writing an unproven layout crashed CS2 once\n"
			"already, and it corrupts engine memory rather than faulting where the bug is.\n"
			"Confirm the stride with `world modulate aggscan` before enabling.\n",
			g_aggWrite.load() ? "ON" : "off");
		return;
	}

	if (0 == _stricmp(op, "aggprobe")) {
		const int n = (argc >= 5) ? atoi(args->ArgV(4)) : 8;
		WorldModulation_ProbeAggregate(n);
		advancedfx::Message(
			"Probing the next %d aggregate batch(es) into mvm_debug channel world.mod.\n"
			"Reports candidate (instance count, data pointer) pairs in the aggregate object.\n"
			"%llu aggregate instances tinted so far.\n", n, g_aggInstances.load());
		return;
	}

	if (0 == _stricmp(op, "reset")) {
		WorldModulation_Reset();
		advancedfx::Message("World modulation off; every category back to the map's own colours.\n");
		return;
	}

	ModCategory c;
	if (ParseCategory(op, &c)) {
		if (argc >= 5 && 0 == _stricmp(args->ArgV(4), "off")) {
			WorldModulation_SetEnabled(c, false);
			advancedfx::Message("%s modulation off.\n", CategoryKey(c));
			return;
		}
		if (argc >= 7) {
			WorldModulation_SetTint(c, atoi(args->ArgV(4)), atoi(args->ArgV(5)), atoi(args->ArgV(6)));
			const bool quiet = (argc >= 8) && (0 == _stricmp(args->ArgV(7), "quiet"));
			if (!quiet) advancedfx::Message("%s tinted %s %s %s.\n", CategoryKey(c),
				args->ArgV(4), args->ArgV(5), args->ArgV(6));
			return;
		}
		advancedfx::Message("Usage: %s world modulate %s <r> <g> <b> | off\n", cmd, CategoryKey(c));
		return;
	}

	advancedfx::Message(
		"mirv_filmmaker world modulate - tint the GEOMETRY as it draws.\n"
		"This is the one that is actually visible. Scene-light tinting is correct but nearly\n"
		"invisible on CS2 maps, because their lighting is baked -- so colour the world instead.\n"
		"Usage:\n"
		"  %s world modulate world <r> <g> <b>      - world surfaces\n"
		"  %s world modulate props <r> <g> <b>      - model/prop surfaces\n"
		"  %s world modulate sunclouds <r> <g> <b>  - sun and cloud layers\n"
		"  %s world modulate effects <r> <g> <b>    - particle/effect surfaces\n"
		"  %s world modulate <category> off\n"
		"  %s world modulate reset\n"
		"  %s world modulate status - enumerate live colors, enabled flags and strength as JSON\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
