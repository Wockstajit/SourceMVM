#pragma once

// The camera path in the world, CS2's K-path markers (CampathDrawer.cpp, marker style) in CS:S:
//
//   * the trajectory: a dark underline, then the line in the path's Live gold or Freeze blue;
//   * each marker as a small camera: its solid body and glass lens shaded by the viewer's key
//     light, its edges over a dark underline, the aimed-at one whitened, its index beside it;
//   * the playhead: where the path puts the camera at the demo's tick, a dot on the line.
//
// CS2 draws these in the world with its own line shader. CS:S has no such pass on x64, so they are
// projected on the CPU (DeadlockOverlayMath.h, as Deadlock's overlay does) into a draw list the
// renderer lays over the game's frame before the UI. Sizes and pixel widths are CS2's.
// Engine-free: the game host and the browser mockup both call DrawPathOverlay.

#include <imgui.h>

#include <vector>

namespace SourceFilmmaker {
namespace Ui {

struct PathOverlayMarker {
	double pos[3] = { 0, 0, 0 };
	double ang[3] = { 0, 0, 0 }; // pitch, yaw, roll
	double fov = 90;             // horizontal, widened for the screen
};

struct PathOverlayModel {
	std::vector<PathOverlayMarker> markers;
	std::vector<double> trajectory; // x, y, z along the path; empty below two markers
	int aimed = -1;                 // drawn white
	bool freeze = false;            // Freeze blue, else Live gold
	bool head = false;              // the playhead is inside the path's ticks
	double headPos[3] = { 0, 0, 0 };
};

// The view the game rendered this frame.
struct PathOverlayView {
	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 }; // pitch, yaw, roll
	double fov = 90;                // horizontal, widened for width x height
	float width = 0;
	float height = 0;
};

// Draws model into list, in pixels of the view's width x height. `digits` sets the index numbers.
void DrawPathOverlay(ImDrawList& list, const PathOverlayModel& model, const PathOverlayView& view, ImFont* digits);

} // namespace Ui
} // namespace SourceFilmmaker
