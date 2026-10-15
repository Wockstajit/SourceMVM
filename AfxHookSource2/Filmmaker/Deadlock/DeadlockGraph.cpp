#include "DeadlockGraph.h"

#include "DeadlockDock.h"
#include "DeadlockShot.h"
#include "DeadlockReplaySessionMath.h"
#include "DeadlockWorkspaces.h"

#include "../Debug/MvmDebugLog.h"
#include "../Movie/CamMarkers.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/GraphExpModel.h"
#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../Deadlock/DeadlockInput.h"
#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <windows.h> // GetAsyncKeyState/VK_* -- same live-read pattern CameraRuntime_GetUiCursor
                     // already uses for Shift, for Ctrl+A (select all) and Delete (delete selection)

namespace Filmmaker {

namespace {

struct GraphSession {
	GraphExpModel model;
	bool seeded = false;
	bool smooth = true;
};
DeadlockReplaySessions<GraphSession> g_sessions;
GraphExpModel& g_model = g_sessions.Current().model;
bool& g_seeded = g_sessions.Current().seeded;
bool g_drive = false;
bool g_waitForRelease = false;

// The last interpolation mode asked for. The camera timeline draws Linear | Smooth as a segmented
// pair and has to light the active half; without this it could set the mode but never show it.
bool& g_smooth = g_sessions.Current().smooth;

// The plot rectangle, in window-client pixels. C++ owns this rather than measuring JavaScript's
// layout: the cursor probe reports client pixels, so the conversion has to happen where the
// rectangle is known, and a rectangle round-tripped through a panel attribute is stale by
// construction.
//
// The numbers match what the JS body lays out (rail 188 + body padding 12, header 44 + the
// toolbar rows), and the JS reads them back out of the state so the two cannot drift.
struct PlotRect {
	int x = 0, y = 0, w = 0, h = 0;
} g_plot;

// The dock's own rectangle, pushed in by the workspace layout. Zero until the workspace has been
// laid out once, which is a real state -- no plot exists before there is a dock to hold it.
struct DockRect {
	int x = 0, y = 0, w = 0, h = 0;
} g_dock;

// The panel's fixed bands, matching DeadlockGraphBodyJs.h exactly. These two files are the one
// place the same numbers appear twice, so they are named identically in both.
//
// FIXED 2026-09-13: these had drifted from the JS (38/190/18/20 here vs. the JS's real 58/212/
// 22/26, per DeadlockGraphBodyJs.h's own "a 58px toolbar, a 212px channel column, a 22px ruler
// and a 26px footer" comment) -- every hit test in this file was therefore solving against a
// plot rectangle tens of pixels off from what was actually drawn. Restored to match the JS.
//
// 2026-09-14: both files moved to CS2's graph geometry (Panorama/GraphEditorJs.h: HEADER_H 64,
// LEFTW 236, RULER_H 26, FOOTER_H 26; rows of 34 from RULER_H + 24, 8px padding, a 64px value
// cell, a 6px gap and a 24px eye).
const int kHeaderHeight = 64;
const int kChannelColumnWidth = 236;
const int kRulerHeight = 26;
const int kFooterHeight = 26;

// The channel column's row geometry, mirrored from DeadlockGraphBodyJs.h's buildLeft(): a 34px
// header band, 30px rows, 12px row padding, and -- right to left -- a 16px eye toggle, a 10px
// gap, then a 56px value cell. The eye and the swatch+name area are real hittest Panorama panels
// with their own onactivate (channel show/hide, solo); the value cell has no panel behind it at
// all -- Panorama gives this HUD context no mouse-move event, so a SCRUB needs the OS-side cursor
// probe the same way a keyframe drag does -- so it is the one sub-rect this file hit-tests itself.
const int kRowsY0 = kRulerHeight + 24;
const int kRowHeight = 34;
const int kRowPadding = 8;
const int kEyeWidth = 24;
const int kValueGap = 6;
const int kValueWidth = 64;

// Per-channel value range, recomputed on seed. Curves are normalised into the plot with this, so
// a position channel measured in thousands of units and a FOV channel measured in degrees are
// both legible at once instead of one flattening the other.
struct Range {
	double lo = 0.0, hi = 1.0;
} g_range[GraphExpModel::kChannelCount];

// Sampling density for the drawn polylines. Enough that a cubic segment reads as a curve rather
// than as a chain of chords, and few enough that the JavaScript panel pool stays small.
const int kSamplesPerChannel = 96;

const char* const kChannelNames[GraphExpModel::kChannelCount] = {
	"X", "Y", "Z", "Pitch", "Yaw", "Roll", "FOV",
};

// Deadlock's own palette, so the curves read as part of the game rather than as a debug overlay.
// Values from citadel_base_styles.css; see Docs/deadlock/ui-style.md.
const char* const kChannelColors[GraphExpModel::kChannelCount] = {
	"#FF410D",  // colorEnemy         X
	"#5FE69E",  // brandGreen         Y
	"#1A9FFF",  // steamBlue          Z
	"#FFED79",  // colorGold          Pitch
	"#CE90FF",  // spiritBrightColor  Yaw
	"#3FDCFF",  // enhancedColorBright Roll
	"#DEC5A0",  // eggshell           FOV
};

void RecomputeLayout() {
	// Derived from the dock, in window-client pixels, because that is what the cursor probe
	// reports and the conversion has to happen where the rectangle is known. It used to be solved
	// against a hardcoded 1920x1080, so every drag on any other screen height hit the wrong key.
	if (g_dock.w <= 0 || g_dock.h <= 0) {
		g_plot.x = g_plot.y = 0;
		g_plot.w = g_plot.h = 0;
		return;
	}
	g_plot.x = g_dock.x + kChannelColumnWidth;
	g_plot.y = g_dock.y + kHeaderHeight + kRulerHeight;
	g_plot.w = g_dock.w - kChannelColumnWidth;
	g_plot.h = g_dock.h - kHeaderHeight - kRulerHeight - kFooterHeight;
	if (g_plot.w < 16) g_plot.w = 16;
	if (g_plot.h < 16) g_plot.h = 16;
}

void RecomputeRanges() {
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		const GraphExpModel::Channel& channel = g_model.GetChannel(ch);
		if (channel.keys.empty()) {
			g_range[ch].lo = 0.0;
			g_range[ch].hi = 1.0;
			continue;
		}
		double lo = channel.keys.front().value;
		double hi = lo;
		for (const GraphExpModel::Keyframe& k : channel.keys) {
			if (k.value < lo) lo = k.value;
			if (k.value > hi) hi = k.value;
		}
		// A flat channel has zero span, which would divide by zero on normalise. Give it a
		// nominal window so a constant curve draws as a horizontal line through the middle.
		if (hi - lo < 1e-6) {
			const double pad = (std::fabs(hi) > 1.0) ? std::fabs(hi) * 0.1 : 1.0;
			lo -= pad;
			hi += pad;
		} else {
			const double pad = (hi - lo) * 0.08;
			lo -= pad;
			hi += pad;
		}
		g_range[ch].lo = lo;
		g_range[ch].hi = hi;
	}
}

double TickSpanLo() {
	const double lo = g_model.MinTick();
	const double hi = g_model.MaxTick();
	return (hi > lo) ? lo : (double)Shot_FirstTick();
}

double TickSpanHi() {
	const double lo = g_model.MinTick();
	const double hi = g_model.MaxTick();
	if (hi > lo) return hi;
	const int last = Shot_LastTick();
	return (last > Shot_FirstTick()) ? last : Shot_FirstTick() + 1;
}

double NormTick(double tick) {
	const double lo = TickSpanLo();
	const double hi = TickSpanHi();
	if (hi <= lo) return 0.0;
	double t = (tick - lo) / (hi - lo);
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return t;
}

double NormValue(int ch, double value) {
	const Range& r = g_range[ch];
	if (r.hi <= r.lo) return 0.5;
	double v = (value - r.lo) / (r.hi - r.lo);
	if (v < 0.0) v = 0.0;
	if (v > 1.0) v = 1.0;
	// Screen y grows downward, so a high value has to sit near the top.
	return 1.0 - v;
}

double PixelToTick(int px) {
	const double lo = TickSpanLo();
	const double hi = TickSpanHi();
	if (g_plot.w <= 0) return lo;
	double t = (double)(px - g_plot.x) / (double)g_plot.w;
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return lo + t * (hi - lo);
}

double PixelToValue(int ch, int py) {
	const Range& r = g_range[ch];
	if (g_plot.h <= 0) return r.lo;
	double v = (double)(py - g_plot.y) / (double)g_plot.h;
	if (v < 0.0) v = 0.0;
	if (v > 1.0) v = 1.0;
	return r.lo + (1.0 - v) * (r.hi - r.lo);
}

bool InPlot(int px, int py) {
	return px >= g_plot.x && px < g_plot.x + g_plot.w
	    && py >= g_plot.y && py < g_plot.y + g_plot.h;
}

// The keyframe nearest the cursor, within a grab radius. Editable channels only, so a hidden or
// non-solo channel cannot be grabbed by accident through a visible one.
bool PickKey(int px, int py, int& outChannel, int& outId) {
	const int kGrabPixels = 14;
	double bestDistance = (double)(kGrabPixels * kGrabPixels);
	bool found = false;

	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (!g_model.IsEditable(ch)) continue;
		const GraphExpModel::Channel& channel = g_model.GetChannel(ch);
		for (const GraphExpModel::Keyframe& k : channel.keys) {
			const double kx = g_plot.x + NormTick(k.tick) * g_plot.w;
			const double ky = g_plot.y + NormValue(ch, k.value) * g_plot.h;
			const double dx = kx - px;
			const double dy = ky - py;
			const double d2 = dx * dx + dy * dy;
			if (d2 <= bestDistance) {
				bestDistance = d2;
				outChannel = ch;
				outId = k.id;
				found = true;
			}
		}
	}
	return found;
}

