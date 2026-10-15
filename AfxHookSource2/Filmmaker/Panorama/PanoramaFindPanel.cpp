// One shared definition of the bounded recursive Panorama child-by-id search that every
// *Hud / *Menu bridge file used to carry its own copy of. See PanoramaFindPanel.h.

#include "PanoramaFindPanel.h"

#include "../../DeathMsg.h" // CS2::PanoramaUIPanel offsets

#include <cstring>

namespace Filmmaker {

void* FindChildById(void* panel, const char* id, int depth) {
	if (!panel || depth > 64)
		return nullptr;
	unsigned char* childrenField = (unsigned char*)panel + CS2::PanoramaUIPanel::children;
	const int count = *(int*)childrenField;
	void** arr = *(void***)(childrenField + 8);
	if (!arr || count <= 0 || count > 100000)
		return nullptr;
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		char* cid = *(char**)((unsigned char*)child + CS2::PanoramaUIPanel::panelId);
		if (cid && 0 == std::strcmp(cid, id))
			return child;
	}
	for (int i = 0; i < count; ++i) {
		void* child = arr[i];
		if (!child) continue;
		if (void* found = FindChildById(child, id, depth + 1))
			return found;
	}
	return nullptr;
}

} // namespace Filmmaker
