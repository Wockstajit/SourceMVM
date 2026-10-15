#pragma once

// One mouse-wheel notch of the free camera's speed and FOV (CameraBridge_AdjustFreeCamSpeed/Fov).
// Engine-free; tested by Automation/tests/free-cam-step-math-tests.cpp.
//
// Speed used to multiply by a fixed basis per notch, which read as odd jumps (x1.22 ... ) that
// never landed on a round number. It now walks in even 0.5x steps. Below 0.5x it halves instead,
// down to 0.125x, so a slow cinematic push is still reachable.

#include <cmath>

namespace Filmmaker {

constexpr double kFreeCamSpeedStep = 0.5;
constexpr double kFreeCamSpeedMin = 0.125;
constexpr double kFreeCamSpeedMax = 256.0;
constexpr double kFreeCamFovStepDeg = 1.0;

inline double NextFreeCamSpeed(double current, int dir) {
	if (dir == 0) return current;
	if (dir > 0) {
		if (current < kFreeCamSpeedStep - 1e-9)
			return std::fmin(current * 2.0, kFreeCamSpeedStep);
		// Snap onto the 0.5 grid first, so a speed from anywhere (a console value) lands on it.
		const double next = std::floor(current / kFreeCamSpeedStep + 1e-9) * kFreeCamSpeedStep + kFreeCamSpeedStep;
		return std::fmin(next, kFreeCamSpeedMax);
	}
	if (current <= kFreeCamSpeedStep + 1e-9)
		return std::fmax(current * 0.5, kFreeCamSpeedMin);
	const double next = std::ceil(current / kFreeCamSpeedStep - 1e-9) * kFreeCamSpeedStep - kFreeCamSpeedStep;
	return std::fmax(next, kFreeCamSpeedStep);
}

} // namespace Filmmaker