double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// The ruler band above the plot (drag it to scrub) and the playhead LINE inside the plot (grab it
// directly), both ported from CS2's GraphEditorJs.h processInput -- previously neither existed:
// an empty-plot click always scrubbed immediately, which left no room for a rectangle drag-select.
bool InRuler(int px, int py) {
	if (g_plot.w <= 0) return false;
	const int y0 = g_dock.y + kHeaderHeight;
	const int y1 = y0 + kRulerHeight;
	return px >= g_plot.x && px < g_plot.x + g_plot.w && py >= y0 && py < y1;
}

double PlayheadPixelX() { return g_plot.x + NormTick(Shot_PlayheadTick()) * g_plot.w; }

bool NearPlayheadLine(int px, int py) {
	const int kGrabPixels = 8;
	return std::fabs((double)px - PlayheadPixelX()) <= (double)kGrabPixels;
}

// The left column's value cell for one channel, in window-client pixels -- the one sub-rect in
// the channel column with no real Panorama panel behind it (see the kRowsY0 block comment above).
int PickValueCell(int px, int py) {
	if (g_dock.w <= 0 || g_dock.h <= 0) return -1;
	const int x1 = g_dock.x + kChannelColumnWidth - kRowPadding - kValueGap - kEyeWidth;
	const int x0 = x1 - kValueWidth;
	if (px < x0 || px >= x1) return -1;
	const int colTop = g_dock.y + kHeaderHeight;
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		const int y0 = colTop + kRowsY0 + ch * kRowHeight;
		const int y1 = y0 + (kRowHeight - 4);
		if (py >= y0 && py < y1) return ch;
	}
	return -1;
}

struct HandleEndpoint {
	double tick = 0.0;
	double value = 0.0;
	double span = 0.0;
};

