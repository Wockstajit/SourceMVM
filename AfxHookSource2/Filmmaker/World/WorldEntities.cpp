#include "WorldEntities.h"

#include "../../ClientEntitySystem.h"

#include <cstring>

namespace Filmmaker {

namespace {

// Kept free of C++ objects so __try is legal (MSVC forbids it in functions needing unwinding).
const char* SafeClassName(CEntityInstance* ent, bool clientClass) {
	if (!ent) return nullptr;
	__try {
		return clientClass ? ent->GetClientClassName() : ent->GetClassName();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool MatchesAny(const char* name, const char* const* names, std::size_t count) {
	if (!name) return false;
	for (std::size_t i = 0; i < count; ++i) {
		if (names[i] && 0 == _stricmp(names[i], name)) return true;
	}
	return false;
}

} // namespace

bool WorldEntity_Matches(CEntityInstance* ent, const char* const* names, std::size_t count) {
	if (!ent || !names || 0 == count) return false;
	if (MatchesAny(SafeClassName(ent, false), names, count)) return true;   // entity class
	return MatchesAny(SafeClassName(ent, true), names, count);              // client class
}

int WorldEntity_Count(const char* const* names, std::size_t count) {
	if (!g_pEntityList || !g_GetEntityFromIndex) return -1;
	int found = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (WorldEntity_Matches(ent, names, count)) ++found;
	}
	return found;
}

} // namespace Filmmaker
