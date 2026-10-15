#pragma once

// Weapon float: the gun chases the aim direction instead of being welded to it, so turning
// swings the weapon and it settles afterwards. This is the term that makes a weapon feel like it
// has mass. (View Dynamics "Viewmodel Float Tightness".)
//
// Driven purely by AIM movement, unlike ViewFxWeaponSway which is a movement-gated walk-cycle
// sine and does nothing at all when you only turn.
//
// LIMITATION: the addon floats the gun's ROTATION (`lerped_dir:Angle()`), which CS2's viewmodel
// detour cannot write -- it carries offset X/Y/Z + FOV only. This is the translation equivalent:
// the weapon is pushed opposite the turn by the lag, which reads as the same swing-and-settle.
// A true rotational port needs the ViewFxVm path.

#include "ViewFxTypes.h"

namespace Filmmaker {

class ViewFxWeaponFloat {
public:
	// Tightness is the rate constant of `1 - exp(-k*dt)`: LOW = very floaty/laggy, HIGH = locked
	// to the aim. 0 disables. The addon's slider is 0-40 with a default of 20.
	void SetTightness(double k) { m_tightness = ViewFxClamp(k, 0.0, 40.0); }
	void SetAmount(double units) { m_amount = ViewFxClamp(units, 0.0, 3.0); }
	double Tightness() const { return m_tightness; }
	double Amount() const { return m_amount; }
	bool Enabled() const { return m_tightness > 0.0 && m_amount > 0.0; }

	void Reset() { m_lagYaw = m_lagPitch = 0.0; }

	void Advance(const ViewFxInput& in);

	// Yaw lag pushes the gun sideways (opposite the turn, so it trails); pitch lag pushes it
	// vertically. NOT movement-gated -- turning on the spot is exactly when it should be visible.
	ViewFxVmOffset VmOffset() const;

private:
	double m_tightness = 0.0; // rate constant; 0 = off. Addon default 20.
	double m_amount = 1.0;    // units of viewmodel offset per degree of accumulated lag
	double m_lagYaw = 0.0;    // smoothed aim lag, degrees
	double m_lagPitch = 0.0;
};

} // namespace Filmmaker
