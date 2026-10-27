#pragma once

// The Camera Editor's TIMELINE dock: CS2's dock (GraphEditorJs.h) in CS:S paint, drawn with Dear
// ImGui. The toolbar (timecode, the KEYFRAMES and GRAPH tabs, transport and speed, the interpolation
// strip, SNAP, Undo, Redo and Delete), the overview and the ruler, the CAMERA PATH layer's rows with
// their values and key navigators, the KEYFRAMES track, the playhead, and the footer's window
// controls. TimelineDockMath.h holds its rules.
//
// Engine-free: the host fills a TimelineDockModel from the dock's keys (CS2's GraphExpModel) each
// frame and applies the actions the dock returns: SourceTimelineDock.h in the game, a stand-in in the
// browser mockup. The visible window, the rows' open state, snap and every gesture are the dock's own.

#include "DemoBar.h"
#include "TimelineDockMath.h"
#include "UiStyle.h"

#include "../../../AfxHookSource2/Filmmaker/Movie/GraphExpModel.h"

#include <string>
#include <utility>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct TimelineDockModel {
	const Filmmaker::GraphExpModel* keys = nullptr; // the dock's keys; required
	bool demo = false;          // a demo is playing; without one the dock takes no edits
	unsigned viewEpoch = 0;     // a change re-applies the suggested window
	double viewT0 = 0, viewT1 = 1;     // the suggested window: the keys, padded, else the playhead +- 1000
	double domainT0 = 0, domainT1 = 0; // the demo's ticks
	double playhead = 0;
	bool scrubbing = false;
	bool playing = false;       // the demo plays (not paused)
	bool pathPlaying = false;   // the camera path flies the view
	bool drive = true;          // the CAMERA PATH layer's switch
	bool autoPath = false;      // its AUTO button
	bool smoothPath = false;    // the path's interpolation is Smooth
	double tickRate = 64;
	double timescale = 1;
	double live[7] = { 0, 0, 0, 0, 0, 0, 90 }; // the rendered camera, per channel
	// The camera path's camera shake track (CamPathShake's dock JSON: CS2's pathKeys, shakes and
	// shakeStyles): its camera keys' ticks, its keys in tick order with each one's style label, and
	// the styles the menu offers (index, label), the first shakeEssentials of them on the menu itself.
	std::vector<double> pathKeys;
	std::vector<TimelineDockMath::ShakeKey> shakes;
	std::vector<std::string> shakeLabels;
	std::vector<std::pair<int, std::string>> shakeStyles;
	int shakeEssentials = 5;
};

struct TimelineDockAction {
	enum class Kind {
		Select,          // ch, id; flag: add to the selection
		SelectSet,       // keys replace the selection
		SelectAll,
		SelectClear,
		EditBegin,       // the edits that follow are one undo step
		MoveSelected,    // a = ticks, b = value
		AddKey,          // ch, a = tick, b = value
		DeleteSelected,
		Ease,            // id: 0 in, 1 out, 2 in and out; flag: the selected keys only
		Interp,          // flag: Smooth (else Linear); id = 1: the selected keys only, else the whole path
		AlignKey,        // ch, id: every selected key moves to that key's tick
		Handle,          // ch, id, side (-1 left, 1 right): a = tx, b = dv; flag: mirror the other side
		Undo,
		Redo,
		Drive,           // flag
		AutoPath,        // flag
		Playhead,        // a = tick: the playhead is dragged there
		PlayheadRelease, // a = tick: the demo goes there and the playhead holds it
		PlayPause,       // the demo, or the camera path while it plays
		Speed,           // a = demo speed
		ShakeCommand,    // text: a camera shake edit, as "mirv_filmmaker pathshake <text>" takes it
	};
	Kind kind = Kind::Select;
	int ch = 0, id = 0, side = 0;
	double a = 0, b = 0;
	bool flag = false;
	std::vector<std::pair<int, int>> keys; // SelectSet: (channel, key id)
	std::string text;                      // ShakeCommand
};