// Where a Bezier handle's dot actually sits, side -1 = left (toward the previous key) or +1 =
// right (toward the next). Mirrors CS2's own handleEndpoint (Panorama/GraphEditorJs.h) and the
// control-point placement GraphExpModel::BezierSegment itself uses, so the handle drawn, the
// handle dragged and the curve it bends can never disagree about where the arm is.
HandleEndpoint ComputeHandleEndpoint(const GraphExpModel::Channel& channel,
                                      const GraphExpModel::Keyframe& k, int side) {
	int idx = -1;
	for (size_t i = 0; i < channel.keys.size(); ++i) {
		if (channel.keys[i].id == k.id) { idx = (int)i; break; }
	}
	const double defSpan = (TickSpanHi() - TickSpanLo()) * 0.06;
	HandleEndpoint e;
	if (side > 0) {
		const bool hasNext = idx >= 0 && (size_t)(idx + 1) < channel.keys.size();
		e.span = hasNext ? (channel.keys[idx + 1].tick - k.tick) : defSpan;
		const double tx = k.cpRight.active ? Clamp01(k.cpRight.tx) : (1.0 / 3.0);
		e.tick = k.tick + tx * e.span;
		e.value = k.value + (k.cpRight.active ? k.cpRight.dv : 0.0);
	} else {
		const bool hasPrev = idx > 0;
		e.span = hasPrev ? (k.tick - channel.keys[idx - 1].tick) : defSpan;
		const double tx = k.cpLeft.active ? Clamp01(k.cpLeft.tx) : (1.0 / 3.0);
		e.tick = k.tick - tx * e.span;
		e.value = k.value + (k.cpLeft.active ? k.cpLeft.dv : 0.0);
	}
	return e;
}

// The Bezier handle nearest the cursor, among the SELECTED keys of editable channels only --
// handles only ever show for a key that is both selected and on a channel solo isn't hiding.
bool PickHandle(int px, int py, int& outChannel, int& outId, int& outSide) {
	const int kGrabPixels = 14;
	double bestDistance = (double)(kGrabPixels * kGrabPixels);
	bool found = false;
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (!g_model.IsEditable(ch)) continue;
		const GraphExpModel::Channel& channel = g_model.GetChannel(ch);
		for (const GraphExpModel::Keyframe& k : channel.keys) {
			if (!g_model.IsSelected(ch, k.id)) continue;
			for (int side = -1; side <= 1; side += 2) {
				const HandleEndpoint e = ComputeHandleEndpoint(channel, k, side);
				const double hx = g_plot.x + NormTick(e.tick) * g_plot.w;
				const double hy = g_plot.y + NormValue(ch, e.value) * g_plot.h;
				const double dx = hx - px, dy = hy - py;
				const double d2 = dx * dx + dy * dy;
				if (d2 <= bestDistance) {
					bestDistance = d2;
					outChannel = ch; outId = k.id; outSide = side;
					found = true;
				}
			}
		}
	}
	return found;
}

// Deletes every currently-selected key across every channel. GraphExpModel::DeleteKey already
// drops a key's own id out of the selection set as it goes, so nothing else needs clearing after.
int DeleteSelectedKeys() {
	int deleted = 0;
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		std::vector<int> ids;
		for (const GraphExpModel::Keyframe& k : g_model.GetChannel(ch).keys) {
			if (g_model.IsSelected(ch, k.id)) ids.push_back(k.id);
		}
		for (int id : ids) {
			if (g_model.DeleteKey(ch, id)) ++deleted;
		}
	}
	return deleted;
}

// Every editable-channel key whose SCREEN position falls inside the rectangle, in window-client
// pixels. Mirrors CS2's keysInRect (GraphEditorJs.h): the rectangle is a screen-space lasso, not
// a tick/value one, so a channel with a taller value range is not easier or harder to select in.
void SelectKeysInRect(int sx, int sy, int ex, int ey) {
	const int x0 = (sx < ex) ? sx : ex;
	const int x1 = (sx > ex) ? sx : ex;
	const int y0 = (sy < ey) ? sy : ey;
	const int y1 = (sy > ey) ? sy : ey;
	bool any = false;
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (!g_model.IsEditable(ch)) continue;
		const GraphExpModel::Channel& channel = g_model.GetChannel(ch);
		for (const GraphExpModel::Keyframe& k : channel.keys) {
			const double kx = g_plot.x + NormTick(k.tick) * g_plot.w;
			const double ky = g_plot.y + NormValue(ch, k.value) * g_plot.h;
			if (kx < x0 || kx > x1 || ky < y0 || ky > y1) continue;
			if (!any) { g_model.ClearSelection(); any = true; }
			g_model.SelectAdd(ch, k.id);
		}
	}
	if (!any) g_model.ClearSelection();
}

// The right-click Ease / Align / Delete menu. C++ owns its rectangle exactly like it owns the
// plot's: JavaScript draws a fixed-size, fixed-position panel from `x`/`y`/the same width/row
// constants below and hit-tests nothing itself, so the drawn menu and the clicked menu can never
// disagree about where an item is -- the same reason the plot itself is never measured by JS.
struct ContextMenu {
	bool open = false;
	int channel = -1; // the right-clicked key: Align Keys' target tick
	int keyId = 0;
	int x = 0, y = 0; // top-left, window-client pixels
	static const int kWidth = 152;
	static const int kHeaderBand = 22; // its own small "SELECTION" caption row, not the graph's kHeaderHeight
	static const int kRowHeight = 26;
	static const int kItemCount = 5; // Ease In, Ease Out, Ease In/Out, Align Keys, Delete
};
ContextMenu g_menu;

void OpenContextMenu(int channel, int keyId, int atX, int atY) {
	g_menu.open = true;
	g_menu.channel = channel;
	g_menu.keyId = keyId;
	const int h = ContextMenu::kHeaderBand + ContextMenu::kItemCount * ContextMenu::kRowHeight;
	int x = atX, y = atY;
	if (g_dock.w > 0) {
		if (x + ContextMenu::kWidth > g_dock.x + g_dock.w) x = g_dock.x + g_dock.w - ContextMenu::kWidth;
		if (x < g_dock.x) x = g_dock.x;
	}
	if (g_dock.h > 0) {
		if (y + h > g_dock.y + g_dock.h) y = g_dock.y + g_dock.h - h;
		if (y < g_dock.y) y = g_dock.y;
	}
	g_menu.x = x;
	g_menu.y = y;
}

void CloseContextMenu() { g_menu.open = false; }

