#pragma once

#include <cmath>

namespace Filmmaker {

// Strafe lean: the view rolls INTO the direction the player is moving, as in Quake's
// V_CalcRoll. Moving left leans left, moving right leans right.
//
// The sign is the whole reason this is a named function with tests rather than an expression
// inline, and it has been wrong twice, so the convention is pinned here from a measurement:
// POSITIVE ROLL LEANS THE VIEW RIGHT (clockwise; the world rotates counterclockwise). Checked in
// game 2026-09-30 by setting the free camera to roll +10 and comparing against roll 0. It matches
// Source's AngleVectors, where a positive roll drops the right vector. `lateralSpeed` is
// velocity . Right, positive moving right, so the lean into the strafe is a target with the SAME
// sign as the lateral speed. Until 2026-09-30 the target was negated on the belief that "CS2 roll
// is positive toward the left lean", which leaned the view AWAY from the strafe; the
// 2026-09-19 change that introduced the negation was a real fix of a different problem (the mod's
// own leading minus had been dropped) but it rested on the same wrong belief about the roll axis.
// The user had been working around it with Reverse Tilt switched on.
//
// Forward and backward motion contribute exactly zero by construction, because the caller has
// already projected velocity onto Right. Diagonal movement therefore leans by its lateral
// component alone: holding forward+left leans less than left alone, which is the Quake feel.

// Lateral speed, units/sec, at which the lean reaches full strength.
inline constexpr double kSmoothViewTiltFullSpeed = 250.0;

// NOMINALLY degrees, but this reproduces the source mod's radians-as-degrees arithmetic: it
// scales by pi/180 and the result is then added to a roll measured in DEGREES, never converted
// back. So a factor of 40 yields about 1.40 degrees of lean. Converting it "correctly" made the
// lean ~29x stronger than the mod and threw the view around. Matching the mod's feel means
// keeping its arithmetic, so the factor keeps the mod's convention.
inline constexpr double kSmoothViewTiltFactorScale = 3.14159265358979323846 / 180.0 * 2.0;

// `lateralSpeed` is velocity . Right (units/sec), `factor` the user's tilt slider, `invert`
// (the Reverse Tilt option) for a lean AWAY from the strafe. Returns the roll target in the same
// units the caller adds to its roll.
inline double SmoothViewTiltTarget(double lateralSpeed, double factor, bool invert) {
	double n = lateralSpeed / kSmoothViewTiltFullSpeed;
	if (n > 1.0) n = 1.0;
	if (n < -1.0) n = -1.0;
	// Same sign as the lateral speed = positive roll = lean right when moving right. See the note
	// above before touching this sign.
	return n * factor * kSmoothViewTiltFactorScale * (invert ? -1.0 : 1.0);
}

} // namespace Filmmaker