enum class TimelineView { Keys = 0, Graph = 1 };

// What the dock keeps between frames.
struct TimelineDockState {
	TimelineView view = TimelineView::Keys;
	TimelineDockMath::Span window;      // the visible ticks: the dock owns pan and zoom
	bool seeded = false;
	unsigned seededEpoch = 0;
	TimelineDockMath::Paging paging;
	double fitT0 = -1, fitT1 = -1;      // what FIT framed last: pressed again it frames every key
	int snap = TimelineDockMath::kSnapOff;
	bool snapMenu = false;
	bool open[15] = { true, true, true, true, true, true, true, true, true, true, true, true, true, true, true }; // per row node
	bool split[2] = { false, false };   // Position and Rotation on one row per channel
	int selRow = -1;                    // the selected row's node, -1 for none
	std::vector<double> shakeSel;       // the selected camera shake keys' ticks
	bool hasShakes = false;             // this frame: the shake track has a key (its Intensity and Speed rows show)
	float rowScroll = 0;

	enum class Gesture { None, Scrub, Keys, Box, Overview, FooterBar, RowsBar, Value, Handle };
	Gesture gesture = Gesture::None;
	float pressX = 0, pressY = 0;       // dock-local stage px
	bool dragStarted = false;           // a key drag passed its 3 px
	bool narrow = false;                // a click on a selected key without a drag selects only it
	std::vector<std::pair<int, int>> narrowTo;
	double leadTick = 0, lastDt = 0;
	int leadCh = -1;                    // a key dragged in GRAPH: its channel's scale moves the values
	double lastDv = 0;
	bool shiftLock = false;             // Shift holds the value where it was: timing only
	double lockDv = 0;
	int handleCh = -1, handleId = 0, handleSide = 0; // the GRAPH handle being dragged
	double rangeLo[7] = {}, rangeHi[7] = {};          // GRAPH: each curve's value range, kept during a gesture
	bool rangeSet[7] = {};
	int overviewPart = 0;               // 1 the left edge, 2 the right edge, 3 the box
	double grabT0 = 0, grabT1 = 0;      // the window when an overview or scrollbar drag began
	float grabScroll = 0;
	double lastKeyPressTime = -1, lastKeyPressTick = 0; // a double-click goes to the key
	std::vector<std::pair<int, int>> knownKeys; // to reveal new keys made off screen
	bool knownValid = false;

	SpeedDialState speed;
	unsigned tipId = 0;
	double tipSince = 0;

	// A right-click menu: what it is about, where it opened, and the submenu open at each level.
	struct Menu {
		bool open = false;
		bool keyMenu = false;            // a key's menu, else the row's
		int node = -1;
		double tick = 0;                 // the key's time
		std::vector<std::pair<int, int>> members; // the key's channels and ids
		float x = 0, y = 0;              // dock-local stage px
		std::vector<int> path;           // the item whose submenu is open, per level
	} menu;

	// A value cell: dragged it scrubs, clicked it opens for typing.
	int valueNode = -1, valueCh = -1;
	double valueStart = 0, valueLast = 0;
	float valuePressX = 0;               // client px: the drag threshold and the scrub run in screen pixels
	bool valueScrub = false;
	bool editing = false;                // the typing field is open
	bool editFocus = false;              // focus it on the next frame
	int editNode = -1, editCh = -1;
	char editText[32] = {};
};

// Draws the dock into the dock rect (client px) at the stage scale and returns what the user did.
std::vector<TimelineDockAction> DrawTimelineDock(TimelineDockState& state, const TimelineDockModel& model,
	const FontSet& fonts, const Rect& dock, float scale);

// Esc closes the dock's menu, else its SNAP menu, else cancels typing, rather than closing the
// workspace. True when it did.
bool TimelineDockEscape(TimelineDockState& state);

// The ribbon's note on the TIMELINE tab: how many layers and keys it holds.
std::string TimelineDockRibbonMeta(const TimelineDockModel& model);

} // namespace Ui
} // namespace SourceFilmmaker