// Which item (0..4) sits under (px,py), or -1 -- computed from the SAME constants OpenContextMenu
// placed the menu with, never from anything JavaScript reports back.
int ContextMenuHit(int px, int py) {
	if (!g_menu.open) return -1;
	if (px < g_menu.x || px >= g_menu.x + ContextMenu::kWidth) return -1;
	const int rel = py - (g_menu.y + ContextMenu::kHeaderBand);
	if (rel < 0) return -1;
	const int item = rel / ContextMenu::kRowHeight;
	if (item < 0 || item >= ContextMenu::kItemCount) return -1;
	return item;
}

void RunContextMenuItem(int item) {
	switch (item) {
	case 0: // Ease In
		g_model.BeginEdit();
		g_model.SetEase(0, true);
		g_model.EndEdit();
		RecomputeRanges();
		break;
	case 1: // Ease Out
		g_model.BeginEdit();
		g_model.SetEase(1, true);
		g_model.EndEdit();
		RecomputeRanges();
		break;
	case 2: // Ease In / Out
		g_model.BeginEdit();
		g_model.SetEase(2, true);
		g_model.EndEdit();
		RecomputeRanges();
		break;
	case 3: { // Align Keys: snap the whole selection to the right-clicked key's own tick.
		const GraphExpModel::Keyframe* anchor = g_model.FindKeyConst(g_menu.channel, g_menu.keyId);
		if (anchor) {
			g_model.BeginEdit();
			const int moved = g_model.AlignSelectionToTick(anchor->tick);
			g_model.EndEdit();
			RecomputeRanges();
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("deadlock.graph", "align moved=%d tick=%.1f", moved, anchor->tick);
		}
		break;
	}
	case 4: { // Delete
		g_model.BeginEdit();
		const int deleted = DeleteSelectedKeys();
		g_model.EndEdit();
		RecomputeRanges();
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("deadlock.graph", "delete-selected (menu) count=%d", deleted);
		break;
	}
	default:
		break;
	}
}

// One gesture at a time, tagged by what press started it -- replaces the old single-purpose
// key-drag-only Drag struct now that a press can also start a handle drag, a rectangle
// drag-select, or a value-cell scrub.
enum class GestureType { None, Key, Handle, Rect, Value, Ruler };

struct Gesture {
	GestureType type = GestureType::None;
	int channel = -1;
	int keyId = 0;
	int side = 0;             // handle: -1 left, +1 right
	double lastTick = 0.0;    // key drag: last cursor tick/value, for a per-frame delta
	double lastValue = 0.0;
	double handleSpan = 0.0;  // handle drag: the span captured at press time
	int sx = 0, sy = 0;       // rect: press corner, window-client pixels
	int cx = 0, cy = 0;       // rect: current corner
	int startPx = 0;          // value scrub: press x and the value it started from
	double startValue = 0.0;
	bool moved = false;
};
Gesture g_gesture;

bool g_lastLmb = false;
bool g_lastRmb = false;
bool g_lastCtrlA = false;
bool g_lastDelete = false;

void AppendNumber(std::string& out, double value, int decimals) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
	out += buf;
}

} // namespace

void Graph_SeedFromShot() {
	std::vector<CamMarker> markers;
	const int count = Shot_MarkerCount();
	markers.reserve((size_t)(count > 0 ? count : 0));
	for (int i = 0; i < count; ++i) {
		if (const CamMarker* m = Shot_Marker(i)) markers.push_back(*m);
	}

	g_model.SeedFromMarkers(markers);
	RecomputeRanges();
	RecomputeLayout();
	g_seeded = true;

	advancedfx::Message("AfxHookDeadlock: graph seeded from %d marker(s), %d keyframe(s).\n",
	                    count, g_model.TotalKeys());
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.graph",
			"seeded markers=%d keys=%d ticks=%.0f..%.0f",
			count, g_model.TotalKeys(), TickSpanLo(), TickSpanHi());
}

void Graph_SetReplay(const std::wstring& demo) {
	g_sessions.Switch(demo);
	// Authored curves/history stay with the replay; gestures and ownership never do.
	g_drive = false;
	g_gesture = Gesture();
	g_menu.open = false;
	g_lastLmb = g_lastRmb = g_lastCtrlA = g_lastDelete = false;
	g_waitForRelease = true;
	RecomputeRanges();
	RecomputeLayout();
}

void Graph_Clear() {
	g_model.Clear();
	g_gesture = Gesture();
	g_menu.open = false;
	g_seeded = false;
}

void Graph_ToggleChannel(int channel) {
	if (channel < 0 || channel >= GraphExpModel::kChannelCount) return;
	g_model.SetVisible(channel, !g_model.GetChannel(channel).visible);
}

bool Graph_ChannelVisible(int channel) {
	if (channel < 0 || channel >= GraphExpModel::kChannelCount) return false;
	return g_model.GetChannel(channel).visible;
}

const char* Graph_ChannelName(int channel) {
	if (channel < 0 || channel >= GraphExpModel::kChannelCount) return "?";
	return kChannelNames[channel];
}

namespace {
// CS2's DriveCameraThisFrame range gate: outside the path's own ticks every curve clamps to its end
// key, and a per-frame write would pin the camera to that key for the rest of the replay -- CS2's
// "the viewport suddenly snaps back to a camera-path point" report.
bool PlayheadInsidePath() {
	const int n = Shot_MarkerCount();
	if (n < 2) return true;
	const CamMarker* first = Shot_Marker(0);
	const CamMarker* last = Shot_Marker(n - 1);
	if (!first || !last) return true;
	const double t = Shot_PlayheadTick();
	return t >= (double)first->tick && t <= (double)last->tick;
}
} // namespace

