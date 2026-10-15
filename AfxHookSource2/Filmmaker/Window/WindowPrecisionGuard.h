#pragma once

// Keeps Windows Snap from stealing a SHIFT (precision) resize drag.
//
// THE BUG THIS EXISTS FOR
//   Holding Shift while dragging a window edge is Source MVM's precision resize: WM_SIZING edits
//   the proposed rect so the size lands exactly on a named ratio / standard resolution
//   (WindowSnapMath.h). But if the pointer reaches a screen edge mid-drag, Windows' own Aero Snap
//   takes the drag over: the OS abandons the rect we approved, applies its snapped one (half
//   screen, vertical maximise), and WM_EXITSIZEMOVE then COMMITS that size to the engine as a
//   video-mode change. The user asked for 1920x1080 and got whatever the desktop edge decided.
//
//   Aero Snap cannot be disabled per-window, and changing the user's system Snap settings is
//   explicitly off the table (it is a global setting that outlives the game). What CAN be done is
//   refuse the change: the OS asks before it moves or resizes a window, via WM_WINDOWPOSCHANGING,
//   and a handler may edit that WINDOWPOS in place -- the same "constrain by editing the proposed
//   rect" contract WM_SIZING already uses here. A snap that is asked and answered with our own
//   rect never happens.
//
// WHY ITS OWN HEADER
//   The decision is pure arithmetic over the drag session's state, so it lives away from the
//   message pump and gets unit tests (WindowPrecisionGuardTests) that run without a window, a
//   desktop, or a Shift key -- the same split AspectRatioMath.h / WindowSnapMath.h already use.
//   WindowResize.cpp owns the messages; this owns the answer.
//
// WHAT IT DELIBERATELY DOES NOT DO
//   * Nothing while Shift is up. A plain drag stays completely stock, Aero Snap included -- snap
//     is a feature the rest of the time, and only precision mode claims the window.
//   * Nothing outside the modal drag loop, so maximise/restore, SDL, and the engine's own
//     video-mode resize are untouched.
//   * It never invents a size: it can only re-assert the rect WM_SIZING already approved.

namespace Filmmaker {

// The drag session's approved rect. WindowResize.cpp owns one of these.
struct PrecisionGuardState {
	bool active = false;      // a Shift-held resize is in progress and has approved a rect
	bool haveOrigin = false;  // the approved rect's top-left is known (a left/top edge drag moves it)
	int windowX = 0;          // approved WINDOW rect, screen coords, frame included
	int windowY = 0;
	int windowW = 0;
	int windowH = 0;
	// The same rect expressed as the CLIENT size, which is what a commit to the engine speaks in.
	int clientW = 0;
	int clientH = 0;
};

// What to do with an incoming WM_WINDOWPOSCHANGING.
struct PrecisionGuardDecision {
	bool clampSize = false;   // overwrite cx/cy with width/height below
	bool clampMove = false;   // overwrite x/y with x/y below
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// Record what WM_SIZING just approved. `shiftHeld` false disarms the guard for the rest of this
// drag step, so releasing Shift mid-drag hands the window straight back to Windows.
inline void PrecisionGuard_Approve(PrecisionGuardState& state, bool shiftHeld,
	int windowX, int windowY, int windowW, int windowH, int clientW, int clientH) {
	if (!shiftHeld || windowW <= 0 || windowH <= 0 || clientW <= 0 || clientH <= 0) {
		state.active = false;
		state.haveOrigin = false;
		return;
	}
	state.active = true;
	state.haveOrigin = true;
	state.windowX = windowX;
	state.windowY = windowY;
	state.windowW = windowW;
	state.windowH = windowH;
	state.clientW = clientW;
	state.clientH = clientH;
}

inline void PrecisionGuard_Release(PrecisionGuardState& state) {
	state.active = false;
	state.haveOrigin = false;
}

// The WM_WINDOWPOSCHANGING answer.
//
// `inModalLoop`  -- we are inside the OS drag loop (WM_ENTERSIZEMOVE seen, no WM_EXITSIZEMOVE yet)
// `shiftHeld`    -- Shift is STILL down right now, re-read at message time
// `sizeChanging` -- the WINDOWPOS carries a size (i.e. SWP_NOSIZE is clear)
// `moveChanging` -- the WINDOWPOS carries a position (i.e. SWP_NOMOVE is clear)
//
// A size that already matches the approved one is left alone: answering an unchanged rect with
// itself is noise, and it would make every ordinary drag step look like a defended snap in the log.
inline PrecisionGuardDecision PrecisionGuard_Evaluate(const PrecisionGuardState& state,
	bool inModalLoop, bool shiftHeld, bool sizeChanging, bool moveChanging,
	int proposedX, int proposedY, int proposedW, int proposedH) {
	PrecisionGuardDecision decision;
	if (!state.active || !inModalLoop || !shiftHeld)
		return decision;
	if (state.windowW <= 0 || state.windowH <= 0)
		return decision;

	if (sizeChanging && (proposedW != state.windowW || proposedH != state.windowH)) {
		decision.clampSize = true;
		decision.width = state.windowW;
		decision.height = state.windowH;
		// A snap that resizes almost always repositions too (it pins the window to the edge it
		// snapped against). Putting the size back while leaving the snapped origin would leave the
		// window parked at the screen edge, so the origin travels with it.
		if (moveChanging && state.haveOrigin && (proposedX != state.windowX || proposedY != state.windowY)) {
			decision.clampMove = true;
			decision.x = state.windowX;
			decision.y = state.windowY;
		}
	}
	return decision;
}

// After the drag: did something (a snap) leave the window at a size precision mode never approved?
// If so the finished size must NOT be what gets committed to the engine. Asked in CLIENT pixels,
// because that is what the commit and the readout card both speak in.
inline bool PrecisionGuard_FinalSizeHijacked(const PrecisionGuardState& state,
	int finalClientW, int finalClientH) {
	if (!state.active || state.clientW <= 0 || state.clientH <= 0)
		return false;
	return finalClientW != state.clientW || finalClientH != state.clientH;
}

} // namespace Filmmaker
