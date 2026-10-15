#pragma once

// The easter-egg overlay: one full-screen Panorama root (#MvmEggRoot) that draws every egg's
// on-screen part -- the Gen X hint banners, the E-girl sparkles and pastel wash, the Sony Vegas
// intervention video, and Clippy.
//
// Unlike the other HUDs it lives in WHICHEVER context is up: the in-game HUD while a map/demo is
// loaded, the main menu otherwise, so a hint can float across either. Switching contexts rebuilds
// it in the new one.
//
//   C++ -> JS : attribute "state" (JSON from EasterEggs_BuildOverlayJson), then $.MvmEggs.render().
//   JS  -> C++: Clippy's buttons issue `mirv_filmmaker egg clippy ...` console commands.

#include "PanoramaBridge.h"

#include <cstdint>
#include <string>

namespace Filmmaker {

class EasterEggOverlayHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

private:
	bool BuildIfNeeded();
	void Teardown();

	PanoramaBridge m_bridge;
	void* m_context = nullptr;
	void* m_root = nullptr;
	short m_symState = -1;
	short m_symOsuFrame = -1, m_symOsuObjs = -1, m_symOsuList = -1;
	short m_symSnake = -1;
	std::string m_snakeJson;
	bool m_snakeWasActive = false;
	std::string m_osuFrame, m_osuObjs, m_osuList;
	uint64_t m_osuObjsRev = 0, m_osuListRev = 0;
	bool m_osuWasActive = false;
	bool m_built = false;
	bool m_animating = false;
	std::string m_lastJson;
};

EasterEggOverlayHud& EasterEggOverlayHudRef();

} // namespace Filmmaker