void Graph_RunMainThreadFrame() {
	// DRIVE IS DECIDED ON ENTERING THE GRAPH TAB, as CS2's GraphEditorExperimentHud::OnEnter does
	// (`m_drive = CameraEditor_Active() ? Cursor() : true`): with the UI cursor up you are editing
	// curves and the preview follows them; in game-mouse mode you are flying the free cam and the
	// graph leaves it alone. Leaving the tab, or closing the workspace, stops it. `graph drive` on
	// the console still overrides until the next enter/exit.
	{
		static bool s_onGraph = false;
		const bool onGraph = Dock_Tab() == DockTab::Graph
			&& Workspaces_Current() != DeadlockWorkspace::Off;
		if (onGraph != s_onGraph) {
			s_onGraph = onGraph;
			g_drive = onGraph && Deadlock::Input_UiMouse();
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("deadlock.graph", "%s drive=%d keys=%d uiMouse=%d",
					onGraph ? "enter" : "exit", g_drive ? 1 : 0, g_model.TotalKeys(),
					Deadlock::Input_UiMouse() ? 1 : 0);
		}
	}

	if (!g_seeded && Shot_MarkerCount() >= 2) Graph_SeedFromShot();

	RecomputeLayout();

	// THE GATE. This function reads the OS cursor directly, every main-thread frame, whether or not
	// the graph is on screen -- so without it a left-click while flying the free camera drags
	// whichever keyframe happens to lie under the pointer. Both halves are needed: the tab says the
	// plot is visible, the mouse mode says the click was aimed at the UI rather than at the game.
	//
	// The drive pass below is deliberately OUTSIDE the gate: a curve that is driving the camera has
	// to keep driving it while you look at the timeline instead.

	int px = 0, py = 0;
	bool lmb = false, rmb = false, shift = false;
	unsigned seq = 0;
	Deadlock::CameraRuntime_GetUiCursor(px, py, lmb, rmb, shift, seq);
	if (!lmb && !rmb) g_waitForRelease = false;
	const bool interactive = !g_waitForRelease && Deadlock::Engine_DemoTick() >= 0
		&& Workspaces_Current() != DeadlockWorkspace::Off
		&& Dock_Tab() == DockTab::Graph && Deadlock::Input_UiMouse();
	if (!interactive) {
		// Release anything mid-gesture rather than freezing it: a drag or an open menu abandoned by
		// closing the tab would otherwise resume from a stale anchor (or stay open) the next time
		// the tab comes back.
		if (g_gesture.type == GestureType::Handle || g_gesture.type == GestureType::Key ||
		    g_gesture.type == GestureType::Value) {
			g_model.EndEdit();
			RecomputeRanges();
		}
		g_gesture = Gesture();
		g_menu.open = false;
		g_lastLmb = false;
		g_lastRmb = false;
		g_lastCtrlA = false;
		g_lastDelete = false;
	}

	const bool pressed = interactive && lmb && !g_lastLmb;
	const bool released = interactive && !lmb && g_lastLmb;
	if (interactive) g_lastLmb = lmb;

	const bool rmbPressed = interactive && rmb && !g_lastRmb;
	if (interactive) g_lastRmb = rmb;

	if (interactive) {
		// Ctrl+A / Delete, read live via GetAsyncKeyState -- the SAME pattern
		// CameraRuntime_GetUiCursor already uses for Shift above, not a new input path, and not a
		// change to DeadlockHotkeys.cpp's global bindings: this only fires while the graph tab is
		// open and the UI already owns the mouse.
		const bool ctrlA = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
		                 && (GetAsyncKeyState('A') & 0x8000) != 0;
		if (ctrlA && !g_lastCtrlA) g_model.SelectAll();
		g_lastCtrlA = ctrlA;

		const bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
		if (del && !g_lastDelete && g_model.SelectionCount() > 0) {
			g_model.BeginEdit();
			const int deleted = DeleteSelectedKeys();
			g_model.EndEdit();
			RecomputeRanges();
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("deadlock.graph", "delete-selected (hotkey) count=%d", deleted);
		}
		g_lastDelete = del;
	}

	// Right-click: on a keyframe, opens the Ease/Align/Delete menu for the selection (adding the
	// clicked key to it first, unless it is already part of a multi-selection -- matching CS2's own
	// right-click contract in GraphEditorJs.h's processInput); off one, closes whatever is open.
	if (rmbPressed) {
		int rch = -1, rid = 0;
		if (InPlot(px, py) && PickKey(px, py, rch, rid)) {
			if (!g_model.IsSelected(rch, rid)) g_model.Select(rch, rid, false);
			OpenContextMenu(rch, rid, px, py);
		} else {
			CloseContextMenu();
		}
	}

	if (pressed) {
		if (g_menu.open) {
			// A press anywhere while the menu is open belongs to the menu, hit or miss -- it must
			// never also start a drag or a rectangle underneath itself.
			const int item = ContextMenuHit(px, py);
			if (item >= 0) RunContextMenuItem(item);
			CloseContextMenu();
		} else if (InPlot(px, py)) {
			int hch = -1, hid = 0, hside = 0;
			int kch = -1, kid = 0;
			if (PickHandle(px, py, hch, hid, hside)) {
				const GraphExpModel::Channel& channel = g_model.GetChannel(hch);
				const GraphExpModel::Keyframe* key = g_model.FindKeyConst(hch, hid);
				g_model.BeginEdit();
				g_gesture = Gesture();
				g_gesture.type = GestureType::Handle;
				g_gesture.channel = hch;
				g_gesture.keyId = hid;
				g_gesture.side = hside;
				g_gesture.handleSpan = key ? ComputeHandleEndpoint(channel, *key, hside).span : 1.0;
			} else if (PickKey(px, py, kch, kid)) {
				// One undo snapshot per gesture, taken at the start. Snapshotting per frame would
				// make undo step back one pixel at a time.
				g_model.Select(kch, kid, shift);
				g_model.BeginEdit();
				g_gesture = Gesture();
				g_gesture.type = GestureType::Key;
				g_gesture.channel = kch;
				g_gesture.keyId = kid;
				g_gesture.lastTick = PixelToTick(px);
				g_gesture.lastValue = PixelToValue(kch, py);
			} else if (NearPlayheadLine(px, py)) {
				g_gesture = Gesture();
				g_gesture.type = GestureType::Ruler;
				Shot_BeginScrub(PixelToTick(px));
			} else {
				// Empty plot: a rectangle drag-select, not an immediate scrub -- scrubbing now lives
				// on the ruler band above the plot and on the playhead line itself (both handled
				// above), which is also what makes a rectangle possible at all.
				g_gesture = Gesture();
				g_gesture.type = GestureType::Rect;
				g_gesture.sx = g_gesture.cx = px;
				g_gesture.sy = g_gesture.cy = py;
			}
		} else if (InRuler(px, py)) {
			g_gesture = Gesture();
			g_gesture.type = GestureType::Ruler;
			Shot_BeginScrub(PixelToTick(px));
		} else {
			const int vch = PickValueCell(px, py);
			if (vch >= 0) {
				double cur = 0.0;
				if (!g_model.SampleChannel(vch, Shot_PlayheadTick(), cur)) cur = 0.0;
				g_model.BeginEdit();
				g_gesture = Gesture();
				g_gesture.type = GestureType::Value;
				g_gesture.channel = vch;
				g_gesture.startPx = px;
				g_gesture.startValue = cur;
			}
		}
	}

	if (interactive && lmb) {
		switch (g_gesture.type) {
		case GestureType::Handle: {
			const GraphExpModel::Channel& channel = g_model.GetChannel(g_gesture.channel);
			const GraphExpModel::Keyframe* key = g_model.FindKeyConst(g_gesture.channel, g_gesture.keyId);
			if (key) {
				const double span = (g_gesture.handleSpan > 1e-9) ? g_gesture.handleSpan : 1.0;
				const double th = PixelToTick(px);
				const double vh = PixelToValue(g_gesture.channel, py);
				double tx = Clamp01(g_gesture.side > 0 ? (th - key->tick) / span : (key->tick - th) / span);
				double dv = vh - key->value;
				// Shift = STRAIGHTEN: lock the handle flat (dv=0) so the curve runs level through
				// the key -- same Shift contract as CS2's handle drag.
				if (shift) dv = 0.0;
				// Keep the handle OUTSIDE the diamond: enforce a minimum on-screen distance from the
				// key so the arm can't be dragged into its own keyframe dot and become ungrabbable.
				const double tickSpanTotal = TickSpanHi() - TickSpanLo();
				const double spanPx = (tickSpanTotal > 1e-9) ? (span / tickSpanTotal * g_plot.w) : (double)g_plot.w;
				const double ddRange = g_range[g_gesture.channel].hi - g_range[g_gesture.channel].lo;
				const double vpx = (std::fabs(ddRange) > 1e-9) ? (dv / ddRange * g_plot.h) : 0.0;
				const double hpx = tx * spanPx;
				const double dist = std::sqrt(hpx * hpx + vpx * vpx);
				const double kMinPixels = 17.0; // the drawn handle dot's radius plus a few px of slack
				if (dist < kMinPixels) {
					if (dist < 1e-3) {
						tx = Clamp01(kMinPixels / ((spanPx > 1e-6) ? spanPx : 1.0));
						dv = 0.0;
					} else {
						const double scale = kMinPixels / dist;
						tx = Clamp01(tx * scale);
						dv *= scale;
					}
				}
				// reflect=false: left/right tangents stay INDEPENDENT. GraphExpModel::SetHandle
				// defaults reflect to true, but CS2's own graph UI always sends false (its JS
				// literally passes a trailing 0) -- ported to match what the reference actually
				// does in play, not what its API merely allows.
				g_model.SetHandle(g_gesture.channel, g_gesture.keyId, g_gesture.side, tx, dv, false);
			}
			break;
		}
		case GestureType::Key: {
			const double tick = PixelToTick(px);
			const double value = PixelToValue(g_gesture.channel, py);
			const double dTick = tick - g_gesture.lastTick;
			const double dValue = value - g_gesture.lastValue;
			if (dTick != 0.0 || dValue != 0.0) {
				// Move the whole selection, not just the grabbed key: that is what makes dragging
				// several aligned keys behave the way an animation editor is expected to.
				g_model.MoveSelectedBy(dTick, dValue);
				g_gesture.lastTick = tick;
				g_gesture.lastValue = value;
			}
			break;
		}
		case GestureType::Rect:
			g_gesture.cx = px;
			g_gesture.cy = py;
			break;
		case GestureType::Value: {
			const int dpx = px - g_gesture.startPx;
			if ((dpx < 0 ? -dpx : dpx) > 5) g_gesture.moved = true;
			if (g_gesture.moved) {
				const Range& r = g_range[g_gesture.channel];
				double span = r.hi - r.lo;
				if (std::fabs(span) < 1e-9) span = 1.0;
				// The same one-three-hundredth-of-the-range-per-pixel sensitivity as CS2's
				// fieldSens, writing a keyframe at the playhead the same way typing one would.
				const double value = g_gesture.startValue + dpx * (span / 300.0);
				g_model.AddKey(g_gesture.channel, Shot_PlayheadTick(), value);
			}
			break;
		}
		case GestureType::Ruler:
			Shot_BeginScrub(PixelToTick(px));
			break;
		default:
			break;
		}
	}

	if (released) {
		switch (g_gesture.type) {
		case GestureType::Handle:
			g_model.EndEdit();
			RecomputeRanges();
			break;
		case GestureType::Key:
			g_model.EndEdit();
			RecomputeRanges();
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("deadlock.graph",
					"drag end ch=%d id=%d tick=%.1f value=%.3f",
					g_gesture.channel, g_gesture.keyId, g_gesture.lastTick, g_gesture.lastValue);
			break;
		case GestureType::Rect:
			SelectKeysInRect(g_gesture.sx, g_gesture.sy, g_gesture.cx, g_gesture.cy);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("deadlock.graph", "rect-select selection=%d", g_model.SelectionCount());
			break;
		case GestureType::Value:
			g_model.EndEdit();
			RecomputeRanges();
			break;
		case GestureType::Ruler:
			// Ends the drag AND seeks. Releasing without the seek leaves the demo tick stale and
			// the timeline immediately disagrees with the graph.
			if (Shot_Scrubbing()) Shot_EndScrub();
			break;
		default:
			if (Shot_Scrubbing()) Shot_EndScrub();
			break;
		}
		g_gesture = Gesture();
	}

	if (g_drive && g_model.TotalKeys() > 0 && PlayheadInsidePath()) {
		double current[3], angles[3], fov;
		CameraBridge_GetCurrentCamera(current, angles, fov);
		const double defaults[GraphExpModel::kChannelCount] = {
			current[0], current[1], current[2], angles[0], angles[1], angles[2], fov,
		};
		double pose[GraphExpModel::kChannelCount];
		g_model.SamplePose(Shot_PlayheadTick(), defaults, pose);
		Deadlock::CameraRuntime_PushOwnedPose(Deadlock::ViewOwner::Graph,
			pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
	}
}

bool Graph_OwnsView() {
	return g_drive && g_model.TotalKeys() > 0 && PlayheadInsidePath();
}

bool Graph_Drive() { return g_drive; }

void Graph_SetDrive(bool drive) {
	g_drive = drive;
	advancedfx::Message("AfxHookDeadlock: graph drive %s\n", drive ? "on" : "off");
}

void Graph_Undo() { g_model.Undo(); RecomputeRanges(); }
void Graph_Redo() { g_model.Redo(); RecomputeRanges(); }

// A public entry for the same delete the right-click menu's "Delete" item and the VK_DELETE poll
// already call -- CS2 parity needed a visible toolbar button (GraphEditorJs.h's "Delete Key"),
// which needs a console verb this internal-linkage DeleteSelectedKeys() cannot be, being static to
// this file.
void Graph_DeleteSelected() {
	if (g_model.SelectionCount() <= 0) return;
	g_model.BeginEdit();
	const int deleted = DeleteSelectedKeys();
	g_model.EndEdit();
	RecomputeRanges();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.graph", "delete-selected (toolbar) count=%d", deleted);
}

void Graph_SetSmooth(bool smooth, bool selectedOnly) {
	if (!selectedOnly) g_smooth = smooth;
	g_model.BeginEdit();
	g_model.SetInterpAll(smooth, selectedOnly);
	g_model.EndEdit();
}


// THE PER-FRAME HALF. The playhead and each channel's value under it move every frame while a
// shot plays; everything else in the graph state -- 7 channels x 96 sampled points -- changes
// only on an edit. They travel separately because the workspace ships the big half only when it
// changes: the sampled curves were ~12 KB recompiled every frame, and that compile was the whole
// of a 6 fps replay. The panel merges these back onto graph.playhead / channels[i].current.
void Graph_AppendHotJson(std::string& out) {
	out += ",\"graphHot\":{\"playhead\":";
	AppendNumber(out, NormTick(Shot_PlayheadTick()), 5);
	out += ",\"current\":[";
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (ch) out += ",";
		double current = 0.0;
		if (!g_model.SampleChannel(ch, Shot_PlayheadTick(), current)) current = 0.0;
		AppendNumber(out, current, 3);
	}
	out += "]}";
}

