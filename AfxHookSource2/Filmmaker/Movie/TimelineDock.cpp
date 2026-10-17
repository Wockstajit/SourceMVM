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
bool GraphEditorExperiment_Enabled();

namespace {

DockTab g_tab = DockTab::Native;
DockHost g_host = DockHost::None;

// Design dock heights (Docs/panorama/panorama_ui_guide.md), ribbon included: the dock is
// bottom-anchored and its height changes per tab. TIMELINE is the 440px dock under the 36px ribbon.
constexpr int kHeightNative = 126;
constexpr int kHeightTimeline = 476;

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
// that explains "TIMELINE did nothing, the native timeline stayed up".
DockHost g_lastOwner = DockHost::None;
DockHost g_lastRefused = DockHost::None;
DockHost g_lastOrphanApply = DockHost::None;

}  // namespace

void TimelineDock_SetTab(DockTab tab) { g_tab = tab; }
DockTab TimelineDock_Tab() { return g_tab; }

const char* TimelineDock_TabName() {
	return g_tab == DockTab::Timeline ? "timeline" : "native";
}

bool TimelineDock_ParseTab(const char* name, DockTab& out) {
	if (!name) return false;
	if (0 == _stricmp(name, "native") || 0 == _stricmp(name, "cs2") || 0 == _stricmp(name, "off")
		|| 0 == _stricmp(name, "regular")) {
		out = DockTab::Native;
		return true;
	}
	// The CAMERA and GRAPH tabs became the one TIMELINE; their names are gone with them.
	if (0 == _stricmp(name, "timeline")) {
		out = DockTab::Timeline;
		return true;
	}
	return false;
}

int TimelineDock_HeightFor(DockTab tab) {
	return tab == DockTab::Timeline ? kHeightTimeline : kHeightNative;
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
		// With one editor tab left, toggle is REGULAR TIMELINE <-> TIMELINE.
		g_tab = (g_tab == DockTab::Timeline) ? DockTab::Native : DockTab::Timeline;
	}
}

void TimelineDock_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : nullptr;
	if (!arg) {
		// The HOST matters as much as the tab: nothing applies the dock unless a workspace owns
		// it, so "timeline / nobody" is the signature of a panel that never gets told it is hosted.
		const char* hostName = g_host == DockHost::CameraEditor ? "camera editor"
			: (g_host == DockHost::Config ? "config" : "nobody");
		advancedfx::Message("%s dock is \"%s\" (height %d), hosted by %s"
			" [native bar hosted=%s; timeline dock enabled=%s].\n", cmd,
			TimelineDock_TabName(), TimelineDock_Height(), hostName,
			CameraTimelineHudRef().EditorHosted() ? "yes" : "NO",
			GraphEditorExperiment_Enabled() ? "yes" : "NO");
		advancedfx::Message("usage: %s dock [native|timeline|toggle]\n", cmd);
		return;
	}
	DockTab before = g_tab;
	TimelineDock_Pick(arg);
	if (g_tab == before && 0 != _stricmp(arg, TimelineDock_TabName()) && 0 != _stricmp(arg, "toggle")) {
		advancedfx::Warning("%s dock: unknown tab \"%s\" (native|timeline|toggle).\n", cmd, arg);
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
	// Re-asserted every frame on purpose (cheap): a stray close or a HUD recreation would otherwise
	// leave the workspace half torn down. The camera timeline stays HOSTED on both tabs (it owns the
	// native bar's patches and the gameplay-HUD hide) but is never docked any more: the TIMELINE tab
	// is the unified dock.
	CameraTimelineHudRef().SetEditorHosted(true);
	CameraTimelineHudRef().SetVisible(false);
	GraphEditorExperiment_Set(g_tab == DockTab::Timeline);
}

}  // namespace Filmmaker
