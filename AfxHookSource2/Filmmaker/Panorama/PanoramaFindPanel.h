#pragma once

// Shared Panorama panel lookup for the *Hud / *Menu bridge files: a bounded recursive
// child search by panel id over the engine's live panel tree (offsets from
// CS2::PanoramaUIPanel in DeathMsg.h). Previously copy-pasted into every bridge file;
// one definition lives in PanoramaFindPanel.cpp.

namespace Filmmaker {

// Breadth-first-ish: direct children are checked before descending, so a shallow match
// wins over a deep one. depth caps recursion at 64 levels.
void* FindChildById(void* panel, const char* id, int depth = 0);

} // namespace Filmmaker
