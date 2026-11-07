#pragma once

// WorkspaceHeaderHud: the bar along the top of the screen in the MODULAR layout
// (Panorama/UiLayout.h, WorkspaceHeaderJs.h, Docs/panorama/modular-layout.md).
//
// Built into the in-game HUD context while a demo plays and the layout is modular, torn down
// otherwise. Each frame it pushes which panels are open, reads back where the bar is for the click
// router, and hosts the TIMELINE dock itself when the dock is switched on with no window open --
// the windows host it whenever one of them is up (Movie/TimelineDock.h ranks).

#include "PanoramaBridge.h"

#include <string>

namespace Filmmaker {

class WorkspaceHeaderHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();
	bool Visible() const { return m_visible; }
	bool PopupOpen() const { return m_popupOpen; }
	void RequestEval(const std::string& js) { m_evalQueue += js; m_evalQueue += ';'; }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr;
	void* m_root = nullptr;
	short m_symState = -1;
	short m_symRect = -1;
	short m_symPopupOpen = -1;
	bool m_built = false;
	bool m_visible = false;
	bool m_popupOpen = false;
	std::string m_lastJson;
	std::string m_evalQueue;
};

WorkspaceHeaderHud& WorkspaceHeaderHudRef();

// Any thread is fine for these two reads (plain flags, set on the main thread).
bool WorkspaceHeader_Visible();
bool WorkspaceHeader_PopupOpen();

} // namespace Filmmaker