void Graph_AppendStateJson(std::string& out) {
	out += ",\"graph\":{";

	out += "\"plotX\":";
	AppendNumber(out, g_plot.x, 0);
	out += ",\"plotY\":";
	AppendNumber(out, g_plot.y, 0);
	out += ",\"plotW\":";
	AppendNumber(out, g_plot.w, 0);
	out += ",\"plotH\":";
	AppendNumber(out, g_plot.h, 0);

	out += ",\"drive\":";
	out += g_drive ? "true" : "false";
	out += ",\"smooth\":";
	out += g_smooth ? "true" : "false";
	out += ",\"keys\":";
	AppendNumber(out, g_model.TotalKeys(), 0);
	out += ",\"selection\":";
	AppendNumber(out, g_model.SelectionCount(), 0);
	out += ",\"canUndo\":";
	out += g_model.CanUndo() ? "true" : "false";
	out += ",\"canRedo\":";
	out += g_model.CanRedo() ? "true" : "false";
	// GraphExpModel's own Catmull-Rom-style smoothing needs at least 3 keyframes per channel to
	// bend at all: with exactly 2, its auto tangents land dead on the straight chord between them
	// (the cubic's control points are evenly spaced along it), so "Smooth" would sit lit while the
	// curve looks and drives EXACTLY like Linear -- Deadlock's own version of CS2's "cubic needs
	// >= 3 markers" InterpDegraded (same threshold, but a separate flag: CS2's is the STABLE
	// CameraPath's, this is GraphExpModel's own).
	out += ",\"interpDegraded\":";
	out += (g_smooth && Shot_MarkerCount() < 3) ? "true" : "false";

	// The right-click Ease/Align/Delete menu, in the panel's own LOCAL space (top-left of the
	// graph body): C++ hit-tests every click against these same numbers, so JS is only ever
	// drawing where it is told, never guessing where its own menu ended up on screen.
	out += ",\"menu\":{\"open\":";
	out += g_menu.open ? "true" : "false";
	out += ",\"x\":";
	AppendNumber(out, g_menu.x - g_dock.x, 0);
	out += ",\"y\":";
	AppendNumber(out, g_menu.y - g_dock.y, 0);
	out += "}";

	// The rectangle drag-select, normalised into the plot exactly like a curve's pts: JS only ever
	// multiplies by its own measured canvas size, never receives a raw pixel it has to interpret.
	out += ",\"rect\":{\"active\":";
	const bool rectActive = (g_gesture.type == GestureType::Rect);
	out += rectActive ? "true" : "false";
	if (rectActive && g_plot.w > 0 && g_plot.h > 0) {
		const int rsx = (g_gesture.sx < g_gesture.cx) ? g_gesture.sx : g_gesture.cx;
		const int rex = (g_gesture.sx > g_gesture.cx) ? g_gesture.sx : g_gesture.cx;
		const int rsy = (g_gesture.sy < g_gesture.cy) ? g_gesture.sy : g_gesture.cy;
		const int rey = (g_gesture.sy > g_gesture.cy) ? g_gesture.sy : g_gesture.cy;
		out += ",\"x0\":";
		AppendNumber(out, Clamp01(((double)rsx - g_plot.x) / g_plot.w), 5);
		out += ",\"y0\":";
		AppendNumber(out, Clamp01(((double)rsy - g_plot.y) / g_plot.h), 5);
		out += ",\"x1\":";
		AppendNumber(out, Clamp01(((double)rex - g_plot.x) / g_plot.w), 5);
		out += ",\"y1\":";
		AppendNumber(out, Clamp01(((double)rey - g_plot.y) / g_plot.h), 5);
	}
	out += "}";

	// Solo is independent of each channel's own eye/visible flag (see Graph_SoloChannel): what a
	// channel actually DRAWS as is visible-AND-(no-solo-OR-this-is-the-solo), exactly CS2's own
	// drawVis/IsEditable formula, computed once here rather than per-channel below.
	const bool anySolo = g_model.AnySolo();

	out += ",\"channels\":[";
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (ch) out += ",";
		const GraphExpModel::Channel& channel = g_model.GetChannel(ch);
		const bool drawVis = channel.visible && (!anySolo || channel.solo);
		out += "{\"name\":\"";
		out += kChannelNames[ch];
		out += "\",\"color\":\"";
		out += kChannelColors[ch];
		out += "\",\"visible\":";
		out += drawVis ? "true" : "false";
		out += ",\"solo\":";
		out += channel.solo ? "true" : "false";

		// REAL UNITS, alongside the normalised curve. The channel column reads each channel's
		// value at the playhead and the canvas labels its value axis, and neither can be recovered
		// from a curve that has already been flattened into the plot.
		out += ",\"lo\":";
		AppendNumber(out, g_range[ch].lo, 3);
		out += ",\"hi\":";
		AppendNumber(out, g_range[ch].hi, 3);
		// Sampled polyline, normalised into the plot. Sampling in C++ rather than shipping the
		// keyframes and re-implementing the Bezier solve in JavaScript keeps ONE evaluator: the
		// curve the panel draws is the curve the camera would follow.
		out += ",\"pts\":[";
		if (drawVis && !channel.keys.empty()) {
			const double lo = TickSpanLo();
			const double hi = TickSpanHi();
			for (int i = 0; i < kSamplesPerChannel; ++i) {
				const double t = (kSamplesPerChannel > 1)
					? (double)i / (double)(kSamplesPerChannel - 1) : 0.0;
				double value = 0.0;
				if (!g_model.SampleChannel(ch, lo + t * (hi - lo), value)) break;
				if (i) out += ",";
				AppendNumber(out, t, 5);
				out += ",";
				AppendNumber(out, NormValue(ch, value), 5);
			}
		}
		out += "]";

		out += ",\"keyPts\":[";
		if (drawVis) {
			bool first = true;
			for (const GraphExpModel::Keyframe& k : channel.keys) {
				if (!first) out += ",";
				first = false;
				const bool sel = g_model.IsSelected(ch, k.id);
				out += "{\"t\":";
				AppendNumber(out, NormTick(k.tick), 5);
				out += ",\"v\":";
				AppendNumber(out, NormValue(ch, k.value), 5);
				out += ",\"sel\":";
				out += sel ? "true" : "false";
				// Bezier handle endpoints, only for a SELECTED key -- matching CS2, which draws
				// handles for the selection only, not for every key on every visible channel.
				if (sel) {
					const HandleEndpoint left = ComputeHandleEndpoint(channel, k, -1);
					const HandleEndpoint right = ComputeHandleEndpoint(channel, k, 1);
					out += ",\"lt\":";
					AppendNumber(out, NormTick(left.tick), 5);
					out += ",\"lv\":";
					AppendNumber(out, NormValue(ch, left.value), 5);
					out += ",\"rt\":";
					AppendNumber(out, NormTick(right.tick), 5);
					out += ",\"rv\":";
					AppendNumber(out, NormValue(ch, right.value), 5);
				}
				out += "}";
			}
		}
		out += "]}";
	}
	out += "]}";
}

