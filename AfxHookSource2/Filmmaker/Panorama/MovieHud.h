#pragma once

// MovieHud: the in-game movie-director help/status panel, rendered with CS2's
// native Panorama in the in-game HUD (CSGOHud) context.
//
// Mirrors the proven FilmmakerMenu pattern, but pins its own PanoramaBridge to
// the HUD panel (AfxHookSource2_GetPanoramaHudPanel) instead of the main menu,
// builds the panel once, re-locates/rebuilds it if the HUD recreates it, and
// pushes a small state JSON each frame. Display-only: visibility is toggled by a
// console command (mirv_filmmaker hud ...), bindable to a key.

#include "PanoramaBridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

class MovieHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetVisible(bool v) { m_visible = v; }
	void Toggle() { m_visible = !m_visible; }
	bool Visible() const { return m_visible; }

	// Queues Panorama JS to run in the HUD-panel context next frame (live probe /
	// REPL via 'mirv_filmmaker hud_eval'); output goes to the console.
	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #MovieHudRoot
	short m_symState = -1;
	bool m_built = false;
	bool m_visible = true; // shown by default while in a demo; F8 toggles it
	std::string m_lastJson;
	std::vector<std::string> m_evalQueue;
};

MovieHud& MovieHudRef();

} // namespace Filmmaker
