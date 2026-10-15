#include "ViewFxWeaponFloat.h"

#include <cmath>

namespace Filmmaker {

namespace {
// Viewmodel units of offset per degree of accumulated aim lag, and the cap on that lag. Tuned so
// the addon-default tightness of 20 gives a clearly visible but not silly swing on a fast flick.
constexpr double kUnitsPerDeg = 0.09;
constexpr double kMaxLagDeg = 12.0;
} // namespace

void ViewFxWeaponFloat::Advance(const ViewFxInput& in) {
	if (m_tightness <= 0.0) { m_lagYaw = m_lagPitch = 0.0; return; }
	if (!(in.dt > 0.0)) return;

	// A spectator cut is not a turn.
	double dYaw = in.dYaw, dPitch = in.dPitch;
	if (std::fabs(dYaw) > kViewFxCutDeltaDeg) dYaw = 0.0;
	if (std::fabs(dPitch) > kViewFxCutDeltaDeg) dPitch = 0.0;

	// Every degree the view turns feeds the lag, which then decays at 1 - exp(-tightness*dt).
	// Low tightness = the weapon swings wide and settles slowly.
	m_lagYaw += dYaw;
	m_lagPitch += dPitch;
	const double a = 1.0 - std::exp(-m_tightness * in.dt);
	m_lagYaw -= m_lagYaw * a;
	m_lagPitch -= m_lagPitch * a;

	// Bound it so a fast flick cannot fling the weapon off screen.
	m_lagYaw = ViewFxClamp(m_lagYaw, -kMaxLagDeg, kMaxLagDeg);
	m_lagPitch = ViewFxClamp(m_lagPitch, -kMaxLagDeg, kMaxLagDeg);
}

ViewFxVmOffset ViewFxWeaponFloat::VmOffset() const {
	ViewFxVmOffset o;
	if (!Enabled()) return o;
	o.x = -m_lagYaw * m_amount * kUnitsPerDeg;
	o.z = -m_lagPitch * m_amount * kUnitsPerDeg;
	return o;
}

} // namespace Filmmaker
