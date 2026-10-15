#include "DeadlockDock.h"

#include "DeadlockGraph.h"
#include "../Movie/GraphExpModel.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "DeadlockShot.h"
#include "../../../Shared/AfxConsole.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Filmmaker {

namespace {

DockTab g_tab = DockTab::Native;

// Ribbon height, from the dock spec. 36px holds a 13px bold uppercase label with 9px of padding
// above and below; below that the tab captions clip at the bottom, which is a bug that reads as
// "the UI is just ugly".
const int kRibbonHeight = 36;

// Editor heights. The NATIVE band hosts Deadlock's own replay bar, so its height is measured off
// that bar rather than chosen; this is only what is reserved before the first measurement lands.
const int kNativeFallback = 94;   // 130 total, until the bar is measured
int g_measuredNative = 0;
// The dock spec gives each tab a TOTAL height: camera 208, native 130, graph 422. The ribbon is
// part of that total, so these are the totals less kRibbonHeight. Written as plain literals rather
// than as an expression because the mockup's copy of these numbers is compared against THESE lines.
// The mockup's camera strip: 208 total, of which the ribbon takes 36. The four rows inside come to
// 171, so they fit with a pixel to spare -- there is no header row and no speed dial any more.
const int kCameraHeight = 172;   // 208 total
const int kGraphHeight = 386;    // 422 total

// The band the workspace laid out, in window-client pixels.
int g_rectX = 0, g_rectY = 0, g_rectW = 0, g_rectH = 0;

} // namespace

void Dock_SetTab(DockTab tab) {
	if (g_tab == tab) return;
	g_tab = tab;
	advancedfx::Message("AfxHookDeadlock: dock -> %s\n", Dock_TabName());
}

DockTab Dock_Tab() { return g_tab; }

const char* Dock_TabName() {
	switch (g_tab) {
	case DockTab::Camera: return "camera";
	case DockTab::Graph: return "graph";
	default: return "native";
	}
}

bool Dock_ParseTab(const char* name, DockTab* out) {
	if (!name || !out) return false;
	if (0 == _stricmp(name, "native") || 0 == _stricmp(name, "regular")) {
		*out = DockTab::Native; return true;
	}
	if (0 == _stricmp(name, "camera") || 0 == _stricmp(name, "timeline")) {
		*out = DockTab::Camera; return true;
	}
	if (0 == _stricmp(name, "graph")) { *out = DockTab::Graph; return true; }
	return false;
}

int Dock_RibbonHeight() { return kRibbonHeight; }

int Dock_EditorHeightFor(DockTab tab) {
	switch (tab) {
	case DockTab::Camera: return kCameraHeight;
	case DockTab::Graph: return kGraphHeight;
	default:
		// Trusted only inside a plausible band. A bar reported as 4px tall is one measured before
		// layout settled; one reported as 900px is the wrong panel. Either would leave the preview
		// overlapping the bar or squeezed to nothing.
		return (g_measuredNative >= 40 && g_measuredNative <= 400)
			? g_measuredNative : kNativeFallback;
	}
}

int Dock_EditorHeight() { return Dock_EditorHeightFor(g_tab); }

int Dock_Height() { return kRibbonHeight + Dock_EditorHeight(); }

void Dock_SetMeasuredNativeHeight(int pixels) { g_measuredNative = pixels; }
int Dock_MeasuredNativeHeight() { return g_measuredNative; }

void Dock_SetRect(int x, int y, int w, int h) {
	// Rejected rather than clamped when it is nonsense. A dock 4px tall is a panel measured before
	// its first layout settled, and believing it would put the graph's hit tests somewhere the
	// graph is not -- which reads as "dragging a keyframe grabs the wrong one" rather than as a
	// layout fault, and is chased in the wrong file for an afternoon.
	if (w < 64 || h < 32) return;
	g_rectX = x;
	g_rectY = y;
	g_rectW = w;
	g_rectH = h;
	Graph_SetDockRect(x, y, w, h);
}

void Dock_GetRect(int& x, int& y, int& w, int& h) {
	x = g_rectX;
	y = g_rectY;
	w = g_rectW;
	h = g_rectH;
}

void Dock_AppendStateJson(std::string& out) {
	char buf[256];
	// `meta` is the line the ribbon shows on the right: what only the active tab knows. The dock
	// cannot work it out itself, and asking each panel for it would mean three panels each keeping
	// their own copy of the shot's state.
	std::string meta;
	switch (g_tab) {
	case DockTab::Camera: {
		char m[96];
		std::snprintf(m, sizeof(m), "%d markers", Shot_MarkerCount());
		meta = m;
		break;
	}
	case DockTab::Graph: {
		// HOW MANY CHANNELS ARE ON, out of the seven. A key count says nothing you cannot already see
		// on the curves, and the thing you lose track of on this tab is which channels you soloed.
		int visible = 0;
		for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
			if (Graph_ChannelVisible(c)) ++visible;
		char m[96];
		std::snprintf(m, sizeof(m), "%d of %d channels", visible, (int)GraphExpModel::kChannelCount);
		meta = m;
		break;
	}
	default: {
		// The replay this bar is scrubbing: the level and its tickrate. Naming the tab back to the
		// user ("replay transport") tells them what they can already see.
		std::string level = Deadlock::Engine_LevelName();
		char m[160];
		// · as a JSON escape, not as literal bytes: this string is spliced straight into the
		// state JSON, and raw UTF-8 came back out of the panel as the mojibake "Â·".
		std::snprintf(m, sizeof(m), "%s \\u00b7 64 tick", level.empty() ? "no replay" : level.c_str());
		meta = m;
		break;
	}
	}

	std::snprintf(buf, sizeof(buf),
		",\"dock\":{\"tab\":\"%s\",\"ribbonH\":%d,\"editorH\":%d,\"meta\":\"%s\"}",
		Dock_TabName(), kRibbonHeight, Dock_EditorHeight(), meta.c_str());
	out += buf;
}

void Dock_RunCommand(int argc, advancedfx::ICommandArgs* args) {
	if (argc < 3) {
		advancedfx::Message("dock = %s  (native | camera | graph)\n", Dock_TabName());
		return;
	}
	if (0 == _stricmp(args->ArgV(2), "rect")) {
		if (argc < 7) {
			int x, y, w, h;
			Dock_GetRect(x, y, w, h);
			advancedfx::Message("dock rect = %d,%d %dx%d\n", x, y, w, h);
			return;
		}
		Dock_SetRect(std::atoi(args->ArgV(3)), std::atoi(args->ArgV(4)),
		             std::atoi(args->ArgV(5)), std::atoi(args->ArgV(6)));
		return;
	}

	DockTab tab;
	if (!Dock_ParseTab(args->ArgV(2), &tab)) {
		advancedfx::Warning("unknown dock tab '%s' -- native, camera or graph.\n", args->ArgV(2));
		return;
	}
	Dock_SetTab(tab);
}

void Dock_Report() {
	advancedfx::Message(
		"  dock:      %s  ribbon=%d editor=%d total=%d  rect=%d,%d %dx%d\n",
		Dock_TabName(), kRibbonHeight, Dock_EditorHeight(), Dock_Height(),
		g_rectX, g_rectY, g_rectW, g_rectH);
}

} // namespace Filmmaker
