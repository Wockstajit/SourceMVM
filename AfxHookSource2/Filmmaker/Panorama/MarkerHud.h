#pragma once

// MarkerHud: the BO2-inspired "CAMERA MARKER #N" edit menu, rendered with CS2's
// native Panorama in the in-game HUD context. Same proven pattern as MovieHud:
// pin a PanoramaBridge to the HUD panel, build the panel once, push a small state
// JSON each frame, and let JS buttons drive actions via console commands
// (mirv_filmmaker marker ...). Visibility follows CameraPath::MenuOpen() (opened
// by F when aiming at a marker; closed by the Done button / ESC hint).

#include "PanoramaBridge.h"

#include <string>

namespace Filmmaker {

class MarkerHud {
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
	void* m_root = nullptr;     // #MarkerHudRoot
	short m_symState = -1;
	bool m_built = false;
	std::string m_lastJson;
};

MarkerHud& MarkerHudRef();

} // namespace Filmmaker
