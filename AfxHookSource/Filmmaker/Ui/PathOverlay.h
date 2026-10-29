#pragma once

// The camera path in the world, CS2's K-path markers (CampathDrawer.cpp, marker style) in CS:S:
//
//   * the trajectory: a dark underline, then the line in the path's Live gold or Freeze blue;
//   * each marker as a small camera: its solid body and glass lens shaded by the viewer's key
//     light, its edges over a dark underline, the aimed-at one whitened, its index beside it;
//   * the playhead: where the path puts the camera at the demo's tick, a dot on the line;
//   * the follow camera's aids: its placed Lock-on camera as a camera marker in the follow colour,
//     and a cyan glass box around the target it was just given;
//   * the weapon pivots' markers: where Weapon Recoil and Weapon Bob hinge the gun, a wireframe
//     sphere in each owner's colour over everything, as CS2's drawer draws it with depth off (the
//     viewmodel would bury a depth-tested one).
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

// A weapon pivot's marker (CS2's RecoilPivotMarker): where it is and how big, already slid out past
// the near plane (RecoilPivotMarkerMath.h), and its owner's colour (WeaponPivotOwnerColour).
struct PathOverlayPivot {
	double pos[3] = { 0, 0, 0 };
	double radius = 0;
	unsigned char rgb[3] = { 255, 255, 255 };
};

struct PathOverlayModel {
	std::vector<PathOverlayMarker> markers;
	std::vector<double> trajectory; // x, y, z along the path; empty below two markers
	int aimed = -1;                 // drawn white
	bool freeze = false;            // Freeze blue, else Live gold
	bool head = false;              // the playhead is inside the path's ticks
	double headPos[3] = { 0, 0, 0 };
	// The follow camera's (CS2's FollowCameraMarker and FollowTargetBox).
	bool followCamera = false;
	PathOverlayMarker followMarker;
	bool targetBox = false;
	double boxPos[3] = { 0, 0, 0 };  // the middle of its floor
	double boxHalf = 0, boxHeight = 0;
	std::vector<PathOverlayPivot> pivots;
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
