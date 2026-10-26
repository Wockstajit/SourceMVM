#pragma once

// The movie director's screen outside the workspaces while a demo plays, CS2's in CS:S paint:
//
//   * the demo bar along the bottom edge, full width (CS2's undocked bar, here the hook's own);
//   * the MOVIE DIRECTOR card in the top-right corner: camera mode, playback, free cam speed, and
//     the camera's position, angles, FOV, tick and time (CS2's MovieHudJs.h);
//   * the CONTROLS card on the left: the director's keys.
//
// F8 blacks it all out (DirectorMath.h PressF8); the game side also hides the game's HUD then.
// Engine-free: the game host and the browser mockup both call DrawDirector inside an ImGui frame.

#include "DemoBar.h"
#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct DirectorCardsModel {
	const char* mode = "-";     // "First person", "Chase cam", "Free cam"
	bool freeCam = false;       // HLAE's free camera flies the view: the mode in the accent colour
	bool playing = false;
	bool paused = false;
	double speed = -1;          // the free cam's speed, x; below 0 when it has none
	double pos[3] = { 0, 0, 0 };
	double ang[3] = { 0, 0, 0 };
	double fov = 0;
	bool haveTick = false;
	int tick = 0;
	double time = 0;            // seconds
};

struct DirectorState {
	DemoBarUi bar;
	bool cards = true;     // the cards are on (F8 brings them back, mirv_filmmaker hud)
	bool blackout = false; // F8: nothing of ours, nor the game's HUD
};

struct DirectorFrame {
	Rect bar; // the panel behind the demo bar, client px; empty while blacked out
	std::vector<DemoBarAction> demoActions;
};

// Draws the director for a clientW x clientH window. topReserve and bottomReserve are the client-px
// heights of game chrome along the top and bottom edges (CS:S's spectator bars, DirectorMath.h):
// the director card stays under the top one, the bar's panel covers the bottom one.
DirectorFrame DrawDirector(DirectorState& state, const DemoBarModel& bar, const DirectorCardsModel& cards,
	const FontSet& fonts, float clientW, float clientH, float topReserve, float bottomReserve);

// The director card's readout text, as drawn, for the hosts' state reports ("Mode=Chase cam ...").
std::string DirectorCardsText(const DirectorCardsModel& cards);

} // namespace Ui
} // namespace SourceFilmmaker
