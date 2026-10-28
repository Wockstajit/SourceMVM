#pragma once

// The CS:S demo bar: CS2's demo bar laid out in CS:S paint, drawn by the hook because CS:S has none
// of its own. Scrubber with round cells and kill, death and bomb markers above it; play/pause, time
// and tick, -5s/+5s, round stepping and the spectated player on the left; CS2's speed dial in the
// middle; next kill, the camera mode, LOOP and CAM EDITOR on the right.
//
// Engine-free: the host fills a DemoBarModel each frame and applies the actions the bar returns.
// The game host and the browser mockup both call DrawDemoBar inside an ImGui window.

#include "DemoBarMath.h"
#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct DemoBarKill {
	int tick = 0;
	Color color = Theme::Accent; // the victim's team colour, the accent when unknown
	bool headshot = false;
	std::string text;            // "Josh killed Xander (usp, headshot)"
};

struct DemoBarRound {
	int start = 0, end = 0, number = 0;
	Color winner = Rgba(0, 0, 0, 0); // the winning team's colour; transparent while undecided
	int endTick = -1;
};

struct DemoBarBomb {
	int tick = 0;
	const char* label = "C4";
};

struct DemoBarModel {
	bool demo = false; // a demo is playing
	bool paused = false;
	int tick = 0;
	int lastTick = 0;
	double intervalPerTick = 0;
	double timescale = 1;
	std::string observed;   // the spectated player's name
	std::string cameraMode; // "FIRST PERSON", "CHASE CAM", "FREE CAM", ...
	std::vector<DemoBarKill> kills;     // in tick order
	std::vector<DemoBarRound> rounds;
	std::vector<int> roundStarts;
	std::vector<DemoBarBomb> bombs;
	std::string markerNote; // shown in the marker band when there are no markers ("reading demo...")
	bool loopOn = false;
	DemoBarMath::LoopRegion loop;
	// While the Render workspace is open the flags are its range instead of LOOP's (CS2's render-range
	// overlay): In and the last frame it renders. Until a range is set they wait at the bar's ends.
	bool renderOpen = false;
	bool renderRangeSet = false;
	DemoBarMath::LoopRegion renderRange;
	// The workspace open over the bar: none shows CAM EDITOR and CONFIG; inside one, a single button
	// swaps to the other (CS2's ModeSwapBtn).
	enum class Workspace { None, Editor, Config };
	Workspace workspace = Workspace::None;
};

struct DemoBarAction {
	// Scrub: the scrubber is dragged to tick (value 0), or let go there (value 1), and the world follows
	// it live (CS2's DemoScrub). Seek: a button's jump (-5S, +5S, a round, a kill).
	enum class Kind { TogglePause, Seek, Scrub, Speed, ToggleLoop, LoopRange, RenderRange, CameraEditor, Config, NextCamera };
	Kind kind = Kind::TogglePause;
	int tick = 0;      // Seek, Scrub; LoopRange's and RenderRange's In
	double value = 0;  // Speed; Scrub's release; LoopRange's Out; RenderRange's last frame
};

// Where each control was drawn last frame, client px. Hosts report it (mirv_filmmaker editor state,
// the mockup's state) so test scripts click controls by name.
struct DemoBarLayout {
	Rect bar, band, play, back, forward, roundPrev, roundNext, nextKill, camera, loop, camEditor, config, dial;
};

// CS2's speed dial, which the demo bar and the TIMELINE dock's toolbar share: logarithmic 0.05x to
// 20x with the middle exactly 1x; a click in one of five zones gives 0.1x, 0.25x, 1x, 4x or 10x, a
// drag anything in between.
struct SpeedDialState {
	bool active = false;
	bool dragged = false;
	float pressX = 0;
	double value = 0.5; // 0..1 along the strip
	double lastSent = 0;
};

// Draws the dial into r (client px) at the stage scale. True when the demo's speed should become
// speed this frame.
bool DrawSpeedDial(ImDrawList* list, const FontSet& fonts, float scale, SpeedDialState& state, double timescale,
	const Rect& r, const char* id, double& speed);

// What the bar keeps between frames: a drag on the scrubber or the dial, and the last layout.
struct DemoBarUi {
	bool scrubbing = false;
	float scrubPressX = 0;
	bool scrubDragged = false; // moved past a click: the world follows the drag
	int scrubTick = 0;
	SpeedDialState dial;
	int loopFlag = -1;          // the In/Out flag being dragged (LOOP's, or Render's): 0 In, 1 Out
	double loopFrac[2] = { 0, 0 };
	double loopAnchorFrac = 0;
	float loopAnchorX = 0;
	bool loopAnchorFine = false;
	DemoBarLayout layout;
};

// "band=(x0 y0 x1 y1) play=(...) ...", for the hosts' state reports.
std::string DemoBarLayoutText(const DemoBarLayout& layout);

// Draws the bar into rect (client px) at the given stage scale and returns what the user did.
std::vector<DemoBarAction> DrawDemoBar(DemoBarUi& ui, const DemoBarModel& model, const FontSet& fonts,
	const Rect& rect, float scale);

} // namespace Ui
} // namespace SourceFilmmaker
