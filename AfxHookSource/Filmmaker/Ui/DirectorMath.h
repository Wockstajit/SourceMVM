#pragma once

// The movie director outside the workspaces, CS2's rules (AfxHookSource2/Filmmaker/Movie/MovieMode.cpp,
// Panorama/MovieHudJs.h) with CS:S's camera numbers. Engine-free and header-only, so the game host
// and the browser mockup decide the same way; Automation/tests/source-director-math-tests.cpp pins it.
//
//   wheel        cycles first person, chase and free cam; Ctrl+wheel is the FOV, Shift+wheel the
//                free cam's speed (with HLAE's own free camera, mirv_input, on: its FOV and speed)
//   Space        pause / play
//   Left, Right  5 seconds back / on
//   F8           blackout: no HUD, no bar, no cards (or, with the cards hidden, the cards back)
//
// The mouse is DirectorMouse.h.

#include "../../../AfxHookSource2/Filmmaker/Movie/FreeCamStepMath.h"

#include "DemoBarMath.h"
#include "WorkspaceLayoutMath.h"

#include <cmath>

namespace SourceFilmmaker {
namespace Ui {
namespace DirectorMath {

// Observer modes of the current Source 2013 multiplayer engine (shareddefs.h). This SDK added
// OBS_MODE_POI at 6, so the roaming camera is 7; CS:S's own spec_mode steps 4, 5, 7, 4.
constexpr int kFixed = 3;
constexpr int kInEye = 4;
constexpr int kChase = 5;
constexpr int kRoaming = 7;

// The wheel's ring: first person, chase, free cam. Up is one step on, as CS2's camera button and its
// wheel; down is one step back. Notches in one frame add up, so up and down cancel.
inline int StepCamera(int mode, int notches) {
	static const int kRing[3] = { kInEye, kChase, kRoaming };
	int at = -1;
	for (int i = 0; i < 3; ++i)
		if (kRing[i] == mode) at = i;
	if (at < 0) at = 2; // not one of the three (fixed, death cam): the next step is first person
	const int step = ((notches % 3) + 3) % 3;
	return kRing[(at + step) % 3];
}

// The bar's camera button and the card's Mode row. Mode 0 is not spectating: a player's own
// recording, seen through the player's eyes.
inline const char* CameraButtonName(int mode) {
	switch (mode) {
	case 0: return "FIRST PERSON";
	case 1: return "DEATH CAM";
	case 2: return "FREEZE CAM";
	case kFixed: return "FIXED CAM";
	case kInEye: return "FIRST PERSON";
	case kChase: return "CHASE CAM";
	case kRoaming: return "FREE CAM";
	default: return "CAMERA";
	}
}

inline const char* CameraCardName(int mode) {
	switch (mode) {
	case 0:
	case kInEye: return "First person";
	case kChase: return "Chase cam";
	case kRoaming: return "Free cam";
	case kFixed: return "Fixed cam";
	default: return "-";
	}
}

enum class Key { None, TogglePause, SkipBack, SkipForward, Blackout };

// Virtual-key codes, which the browser's DOM keyCode shares for these keys.
inline Key KeyFor(int vk) {
	switch (vk) {
	case 0x20: return Key::TogglePause; // Space
	case 0x25: return Key::SkipBack;    // Left
	case 0x27: return Key::SkipForward; // Right
	case 0x77: return Key::Blackout;    // F8
	default: return Key::None;
	}
}

// Left / Right: five seconds at the demo's tick rate, from the playhead, kept inside the demo.
constexpr double kSkipSeconds = 5;
inline int SkipTarget(int tick, int direction, double intervalPerTick, int lastTick) {
	const int ticks = DemoBarMath::TicksForSeconds(kSkipSeconds, intervalPerTick);
	int target = tick + (direction < 0 ? -ticks : ticks);
	if (target < 0) target = 0;
	if (lastTick > 0 && target > lastTick) target = lastTick;
	return target;
}

// Ctrl+wheel: one degree a notch, wheel up zooms in; 1 to 170, whole degrees.
constexpr double kFovMin = 1, kFovMax = 170;
inline double StepFov(double fov, int notches) {
	double next = std::round(fov) - notches * Filmmaker::kFreeCamFovStepDeg;
	if (next < kFovMin) next = kFovMin;
	if (next > kFovMax) next = kFovMax;
	return next;
}

// Shift+wheel in the free cam: CS2's speed ladder (FreeCamStepMath.h), one notch at a time. Speed 1x
// is the game's default free cam speed.
inline double StepSpeed(double speed, int notches) {
	for (int i = 0; i < (notches < 0 ? -notches : notches); ++i)
		speed = Filmmaker::NextFreeCamSpeed(speed, notches < 0 ? -1 : 1);
	return speed;
}

// What a wheel notch does, by modifier: Ctrl is the FOV in any camera; Shift is the speed, in the free
// cam only (elsewhere the notch is used up and does nothing, as in CS2); otherwise the camera. With
// HLAE's free camera (mirv_input) flying the view, Ctrl and Shift are its FOV and speed instead; a
// plain notch still steps the game's cameras, which ends that free camera (CS2's MovieMode).
enum class Wheel { Camera, Fov, Speed, FreeCamFov, FreeCamSpeed, Nothing };
inline Wheel WheelFor(bool ctrl, bool shift, int mode, bool ownFreeCam = false) {
	if (ownFreeCam && ctrl) return Wheel::FreeCamFov;
	if (ownFreeCam && shift) return Wheel::FreeCamSpeed;
	if (ctrl) return Wheel::Fov;
	if (shift) return mode == kRoaming ? Wheel::Speed : Wheel::Nothing;
	return Wheel::Camera;
}

// Shift held slows HLAE's free camera to a quarter (CS2's CameraBridge_SetFreeCamSlow).
constexpr double kFreeCamSlowFactor = 0.25;

// F8: with the cards hidden it brings them back (and ends a blackout); otherwise it toggles the
// blackout.
struct HudState {
	bool cards = true;
	bool blackout = false;
};
inline HudState PressF8(HudState s) {
	if (!s.cards) {
		s.cards = true;
		s.blackout = false;
	} else {
		s.blackout = !s.blackout;
	}
	return s;
}

// The cards' placement, CS2's (MovieHudJs.h), in stage px before the cards' 0.8 scale: the director
// card in the top-right corner, the controls card on the left a third of the way down (where CS2
// falls back to when it cannot find the radar; CS:S's spectator view has none), each inset the same.
constexpr float kCardScale = 0.8f;
constexpr float kDirectorCardW = 278;
constexpr float kControlsCardW = 252;
constexpr float kCardTop = 4;
constexpr float kCardInset = 20;
constexpr float kControlsCardTopShare = 0.34f;
constexpr float kCardFadeH = 52;

struct CardPlacement {
	float x = 0, y = 0, w = 0; // client px; height follows the content
};

// topReserve is the height in client px of game chrome along the top edge that the director card
// must clear (CS:S's spectator bar).
inline CardPlacement DirectorCard(float clientW, float clientH, float topReserve) {
	const float s = clientH / kStageHeight;
	CardPlacement p;
	p.w = kDirectorCardW * kCardScale * s;
	p.x = clientW - kCardInset * s - p.w;
	p.y = topReserve + kCardTop * s;
	return p;
}

inline CardPlacement ControlsCard(float clientW, float clientH) {
	(void)clientW;
	const float s = clientH / kStageHeight;
	CardPlacement p;
	p.w = kControlsCardW * kCardScale * s;
	p.x = kCardInset * s;
	p.y = std::round(clientH * kControlsCardTopShare);
	return p;
}

// The bar outside the workspaces: CS2's undocked bar, the full width along the bottom edge, as tall
// as in the Camera Editor's dock.
inline Rect BarRect(float clientW, float clientH) {
	return MakeRect(0, clientH - kDemoBarHeight * (clientH / kStageHeight), clientW, clientH);
}

// CS:S's spectator view has a bar along the top (team scores, round timer) and one along the bottom
// (the spectated player, Options, Camera Options), each 52 of its 480-tall layout units, measured on
// a 481 px tall game window. The director card stays under the top one, and the director's bar
// covers the bottom one, whose player and camera controls it replaces.
constexpr float kSpectatorBarShare = 52.0f / 480.0f;

// The panel behind the bar: the bar's own height, or the bottom spectator bar's when that is taller.
inline Rect BarPanelRect(float clientW, float clientH, float bottomReserve) {
	Rect r = BarRect(clientW, clientH);
	if (clientH - bottomReserve < r.y0) r.y0 = clientH - bottomReserve;
	return r;
}

} // namespace DirectorMath
} // namespace Ui
} // namespace SourceFilmmaker
