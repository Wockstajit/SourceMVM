#pragma once

// CS2's camera-marker popups (MarkerHudJs.h) in CS:S paint, in the F8 card's language (HudCard.h):
//
//   * the CAMERA MARKER card, centred, while F has opened it on an aimed marker: the previous and
//     next arrows around CAMERA MARKER #n . N TOTAL; PLAYBACK (speed mode, path curve, timing,
//     auto-snap as one-click selectors, the constant or segment speed as steppers, a line on what
//     the mode does); ACTIONS (Reposition Camera, Remove Marker, Remove All Markers, which asks for
//     a second click); CONTROLS; and F / ESC Done. A catcher behind it takes every click that
//     misses it, so the spectated player cannot change under the card.
//   * the prompt banner, click-through, in the game's rect (the window, or the editor's preview):
//     the path's notices, and REPOSITIONING CAMERA #n while a camera is being moved.
//
// Engine-free: the game host and the browser mockup both draw with it inside an ImGui frame.

#include "UiStyle.h"

#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct MarkerCardModel {
	int selected = -1;
	int count = 0;
	int speedMode = 0;     // 0 Manual, 1 Constant, 2 Per-segment
	int interp = 0;        // 0 Linear, 1 Smooth
	int timing = 0;        // 0 Live, 1 Freeze
	bool autoSnap = false;
	float constSpeed = 1.0f;
	float speedMul = 1.0f; // the selected marker's outgoing segment
	bool isLast = false;   // the selected marker ends the path: it has no outgoing segment
};

struct MarkerCardAction {
	enum class Kind {
		Previous,
		Next,
		CycleSpeedMode,
		CycleInterp,
		ToggleTiming,
		ToggleAutoSnap,
		ConstSpeed,   // value
		SpeedMul,     // value, the selected marker's
		Reposition,
		Remove,
		RemoveAll,
		Close,
	};
	Kind kind = Kind::Close;
	float value = 0;
};

struct MarkerCardState {
	bool confirmRemoveAll = false; // Remove All was clicked once
	float height = 0;              // last frame's card height, to centre it
};

// The card over a full-window catcher. Call between ImGui::NewFrame and ImGui::Render, after the
// workspace or director, so it is on top.
void DrawMarkerCard(MarkerCardState& state, const MarkerCardModel& model, const FontSet& fonts, float clientW, float clientH,
	std::vector<MarkerCardAction>& actions);

struct PathBanner {
	const char* title = "";
	const char* line = "";
	const char* keys = "";
};

// The banner centred 11% down area (client px), scaled with area as CS2 scales it with the preview.
void DrawPathBanner(ImDrawList* draw, const PathBanner& banner, const FontSet& fonts, const Rect& area);

} // namespace Ui
} // namespace SourceFilmmaker
