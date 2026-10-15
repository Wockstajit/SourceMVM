#include "../../AfxHookSource2/Filmmaker/Movie/RecoilPivotMarkerMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-9) {
	return std::fabs(a - b) <= epsilon;
}

// A perspective projection divides by forward distance, so this is the marker's screen position
// (in units of "per unit of depth") and its angular size. Both must be invariant under the
// near-plane escape -- that is the entire justification for moving the marker at all.
struct Screen { double x, y, angularSize; };
Screen Project(const RecoilPivotDrawPoint& p) {
	return Screen{ p.right / p.fwd, p.up / p.fwd, p.radius / p.fwd };
}
} // namespace

int main() {
	const double r = kRecoilPivotMarkerRadius;

	// ---- a hinge already clear of the near plane is left exactly alone ----
	{
		const auto p = RecoilPivotPlaceMarker(30.0, 3.0, -4.0, r);
		Check(p.visible, "a distant hinge is drawable");
		Check(Near(p.scale, 1.0), "a hinge past the minimum distance is not moved");
		Check(Near(p.fwd, 30.0) && Near(p.right, 3.0) && Near(p.up, -4.0) && Near(p.radius, r),
			"and its position and radius are untouched");
	}

	// ---- THE BUG: Weapon Bob's own default hinge was inside the near plane ----
	// 4.1 forward with a 0.6 radius puts the whole marker inside a plane at ~6.5, so the orb was
	// sliced in half at the shipped default and gone entirely for any smaller Forward.
	{
		const auto p = RecoilPivotPlaceMarker(4.1, 2.1, -4.0, r);
		Check(p.visible, "the default Weapon Bob hinge is drawable");
		Check(p.fwd - p.radius >= kRecoilPivotMinFrontDistance - 1e-9,
			"the nearest point of the marker clears the near plane");
		Check(p.scale > 1.0, "and it got there by being pushed out, not by being shrunk");
	}

	// ---- the escape is invisible: same pixels, same size ----
	{
		const double fwd = 4.1, right = 2.1, up = -4.0;
		const RecoilPivotDrawPoint raw{ fwd, right, up, r, true, 1.0 };
		const auto p = RecoilPivotPlaceMarker(fwd, right, up, r);
		const Screen a = Project(raw), b = Project(p);
		Check(Near(a.x, b.x, 1e-12) && Near(a.y, b.y, 1e-12),
			"the escape does not move the marker on screen");
		Check(Near(a.angularSize, b.angularSize, 1e-12),
			"the escape does not change the marker's apparent size");
	}

	// ---- dragging Forward down sweeps it OFF SCREEN instead of blinking out ----
	// The complaint was that the marker vanished mid-frame. Screen position must stay a
	// continuous, growing function of the hinge as Forward shrinks, right up to the eye.
	{
		double previous = -1.0;
		for (double fwd = 12.0; fwd > 0.8; fwd -= 0.2) {
			const auto p = RecoilPivotPlaceMarker(fwd, 2.1, -4.0, r);
			Check(p.visible, "every hinge in front of the eye stays drawable");
			const double offset = std::hypot(p.right / p.fwd, p.up / p.fwd);
			Check(offset > previous, "the marker moves steadily outward as Forward shrinks");
			previous = offset;
		}
		Check(previous > 1.0, "and ends up well outside the frame rather than disappearing in it");
	}

	// ---- a hinge at or behind the eye has no screen position, and says so ----
	{
		Check(!RecoilPivotPlaceMarker(-7.5, 2.25, -4.0, r).visible,
			"the knife hinge, which sits behind the head, is not drawable");
		Check(!RecoilPivotPlaceMarker(0.0, 2.0, -4.0, r).visible,
			"a hinge exactly at the eye is not drawable");
		Check(!RecoilPivotPlaceMarker(r * 0.5, 2.0, -4.0, r).visible,
			"a hinge close enough that the eye is inside the marker is not drawable");
	}

	// ---- degenerate inputs are rejected rather than propagated ----
	{
		Check(!RecoilPivotPlaceMarker(30.0, 3.0, -4.0, 0.0).visible, "a zero radius draws nothing");
		Check(!RecoilPivotPlaceMarker(std::nan(""), 3.0, -4.0, r).visible, "NaN draws nothing");
	}

	std::cout << "RecoilPivotMarkerMathTests: all checks passed\n";
	return 0;
}
