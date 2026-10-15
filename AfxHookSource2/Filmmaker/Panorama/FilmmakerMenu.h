#pragma once

// Orchestrates the Panorama menu: builds the UI (injects the JS), pushes the
// demo list into it, and polls the JS->C++ command queue each frame.
//
// Entirely guarded: until the Panorama bridge has an engine + a verified
// RunScript offset + a context panel, every method is a safe no-op, and the
// console-driven demo browser keeps working unchanged.

#include "PanoramaBridge.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {

class DemoLibrary;

class FilmmakerMenu {
public:
	PanoramaBridge& Bridge() { return m_bridge; }

	// Called every frame from the render thread. Builds the UI when possible,
	// pushes new scan results, and dispatches queued UI commands.
	void RunFrame(DemoLibrary& library);

	// Forces a rebuild of the UI on the next frame.
	void RequestRebuild() { m_built = false; m_buildAttempted = false; }

	// Requests the page be shown (processed on the main thread next frame).
	void RequestShow() { m_showRequested = true; }

	// Queues arbitrary Panorama JS to run on the main thread next frame (live
	// probe / REPL via 'mirv_filmmaker ui_eval'). Output goes to the console.
	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

	bool IsBuilt() const { return m_built; }

private:
	void BuildUi();
	void PushData(DemoLibrary& library);
	// Pushes the list-organiser payload (sort mode, user folders, last-played times). Separate
	// from PushData because it changes on its own schedule -- a folder toggle must not wait for
	// a library rescan, and a rescan must not resend the organiser state.
	void PushOrganizer();
	void PollCommands(DemoLibrary& library);

	void* FindRootPanel();

	PanoramaBridge m_bridge;
	bool m_built = false;
	bool m_buildAttempted = false;
	bool m_pauseTrackerInstalled = false;
	short m_symPauseMenu = -1;
	bool m_showRequested = false;
	std::vector<std::string> m_evalQueue;
	void* m_rootPanel = nullptr;
	short m_symCmd = -1;
	short m_symDemos = -1;
	short m_symOrg = -1;
	uint64_t m_lastVersion = (uint64_t)-1;
	uint64_t m_lastOrgVersion = (uint64_t)-1;
	std::string m_lastRepairSignature;
};

} // namespace Filmmaker
