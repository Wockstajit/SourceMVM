#include "MethodActingFlex.h"
#include "ModelPoseAccess.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Active / MvmDebugLog_Linef
#include "../../SchemaSystem.h"            // g_clientDllOffsets (ModelChain, for the cache key)
#include "../../../Shared/binutils.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
namespace MethodActingFlex {

namespace {

// --- resolved natives ---------------------------------------------------------------------------
//
// GetFlexControllerCount(entity)          -> the model's flex controller count
// GetFlexControllerName(entity, index)    -> that controller's name, or nullptr
// SetFlexWeightByIndex(entity, idx, val)  -> range-checks idx against the count, remaps through
//                                            the controller's min/max, stores the weight
//
// All three take the ENTITY, so nothing here has to know the shape of a single engine struct.
// An earlier revision walked the flex descriptor array by hand and inferred the name field's
// offset from the compiled model data; that read past the end of the controller list into the
// model's flex DESCRIPTIONS (whose names also look valid) and took an access violation on every
// lookup. These accessors are what the engine's own hostage blink code uses instead.

typedef int         (__fastcall* GetFlexCount_t)(void* entity);
typedef const char* (__fastcall* GetFlexName_t)(void* entity, int index);
typedef void        (__fastcall* SetFlexWeightByIndex_t)(void* entity, int index, float value);

GetFlexCount_t         g_getFlexCount = nullptr;
GetFlexName_t          g_getFlexName = nullptr;
SetFlexWeightByIndex_t g_setFlexWeightByIndex = nullptr;

Status g_status;
int g_indexOverride = -1;

// Sanity bound on a count read out of the engine. CS2 agents have 3.
constexpr int kMaxControllers = 64;

// Cached by the resolved CModel resource, shared by players wearing the same agent.
// The entity model-name storage is not a stable resource key.
struct EyeCacheEntry { const void* key; EyeIndices eyes; };
constexpr int kCacheSize = 32;
EyeCacheEntry g_cache[kCacheSize] = {};
int g_cacheUsed = 0;
int g_cacheNext = 0;

size_t FindClientPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	const int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

// --- SEH-guarded primitives ---------------------------------------------------------------------
// Each __try body is POD-only; MSVC rejects a __try in a frame that needs C++ unwinding.

int GuardedCount(void* entity) {
	int out = -1;
	__try {
		out = g_getFlexCount(entity);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out = -1;
		++g_status.faults;
	}
	if (out < 0 || out > kMaxControllers) return -1;
	return out;
}

// Copies a controller name out and validates it is a printable C string, so a moved pattern
// hands back "no name" rather than feeding a garbage pointer to strstr.
bool GuardedName(void* entity, int index, char* out, int outSize) {
	bool ok = false;
	out[0] = 0;
	__try {
		const char* name = g_getFlexName(entity, index);
		if (name) {
			int i = 0;
			for (; i < outSize - 1; ++i) {
				const char c = name[i];
				if (c == 0) break;
				if (c < 0x20 || c > 0x7E) { i = -1; break; }
				out[i] = c;
			}
			if (i > 0) {
				out[i] = 0;
				ok = true;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
		++g_status.faults;
	}
	if (!ok) out[0] = 0;
	return ok;
}

bool GuardedSetWeight(void* entity, int index, float value) {
	bool ok = false;
	__try {
		g_setFlexWeightByIndex(entity, index, value);
		ok = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
		++g_status.faults;
	}
	return ok;
}

// The resolved CModel resource, used only as a cache key here.
// Returns null when the schema chain is unavailable, which just disables caching.
const void* GuardedModelKey(void* entity) {
    return ModelPoseAccess::ModelOf(ModelPoseAccess::ModelStateOf((CEntityInstance*)entity));
}

bool CacheLookup(const void* key, EyeIndices* out) {
	if (!key) return false;
	for (int i = 0; i < g_cacheUsed; ++i) {
		if (g_cache[i].key == key) { *out = g_cache[i].eyes; return true; }
	}
	return false;
}

void CacheStore(const void* key, const EyeIndices& eyes) {
	if (!key) return;
	if (g_cacheUsed < kCacheSize) {
		g_cache[g_cacheUsed].key = key;
		g_cache[g_cacheUsed].eyes = eyes;
		++g_cacheUsed;
		return;
	}
	g_cache[g_cacheNext].key = key;
	g_cache[g_cacheNext].eyes = eyes;
	g_cacheNext = (g_cacheNext + 1) % kCacheSize;
}

} // namespace

bool Resolve() {
	if (g_status.attempted)
		return g_status.resolved;
	g_status.attempted = true;

	// The CS2 hostage's own "find the eye_blink flex controller by name" loop:
	//   mov edx,edi / mov rcx,rsi / call GetFlexControllerName / mov rcx,rax /
	//   lea rdx,"eye_blink" / call strcmp / test eax,eax / je found /
	//   mov rcx,rsi / inc edi / call GetFlexControllerCount
	// Anchoring here rather than on the two functions' prologues is deliberate: a second
	// client.dll function has a byte-identical prologue to the name accessor and differs only in
	// its tail jump, so a prologue scan picks one of two at random.
	const size_t anchor = FindClientPattern(
		"8B D7 48 8B CE E8 ?? ?? ?? ?? 48 8B C8 48 8D 15 ?? ?? ?? ?? "
		"FF 15 ?? ?? ?? ?? 85 C0 74 ?? 48 8B CE FF C7 E8");

	if (anchor) {
		g_getFlexName = (GetFlexName_t)ResolveRelCall(anchor + 0x05);
		g_getFlexCount = (GetFlexCount_t)ResolveRelCall(anchor + 0x23);
	}

	// C_BaseModelEntity::SetFlexWeightByIndex. The bounds-checked wrapper, NOT the inner
	// SetFlexWeight: that one validates only that the model's controller count matches the
	// entity's weight count, never that the index is inside them, so a wrong index there writes
	// past the array.
	g_setFlexWeightByIndex = (SetFlexWeightByIndex_t)FindClientPattern(
		"85 D2 78 ?? 48 89 5C 24 08 57 48 83 EC 30 48 8B F9 0F 29 74 24 20 "
		"48 8B 89 30 03 00 00 0F 28 F2 8B DA 48 8B 01 FF 50 68");

	g_status.resolved = g_getFlexName && g_getFlexCount && g_setFlexWeightByIndex;

	MvmDebugLog_LinefAlways("acting.flex",
		"resolve anchor=%p count=%p name=%p set=%p ok=%d",
		(void*)anchor, (void*)g_getFlexCount, (void*)g_getFlexName,
		(void*)g_setFlexWeightByIndex, g_status.resolved ? 1 : 0);

	return g_status.resolved;
}

const Status& GetStatus() { return g_status; }

void SetIndexOverride(int index) { g_indexOverride = index < 0 ? -1 : index; }
int  IndexOverride() { return g_indexOverride; }

bool FindEyeIndices(void* entity, EyeIndices* out) {
	if (!entity || !out || !Resolve())
		return false;

	const void* key = GuardedModelKey(entity);
	if (!key) return false; // Non-rendering ragdoll carriers have no facial model.
	if (CacheLookup(key, out)) {
		if (g_indexOverride >= 0) out->blink = g_indexOverride;
		return out->count > 0;
	}
	++g_status.lookups;

	EyeIndices eyes;
	eyes.count = GuardedCount(entity);
	if (eyes.count <= 0) {
		if (eyes.count == 0) CacheStore(key, eyes);
		return false;
	}

	char names[sizeof(g_status.lastNames)];
	names[0] = 0;
	int namesLen = 0;

	for (int i = 0; i < eyes.count; ++i) {
		char name[64];
		if (!GuardedName(entity, i, name, sizeof(name)))
			continue;
		g_status.sawName = true;

		const int len = (int)strlen(name);
		if (namesLen + len + 1 < (int)sizeof(names) - 1) {
			if (namesLen) names[namesLen++] = ',';
			memcpy(names + namesLen, name, len);
			namesLen += len;
			names[namesLen] = 0;
		}

		// Matched on substrings so a renamed-but-equivalent controller on a future agent still
		// works. "up" is tested before "down" only because neither name contains the other.
		if (eyes.blink < 0 && strstr(name, "blink")) eyes.blink = i;
		else if (eyes.lookUp < 0 && strstr(name, "up")) eyes.lookUp = i;
		else if (eyes.lookDown < 0 && strstr(name, "down")) eyes.lookDown = i;
	}

	g_status.lastCount = eyes.count;
	g_status.lastEyes = eyes;
	if (names[0])
		strncpy_s(g_status.lastNames, sizeof(g_status.lastNames), names, _TRUNCATE);

	MvmDebugLog_LinefAlways("acting.flex.model", "entity=%p model=%p count=%d blink=%d up=%d down=%d names=%s",
        entity,key,eyes.count,eyes.blink,eyes.lookUp,eyes.lookDown,names);
    CacheStore(key, eyes);
	*out = eyes;
	if (g_indexOverride >= 0) out->blink = g_indexOverride;
	return true;
}

bool SetWeight(void* entity, int index, float value) {
	if (!entity || index < 0 || !Resolve())
		return false;
	if (!GuardedSetWeight(entity, index, value))
		return false;
	++g_status.writes;
	return true;
}

} // namespace MethodActingFlex
} // namespace Filmmaker
