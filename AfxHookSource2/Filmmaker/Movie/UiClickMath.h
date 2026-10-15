#pragma once

// Click-to-UI-mode: in GAME mode (G handed the mouse to the camera) a left click on our UI switches
// to UI mode and goes to the panel, instead of being eaten as a camera click. A click on the game
// itself (the workspace preview, or the world in regular viewing) keeps doing what it did.
//
// In game mode the arrow stays free and visible between looks, so the click position is where the
// user aimed. While a look is in progress the free cam pins a hidden cursor to the window centre --
// inside the preview / the world -- so such a click answers "game" by position alone.
//
// Engine-free so Automation/tests/ui-click-math-tests.cpp can pin it; MovieMode::OnUiClick gathers
// the inputs.
//
// Where the UI is:
//   * a workspace (Camera Editor, demo Config) is open: everything OUTSIDE its preview rect;
//   * regular viewing: the native demo bar (it carries our MOUSE / CAM EDITOR / CONFIG buttons).
// An unknown rect answers "not UI": a click that cannot be placed keeps the old behaviour.

namespace Filmmaker {
namespace UiClick {

struct Rect {
	bool valid = false;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // client px, x1/y1 exclusive
	bool Contains(float x, float y) const { return valid && x >= x0 && x < x1 && y >= y0 && y < y1; }
};

struct Input {
	bool uiModeOn = false;        // the UI already has the mouse: nothing to switch
	bool uiModeAvailable = false; // G could hand the UI the mouse here (workspace, third person, free cam)
	bool workspace = false;       // Camera Editor or demo Config is open
	Rect preview;                 // the workspace's game preview
	Rect demoBar;                 // the native demo bar (regular viewing)
	float x = 0, y = 0;           // the click, client px
};

inline bool ClickSwitchesToUiMode(const Input& in) {
	if (in.uiModeOn || !in.uiModeAvailable) return false;
	if (in.workspace) return in.preview.valid && !in.preview.Contains(in.x, in.y);
	return in.demoBar.Contains(in.x, in.y);
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