int Graph_KeyCount() { return g_model.TotalKeys(); }

bool Graph_Smooth() { return g_smooth; }

void Graph_SetDockRect(int x, int y, int w, int h) {
	if (g_dock.x == x && g_dock.y == y && g_dock.w == w && g_dock.h == h) return;
	g_dock.x = x;
	g_dock.y = y;
	g_dock.w = w;
	g_dock.h = h;
	RecomputeLayout();
}

void Graph_SoloChannel(int channel) {
	if (channel < 0 || channel >= GraphExpModel::kChannelCount) return;
	// FIXED 2026-09-13: solo is GraphExpModel's OWN independent flag (SetSolo/AnySolo/IsEditable,
	// exactly like CS2's), not a second use of each channel's eye/visible flag. The old
	// implementation hijacked `visible` to fake solo -- it hid every OTHER channel's eye state and
	// then handed every channel's eye back to "on" on un-solo, silently un-hiding one a viewer had
	// deliberately hidden. SetSolo is already exclusive (turning one on clears the others), so
	// toggling this one off leaves no channel soloed and every channel's own eye state untouched.
	g_model.SetSolo(channel, !g_model.GetChannel(channel).solo);
}

void Graph_Report() {
	advancedfx::Message("  graph sessions: cached=%zu\n", g_sessions.CachedCount());
	advancedfx::Message(
		"  graph:     keys=%d selection=%d drive=%s plot=%d,%d %dx%d ticks=%.0f..%.0f"
		" canUndo=%d canRedo=%d menu=%s\n",
		g_model.TotalKeys(), g_model.SelectionCount(), g_drive ? "on" : "off",
		g_plot.x, g_plot.y, g_plot.w, g_plot.h, TickSpanLo(), TickSpanHi(),
		g_model.CanUndo() ? 1 : 0, g_model.CanRedo() ? 1 : 0, g_menu.open ? "open" : "closed");
	std::string channels, solo;
	for (int ch = 0; ch < GraphExpModel::kChannelCount; ++ch) {
		if (g_model.GetChannel(ch).visible) {
			if (!channels.empty()) channels += " ";
			channels += kChannelNames[ch];
		}
		if (g_model.GetChannel(ch).solo) {
			if (!solo.empty()) solo += " ";
			solo += kChannelNames[ch];
		}
	}
	advancedfx::Message("  visible:   %s\n", channels.empty() ? "none" : channels.c_str());
	advancedfx::Message("  solo:      %s\n", solo.empty() ? "none" : solo.c_str());
}

} // namespace Filmmaker
