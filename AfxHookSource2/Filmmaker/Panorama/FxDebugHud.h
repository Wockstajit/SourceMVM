#pragma once

// FxDebugHud: two small colored squares pinned bottom-left of the in-game HUD, one per FX
// pack (Modern / Insurgency On), that flash briefly whenever ParticleFx confirms a swap into
// that pack's wisp-carrying parent smoke system. Purely a debug/verification aid (user
// request 2026-07-03) so a screenshot can prove the wisp swap fired -- see FxDebugHudJs.h
// for the visual language and its caveats. Off by default; toggle with
// 'mirv_filmmaker fx debughud on|off'. Same proven pattern as MarkerHud: pin a
// PanoramaBridge to the HUD panel, build the panel once, push a small state JSON each frame.

#include "PanoramaBridge.h"

#include <string>

namespace Filmmaker {

class FxDebugHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #FxDebugHudRoot
	short m_symState = -1;
	bool m_built = false;
	std::string m_lastJson;
};

FxDebugHud& FxDebugHudRef();

} // namespace Filmmaker
