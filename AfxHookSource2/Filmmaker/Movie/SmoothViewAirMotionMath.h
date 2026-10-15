#pragma once

#include <cmath>

namespace Filmmaker {

struct SmoothViewAirMotionState {
	bool initialized = false;
	double pitch = 0.0;
	// Jump kick: a sprung jolt started by a take-off or a landing, in degrees and degrees/second.
	bool wasOnGround = true;
	double airVz = 0.0; // the last vertical speed while airborne: how hard the landing is
	double kick = 0.0, kickVel = 0.0;
};

struct SmoothViewAirMotionResult {
	double targetPitch = 0.0;
	double pitch = 0.0; // the smoothed air pitch plus the kick
	double kick = 0.0;
};

inline double SmoothViewAirClamp(double value, double low, double high) {
	return value < low ? low : value > high ? high : value;
}

// Jump kick impulses, at intensity 1. Positive pitch looks down (Source). A take-off jolts the view
// up a few degrees; a landing dips it down by how fast the player was falling (full at 600 u/s).
constexpr double kSmoothViewTakeoffKick = -90.0;  // deg/s
constexpr double kSmoothViewLandingKick = 170.0;  // deg/s at a full-speed landing
constexpr double kSmoothViewKickOmega = 12.0;     // rad/s: how fast the jolt swings back
constexpr double kSmoothViewKickDamping = 0.55;   // fraction of critical: one small overshoot

// The demo supplies vertical velocity and ground state at demo-tick cadence. Convert them to a
// held target, then approach that target on the render clock. Applying the raw target directly
// makes every velocity step and ground-state edge a visible pitch step.
//
// FALL PITCH tilts the view down while falling (up to 10 degrees x intensity at 500 u/s).
// JUMP KICK tilts it up while rising, and on top of that kicks on the two moments the name says:
// a jolt up at take-off and a dip down on landing that grows with the landing speed. The kick
// was missing before -- the switch only ever produced the slow rise, a few degrees on a normal
// jump, so it looked like it did nothing. Ground changes are detected once, on the sample that
// changes; the spring advances on the render clock like the pitch.
inline SmoothViewAirMotionResult SmoothViewUpdateAirMotion(
	SmoothViewAirMotionState& state,
	double verticalVelocity, bool onGround,
	bool fallingEnabled, double fallingIntensity,
	bool jumpingEnabled, double jumpingIntensity,
	double renderDt, bool advance) {
	SmoothViewAirMotionResult result;
	if (!onGround && verticalVelocity < 0.0 && fallingEnabled) {
		result.targetPitch = 10.0
			* SmoothViewAirClamp(std::fabs(verticalVelocity) / 500.0, 0.0, 1.0)
			* SmoothViewAirClamp(fallingIntensity, 0.0, 2.0);
	} else if (!onGround && verticalVelocity > 0.0 && jumpingEnabled) {
		result.targetPitch = -10.0
			* SmoothViewAirClamp(verticalVelocity / 500.0, 0.0, 1.0)
			* SmoothViewAirClamp(jumpingIntensity, 0.0, 2.0);
	}

	if (!state.initialized) {
		// Starting mid-air (a seek) is not a take-off.
		state.initialized = true;
		state.pitch = 0.0;
		state.wasOnGround = onGround;
	}
	const double kickScale = jumpingEnabled ? SmoothViewAirClamp(jumpingIntensity, 0.0, 2.0) : 0.0;
	if (state.wasOnGround && !onGround && verticalVelocity > 50.0) {
		state.kickVel += kSmoothViewTakeoffKick * kickScale;
	} else if (!state.wasOnGround && onGround) {
		const double impact = SmoothViewAirClamp(-state.airVz / 600.0, 0.0, 1.0);
		state.kickVel += kSmoothViewLandingKick * impact * kickScale;
	}
	if (!onGround) state.airVz = verticalVelocity;
	state.wasOnGround = onGround;
	if (!jumpingEnabled) state.kick = state.kickVel = 0.0;

	if (advance && std::isfinite(renderDt) && renderDt > 0.0 && renderDt <= 0.5) {
		constexpr double kResponsePerSecond = 14.0;
		const double alpha = 1.0 - std::exp(-renderDt * kResponsePerSecond);
		state.pitch += (result.targetPitch - state.pitch) * alpha;
		if (std::fabs(result.targetPitch - state.pitch) < 0.0001) state.pitch = result.targetPitch;
		// The kick's spring, in small steps so a slow frame cannot blow it up.
		const int steps = int(std::ceil(renderDt / (1.0 / 240.0)));
		const double h = renderDt / steps;
		for (int i = 0; i < steps; ++i) {
			const double accel = -kSmoothViewKickOmega * kSmoothViewKickOmega * state.kick
				- 2.0 * kSmoothViewKickDamping * kSmoothViewKickOmega * state.kickVel;
			state.kickVel += accel * h;
			state.kick += state.kickVel * h;
		}
		if (std::fabs(state.kick) < 1e-4 && std::fabs(state.kickVel) < 1e-3) state.kick = state.kickVel = 0.0;
	}
	result.kick = state.kick;
	result.pitch = state.pitch + state.kick;
	return result;
}

} // namespace Filmmaker
