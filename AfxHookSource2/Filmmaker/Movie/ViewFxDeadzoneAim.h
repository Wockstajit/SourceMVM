#pragma once

// Deadzone aim: a decoupled viewmodel. The recorded aim moves the WEAPON first inside a deadzone
// cone while the CAMERA catches up, so the weapon reads as leading the turn.
//
// Modeled on ARC9's sh_freeaim.lua (github.com/necoarctic/ARC-9):
//   * each aim delta is only PARTIALLY absorbed into the free-aim offset (ARC9 uses diff * 0.25)
//     -- this damping is what keeps the effect from feeling hair-trigger;
//   * the offset is clamped per-axis AND by 2D magnitude to a radius;
//   * the offset is HELD, never chased back to centre mid-swipe (ARC9 has no auto-recenter at
//     all); we add a slow at-rest recenter so a demo camera does not stay miscentered for
//     minutes, gated on the aim being quiet for a while first.
//
// The camera shows (true aim - freeAim); the viewmodel is nudged toward the true aim.
//
// An earlier exp-chase design recentered between every swipe -- the weapon bounced back to the
// middle mid-turn -- and transferred 100% of small deltas, which felt hair-trigger at any
// intensity (a small radius just meant "instantly pinned + sign-flipping"). Do not reintroduce it.

#include "ViewFxTypes.h"

namespace Filmmaker {

class ViewFxDeadzoneAim {
public:
	void SetIntensity(double percent) { m_intensity = ViewFxClampIntensity(percent); }
	double Intensity() const { return m_intensity; }
	bool Enabled() const { return m_intensity > 0.0; }

	// When on, the free-aim only moves the GUN. The camera and crosshair stay locked to where
	// the player is actually aiming, so the crosshair sits on the real bullet impact point
	// instead of drifting away.
	void SetLockCrosshair(bool on) { m_lockCrosshair = on; }
	bool LockCrosshair() const { return m_lockCrosshair; }

	void Reset() {
		m_freeAimPitch = m_freeAimYaw = 0.0;
		m_quietTime = 0.0;
		m_camDeltaPitch = m_camDeltaYaw = 0.0;
	}

	// Runs on every accepted tick step, even when the player is stationary -- you can stand still
	// and flick.
	void Advance(const ViewFxInput& in);

	// Applied camera lag = -freeAim, degrees. Zero while Lock Crosshair is on.
	double CamDeltaPitch() const { return m_camDeltaPitch; }
	double CamDeltaYaw() const { return m_camDeltaYaw; }

	// Where the TRUE aim sits relative to the camera. Published to the ViewFxVm rotation
	// experiment, which must rotate the weapon BY +freeAim to point at the true aim.
	double FreeAimPitch() const { return m_freeAimPitch; }
	double FreeAimYaw() const { return m_freeAimYaw; }

	// Pushes the viewmodel toward where the aim actually is, so the weapon reads as having moved
	// first. The lag is already clamped to the cone, so this is bounded (~0.5 units at 100%).
	ViewFxVmOffset VmOffset() const;

private:
	double m_intensity = 0.0;         // percent, 0-150
	bool m_lockCrosshair = false;
	double m_freeAimPitch = 0.0, m_freeAimYaw = 0.0;
	double m_quietTime = 0.0;         // seconds of near-zero aim deltas (recenter gate)
	double m_camDeltaPitch = 0.0, m_camDeltaYaw = 0.0;
};

} // namespace Filmmaker
