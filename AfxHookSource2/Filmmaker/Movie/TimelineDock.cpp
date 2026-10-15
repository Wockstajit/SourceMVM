#include "TimelineDock.h"

#include "../Panorama/CameraTimelineHud.h"
#include "../Debug/MvmDebugLog.h" // mvm_debug: who owns the dock, and who was refused

#include "../../../Shared/AfxConsole.h"

#include <cstring>

namespace Filmmaker {

// Declared in Filmmaker.cpp alongside the other GraphEditorExperiment_* bridges; taken by
// declaration rather than by including GraphEditorExperimentHud.h so this module keeps its
// dependency on the graph down to the one call it actually makes.
void GraphEditorExperiment_Set(bool enabled);

namespace {

DockTab g_tab = DockTab::Native;
DockHost g_host = DockHost::None;

// Design dock heights (Docs/panorama/panorama_ui_guide.md): the dock is
// bottom-anchored and its height changes per tab.
constexpr int kHeightNative = 126;
constexpr int kHeightCamera = 196;
constexpr int kHeightGraph = 416;

// The Camera Editor outranks CONFIG: it is the workspace that owns the whole screen, and opening
// it closes CONFIG anyway. Without a rank, a frame in which both believe they are open would let
// them alternate the overlays on and off.
int Rank(DockHost host) {
	switch (host) {
	case DockHost::CameraEditor: return 2;
	case DockHost::Config: return 1;
	default: return 0;
	}
}

const char* HostName(DockHost host) {
	switch (host) {
	case DockHost::CameraEditor: return "editor";
	case DockHost::Config: return "config";
	default: return "nobody";
	}
}

// EDGE-TRIGGERED, because these run every frame: only a CHANGE in who owns the dock, or a change
// in who is being refused, is worth a line. A per-frame log here would bury the one transition
// that explains "CAMERA / GRAPH did nothing, the native timeline stayed up".
DockHost g_lastOwner = DockHost::None;
DockHost g_lastRefused = DockHost::None;
DockHost g_lastOrphanApply = DockHost::None;

}  // namespace

void TimelineDock_SetTab(DockTab tab) { g_tab = tab; }
DockTab TimelineDock_Tab() { return g_tab; }

const char* TimelineDock_TabName() {
	switch (g_tab) {
	case DockTab::Camera: return "camera";
	case DockTab::Graph: return "graph";
	default: return "native";
	}
}

bool TimelineDock_ParseTab(const char* name, DockTab& out) {
	if (!name) return false;
	// The console has accepted these spellings since before the dock was shared; keep every one
	// of them working, or a user's binds break for a refactor they cannot see.
	if (0 == _stricmp(name, "native") || 0 == _stricmp(name, "cs2") || 0 == _stricmp(name, "off")
		|| 0 == _stricmp(name, "regular")) {
		out = DockTab::Native;
		return true;
	}
	if (0 == _stricmp(name, "timeline") || 0 == _stricmp(name, "camera")) {
		out = DockTab::Camera;
		return true;
	}
	if (0 == _stricmp(name, "graph")) {
		out = DockTab::Graph;
		return true;
	}
	return false;
}

int TimelineDock_HeightFor(DockTab tab) {
	switch (tab) {
	case DockTab::Camera: return kHeightCamera;
	case DockTab::Graph: return kHeightGraph;
	default: return kHeightNative;
	}
}

int TimelineDock_Height() { return TimelineDock_HeightFor(g_tab); }

bool TimelineDock_Claim(DockHost host) {
	if (host == DockHost::None) return false;
	if (g_host == host) { g_lastRefused = DockHost::None; return true; }
	if (Rank(host) < Rank(g_host)) {
		// THE failure this instrumentation exists for: a workspace that is on screen, whose tabs
		// change g_tab, but which never gets to apply it because something higher-ranked still
		// holds the dock. If the holder is an editor that is no longer open, its Release was
		// missed and the dock is stuck.
		if (MvmDebugLog_Active() && g_lastRefused != host) {
			g_lastRefused = host;
			MvmDebugLog_Linef("ui.dock", "claim REFUSED host=%s owner=%s tab=%s",
				HostName(host), HostName(g_host), TimelineDock_TabName());
		}
		return false;
	}
	g_lastRefused = DockHost::None;
	g_host = host;
	if (MvmDebugLog_Active() && g_lastOwner != g_host) {
		g_lastOwner = g_host;
		MvmDebugLog_Linef("ui.dock", "owner -> %s tab=%s", HostName(g_host), TimelineDock_TabName());
	}
	return true;
}

void TimelineDock_Release(DockHost host) {
	if (g_host != host) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("ui.dock", "release IGNORED by=%s owner=%s", HostName(host), HostName(g_host));
		return;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("ui.dock", "release by=%s tab=%s", HostName(host), TimelineDock_TabName());
	g_lastOwner = DockHost::None;
	g_host = DockHost::None;
	// Park the dock: whatever we were showing has to come down with its host, or the overlay
	// outlives the workspace that owned it and floats over the plain game.
	CameraTimelineHudRef().SetEditorHosted(false);
	CameraTimelineHudRef().SetVisible(false);
	GraphEditorExperiment_Set(false);
}

DockHost TimelineDock_Host() { return g_host; }

void TimelineDock_Pick(const char* name) {
	DockTab tab;
	if (TimelineDock_ParseTab(name, tab)) {
		g_tab = tab;
		// The click landed and the tab moved. If the dock still shows the old editor after this
		// line, the problem is downstream (ownership / apply), not the button.
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("ui.dock", "pick \"%s\" -> tab=%s owner=%s",
				name ? name : "", TimelineDock_TabName(), HostName(g_host));
		return;
	}
	if (name && 0 == _stricmp(name, "toggle")) {
		// Toggle flips between the two OVERLAYS, never into Native -- that is what the editor's
		// old `curveeditor toggle` did, and a toggle that can land on "no editor" is a switch
		// that sometimes does nothing visible.
		g_tab = (g_tab == DockTab::Camera) ? DockTab::Graph : DockTab::Camera;
	}
}

void TimelineDock_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : nullptr;
	if (!arg) {
		// The HOST matters as much as the tab: nothing applies the dock unless a workspace owns
		// it, so "camera / nobody" is the signature of a panel that never gets told it is hosted
		// and falls back to its standalone card.
		const char* hostName = g_host == DockHost::CameraEditor ? "camera editor"
			: (g_host == DockHost::Config ? "config" : "nobody");
		advancedfx::Message("%s dock is \"%s\" (height %d), hosted by %s"
			" [timeline: hosted=%s visible=%s].\n", cmd,
			TimelineDock_TabName(), TimelineDock_Height(), hostName,
			CameraTimelineHudRef().EditorHosted() ? "yes" : "NO",
			CameraTimelineHudRef().Visible() ? "yes" : "NO");
		advancedfx::Message("usage: %s dock [native|camera|graph|toggle]\n", cmd);
		return;
	}
	DockTab before = g_tab;
	TimelineDock_Pick(arg);
	if (g_tab == before && 0 != _stricmp(arg, TimelineDock_TabName())) {
		advancedfx::Warning("%s dock: unknown tab \"%s\" (native|camera|graph|toggle).\n", cmd, arg);
		return;
	}
	advancedfx::Message("mirv_filmmaker: dock = %s.\n", TimelineDock_TabName());
}

void TimelineDock_Apply(DockHost host) {
	if (g_host != host || host == DockHost::None) {
		if (MvmDebugLog_Active() && host != DockHost::None && g_lastOrphanApply != host) {
			g_lastOrphanApply = host;
			MvmDebugLog_Linef("ui.dock", "apply SKIPPED host=%s owner=%s tab=%s",
				HostName(host), HostName(g_host), TimelineDock_TabName());
		}
		return;
	}
	g_lastOrphanApply = DockHost::None;
	const bool camera = g_tab == DockTab::Camera;
	const bool graph = g_tab == DockTab::Graph;
	// Re-asserted every frame on purpose (cheap): a stray timeline close or a HUD recreation
	// would otherwise leave the workspace half torn down, which is the bug this mirrors from
	// CameraEditorHud's own comment.
	CameraTimelineHudRef().SetEditorHosted(true);
	CameraTimelineHudRef().SetVisible(camera);
	GraphEditorExperiment_Set(graph);
}

}  // namespace Filmmaker
