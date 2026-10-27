#pragma once

// The Config workspace's inspector: CS2's CONFIG panel (ConfigHudJs.h and its CAMERA tab,
// ConfigHudCameraTabJs.h) in CS:S paint. A rail of cards under group headings beside the selected
// card, and a pinned footer. Each card is CS2's: a header (edge, title, summary, Reset, the master
// switch) over one page of rows split by group headings. A slider is dragged relative to where it
// was across one bar width (Shift a fifth as fast) and never typed; a switch row flips on a click.
//
// Engine-free: the host fills a ConfigModel each frame from the camera modules (the same fields
// CS2's ConfigHud state JSON carries) and runs the console commands the inspector returns, as CS2's
// panel sends them. The game host and the browser mockup both draw it.

#include "InspectorCards.h"
#include "UiStyle.h"

#include <string>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

// The CAMERA tab's cards, in rail order.
enum class ConfigCard { SmoothView = 0, Handheld, WeaponBob, Count };

// What the cards show: CS2's state JSON fields, named the same. The defaults are the modules'.
struct ConfigModel {
	bool demo = false;
	// SMOOTH VIEW
	bool svOn = false;
	bool svFalling = true, svJumping = true, svHealthSway = true, svTiltEnabled = true, svTiltInv = false;
	double svBreathX = 1, svBreathY = 1, svStandAmp = 0.2, svWalkAmp = 0.4, svRunAmp = 0.8, svTilt = 40, svTiltSpeed = 5;
	bool smoothViewDefault = true;
	// HANDHELD CAMERA
	bool handheldOn = false;
	bool hhVmDecoupled = false, hhCenterLockOn = false;
	double hhCenterLock = 100, hhDrift = 0, hhDriftDir = 0, hhTilt = 100, hhVariance = 40, hhCurve = 0, hhSpeed = 100;
	double hhRegrip = 100, hhPeriod = 10;
	std::string hhIdle = "INVESTIGATION", hhWalk = "WALK_TO_THE_STORE", hhRun = "HANDYCAM_RUN";
	bool handheldDefault = true;
	// The game draws the weapon through a channel the shake layers can move (CS2's viewmodel hook).
	// Without it, Decouple Viewmodel has nothing to decouple.
	bool viewmodelChannel = false;
	// WEAPON BOB
	bool wbOn = false;
	double wbAmount = 100, wbSway = 100, wbTilt = 100, wbRate = 100, wbStep = 60;
	double wbLook = 100, wbInertia = 100, wbIdle = 100, wbCam = 12;
	bool weaponBobDefault = true;
};

// What the inspector keeps between frames.
struct ConfigState {
	ConfigCard card = ConfigCard::SmoothView;
	int openDrop = -1;      // the Handheld curve list that is open: 0 Standing, 1 Walking, 2 Running
	Rect dropField;         // its field, client px
	std::string lastSent;   // the slider command a drag sent last
	// The tooltip: the row under the pointer, its text, and since when (CS2 waits 0.18 s).
	std::string tipText;
	Rect tipAnchor;
	double tipSince = -1;
};

// Draws the CAMERA tab into body (client px: under the tab strip, to the column's bottom) at the
// stage scale: the rail, the selected card and the pinned footer. Appends the console commands the
// user's edits send, as CS2's panel sends them.
void DrawConfigInspector(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, const Rect& body,
	std::vector<std::string>& commands);

// The open curve list and the tooltip, over everything: draw after the panels.
void DrawConfigOverlays(ConfigState& state, const ConfigModel& model, const FontSet& fonts, float s, float clientW,
	float clientH, std::vector<std::string>& commands);

// True while a list is open: the preview takes no clicks until it closes.
bool ConfigModal(const ConfigState& state);

// Esc closes an open list rather than the workspace. True when it did.
bool ConfigEscape(ConfigState& state);

} // namespace Ui
} // namespace SourceFilmmaker
