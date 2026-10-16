#pragma once

// Where the mouse belongs inside a workspace (Camera Editor, demo Config), decided by where you
// click -- there is no mouse toggle:
//   * click the game preview  -> GAME: the click is the camera's (CS2's look-around drag, or our
//                                free cam), and our panels stop catching clicks;
//   * click anywhere else     -> UI: the click goes to the panel.
// The wheel follows the same split: over the preview it steps CS2's camera (first person / chase /
// free cam), over a panel it scrolls that panel. Outside a workspace the whole window is the game.
//
// In game mode the arrow stays free and visible between looks, so the click position is where the
// user aimed. While a look is in progress the free cam pins a hidden cursor to the window centre --
// inside the preview -- so such a click answers "game" by position alone.
//
// Engine-free so Automation/tests/ui-click-math-tests.cpp can pin it; MovieMode gathers the inputs.
// An unknown preview rect answers "leave it as it is": a click or notch that cannot be placed
// never flips the mode.

namespace Filmmaker {
namespace UiClick {

struct Rect {
	bool valid = false;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // client px, x1/y1 exclusive
	bool Contains(float x, float y) const { return valid && x >= x0 && x < x1 && y >= y0 && y < y1; }
};

struct Input {
	bool uiModeOn = false;  // the UI has the mouse right now
	bool workspace = false; // Camera Editor or demo Config is open
	Rect preview;           // the workspace's game preview
	bool popupOpen = false; // a popup that covers the window (Editor Settings) is up
	float x = 0, y = 0;     // the click / wheel position, client px
};

// GAME mode, a click on a panel: hand the mouse to the UI and give the panel the click. With a
// popup up, every click is the popup's, even one over the preview it sits on.
inline bool ClickSwitchesToUiMode(const Input& in) {
	if (!in.workspace || in.uiModeOn) return false;
	if (in.popupOpen) return true;
	return in.preview.valid && !in.preview.Contains(in.x, in.y);
}

// UI mode, a click in the preview: hand the mouse to the camera; the click carries on to the game.
inline bool ClickSwitchesToGameMode(const Input& in) {
	return in.workspace && in.uiModeOn && !in.popupOpen && in.preview.Contains(in.x, in.y);
}

// While a camera path PLAYS (or is about to), the path flies the camera. A press in the preview
// must neither flip the workspace to GAME mode -- free-look would fight the path for the view --
// nor reach CS2, whose right-click look-around and left-click player step both take the pointer.
// Either button, either mouse mode; panels and popups keep their clicks. The caller swallows the
// matching release too.
inline bool PathPlaybackSwallowsPress(const Input& in, bool pathPlaying) {
	return pathPlaying && in.workspace && !in.popupOpen && in.preview.Contains(in.x, in.y);
}

// Does this wheel notch step CS2's camera (true) or scroll the panel under it (false)?
inline bool WheelCyclesCamera(const Input& in) {
	if (!in.workspace) return true;
	return !in.popupOpen && in.preview.Contains(in.x, in.y);
}

// A rect given as window fractions (how the workspaces publish their preview) in client px.
inline Rect FromFractions(float fx0, float fy0, float fx1, float fy1, float clientW, float clientH) {
	Rect r;
	r.valid = clientW > 0 && clientH > 0 && fx1 > fx0 && fy1 > fy0;
	r.x0 = fx0 * clientW; r.y0 = fy0 * clientH; r.x1 = fx1 * clientW; r.y1 = fy1 * clientH;
	return r;
}

} // namespace UiClick
} // namespace Filmmaker
