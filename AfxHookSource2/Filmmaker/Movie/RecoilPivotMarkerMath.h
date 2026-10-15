#pragma once

// Pivot marker placement -- pure math, no engine dependencies so it can be unit tested offline.
//
// THE PROBLEM: the hinge lives inside the weapon, and the weapon is closer to the eye than the
// camera's near plane. Weapon Bob's shipped hinge is 4.1 units forward; CS2's near plane is
// around 6.5. A 0.6-radius sphere centred at 4.1 is therefore ENTIRELY inside the near plane and
// the rasterizer throws it away, and a hinge only slightly further out gets sliced in half --
// which is what "the cut-off happens too early, I can't move it back further without it
// disappearing before it is off screen" describes. Every Forward value below about 7 was
// unusable, including the default.
//
// THE FIX: slide the marker OUT ALONG THE RAY FROM THE EYE until it clears the plane, and scale
// its radius by the same factor.
//
// That is not a fudge, it is exact. A perspective projection divides by depth, so scaling a point
// and its radius by a common factor about the eye leaves BOTH the screen position and the
// on-screen size completely unchanged -- the marker draws in precisely the pixels it would have
// drawn in if the near plane were not there. And because the marker is already drawn with depth
// testing off (the viewmodel is rendered over this pass, so a depth-tested marker is buried
// inside the gun it annotates), pushing it further away costs nothing: there is no occlusion to
// get wrong.
//
// The one thing it cannot rescue is a hinge at or behind the eye. A knife hinges BEHIND the head
// (the measured MW2019 anchor is -7.5 forward), and a point behind the camera has no screen
// position at all -- there is no honest place to draw it. Those report `visible = false` and the
// orb is simply not drawn. Dragging Forward down now sweeps the marker out toward the edge of the
// frame and off it, instead of blinking out mid-screen.

#include <cmath>

namespace Filmmaker {

// How far in front of the eye the NEAREST point of the marker must sit. Comfortably past CS2's
// near plane (~6.5) so no pass can clip it, and free to be generous: the scale preserves the
// marker's apparent size exactly, and depth testing is off, so a more distant marker is
// indistinguishable from a near one except that it survives.
constexpr double kRecoilPivotMinFrontDistance = 16.0;

// The marker's radius at the hinge's TRUE distance, in world units -- a small ball at arm's
// length rather than something that fills the frame. What actually reaches the drawer is this
// scaled by the near-plane escape below, so the on-screen size is the same either way.
constexpr double kRecoilPivotMarkerRadius = 0.6;

// A marker placed in CAMERA-LOCAL space: +fwd out along the view axis, +right, +up.
struct RecoilPivotDrawPoint {
	double fwd = 0.0;
	double right = 0.0;
	double up = 0.0;
	double radius = 0.0;
	bool visible = false;
	// The factor the point was pushed out by. 1 means the hinge was already clear; larger means
	// the marker is drawn further away than the hinge really is, at the same screen position and
	// the same apparent size. Diagnostics only.
	double scale = 1.0;
};

// Place a marker for a hinge at camera-local (fwd, right, up) with world radius `radius`.
inline RecoilPivotDrawPoint RecoilPivotPlaceMarker(
	double fwd, double right, double up, double radius) {
	RecoilPivotDrawPoint out;
	if (!(radius > 0.0) || !std::isfinite(fwd) || !std::isfinite(right) || !std::isfinite(up))
		return out;

	// The near plane is perpendicular to the view axis, so the nearest point of the sphere is
	// exactly `fwd - radius` regardless of how far off-axis the hinge sits.
	const double front = fwd - radius;

	// At or behind the eye: no screen position exists. Includes the whole knife family.
	if (!(front > 1e-3)) return out;

	const double scale = (front >= kRecoilPivotMinFrontDistance)
		? 1.0
		: (kRecoilPivotMinFrontDistance / front);

	out.fwd = fwd * scale;
	out.right = right * scale;
	out.up = up * scale;
	out.radius = radius * scale;
	out.scale = scale;
	out.visible = true;
	return out;
}

} // namespace Filmmaker
