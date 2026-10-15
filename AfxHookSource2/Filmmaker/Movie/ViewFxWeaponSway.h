#pragma once

// Weapon sway: movement-scaled walk drift added to the viewmodel offset.
//
// Runs its own free-running WALL-CLOCK phase (so it behaves the same whether the demo is playing
// or paused) driven by the stack's last tracked planar speed. ALL of it is movement-gated -- a
// stationary player's weapon sits perfectly still.
//
// This class is plumbing only: the knob, the state, and the call. Every line of arithmetic --
// including the amplitude smoothing and the adaptive staleness gate that between them fix the
// intermittent jitter -- lives in the engine-free ViewFxWeaponSwayMath.h, which is unit tested by
// Automation/tests/viewfx-weapon-sway-math-tests.cpp.

#include "ViewFxTypes.h"
#include "ViewFxWeaponSwayMath.h"

namespace Filmmaker {

class ViewFxWeaponSway {
public:
	void SetIntensity(double percent) { m_intensity = ViewFxClampIntensity(percent); }
	double Intensity() const { return m_intensity; }
	bool Enabled() const { return m_intensity > 0.0; }

	void Reset() { m_state = ViewFxWeaponSwayState(); }

	// `speedSerial` is bumped by the stack every time the speed estimate is recomputed. It is the
	// staleness signal: when the tracker stops stepping (paused demo, or another camera system
	// owns the pose) the last speed FREEZES, and without this the wall-clock drift would keep
	// marching on a frozen demo -- live-confirmed as "weapon sway continues when they're still",
	// speed stuck at 28. The drift fades out shortly after updates stop and fades back in when
	// they return; how long "shortly" is now scales with the stream's own update cadence, so
	// slow-motion playback does not read as a stopped stream (see ViewFxWeaponSwayMath.h).
	ViewFxVmOffset Offset(float nowSeconds, double speed, unsigned long long speedSerial);

	// Diagnostics for "viewfx state": the smoothed fraction actually applied, what it is chasing,
	// and whether the speed stream has gone stale. A sway that looks wrong is nearly always one
	// of these three disagreeing with what the player is doing on screen.
	double AppliedAmplitude() const { return m_state.amplitude; }
	double TargetAmplitude() const { return m_lastTarget; }
	bool Stale() const { return m_lastStale; }

private:
	double m_intensity = 0.0; // percent, 0-150
	ViewFxWeaponSwayState m_state;
	double m_lastTarget = 0.0;
	bool m_lastStale = false;
};

} // namespace Filmmaker
