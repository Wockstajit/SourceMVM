#include "ViewFxDeadzoneAim.h"

#include "FollowCameraMath.h" // FollowHalfTimeAlpha

#include <cmath>

namespace Filmmaker {

namespace {

constexpr double kAbsorb = 0.25;         // fraction of each aim delta absorbed (ARC9's 0.25)
constexpr double kBaseRadiusDeg = 4.0;   // free-aim radius at 100%
constexpr double kSnapDeg = 45.0;        // per-axis per-tick delta beyond this = POV snap, reset
constexpr double kQuietDeg = 0.04;       // per-tick delta below this counts as "aim at rest"
constexpr double kQuietDelay = 0.35;     // seconds of rest before recentering starts
constexpr double kRecenterHalfTime = 0.8; // at-rest recenter half-time, seconds
constexpr double kVmShiftPerDegYaw = 0.12;   // viewmodel X units per lag degree (subtle hint)
constexpr double kVmShiftPerDegPitch = 0.10; // viewmodel Z units per lag degree

} // namespace

void ViewFxDeadzoneAim::Advance(const ViewFxInput& in) {
	const double dp = in.dPitch, dyw = in.dYaw;

	if (std::fabs(dp) > kSnapDeg || std::fabs(dyw) > kSnapDeg) {
		// POV snap (spectator switch / demo cut / whip flick): recenter instantly rather than
		// banking a bogus offset from a jump that was not hand aim.
		m_freeAimPitch = m_freeAimYaw = 0.0;
		m_quietTime = 0.0;
		return;
	}

	if (!Enabled()) {
		m_freeAimPitch = m_freeAimYaw = 0.0;
		m_quietTime = 0.0;
		m_camDeltaPitch = m_camDeltaYaw = 0.0;
		return;
	}

	const double radius = kBaseRadiusDeg * (m_intensity / 100.0);
	m_freeAimPitch += dp * kAbsorb;
	m_freeAimYaw += dyw * kAbsorb;
	if (m_freeAimPitch > radius) m_freeAimPitch = radius; else if (m_freeAimPitch < -radius) m_freeAimPitch = -radius;
	if (m_freeAimYaw > radius) m_freeAimYaw = radius; else if (m_freeAimYaw < -radius) m_freeAimYaw = -radius;
	const double mag = std::sqrt(m_freeAimPitch * m_freeAimPitch + m_freeAimYaw * m_freeAimYaw);
	if (mag > radius && mag > 0.0) { // 2D clamp, same as ARC9's mag2d clamp
		m_freeAimPitch *= radius / mag;
		m_freeAimYaw *= radius / mag;
	}

	// At-rest recenter (our addition; ARC9 holds forever): only after the aim has been quiet for
	// a while, and slow -- never fights an in-progress swipe.
	if (std::fabs(dp) < kQuietDeg && std::fabs(dyw) < kQuietDeg) {
		m_quietTime += in.dt;
		if (m_quietTime > kQuietDelay) {
			const double aRe = FollowHalfTimeAlpha(kRecenterHalfTime, in.dt);
			m_freeAimPitch -= m_freeAimPitch * aRe;
			m_freeAimYaw -= m_freeAimYaw * aRe;
		}
	} else {
		m_quietTime = 0.0;
	}

	// Camera shows (true aim - freeAim). Lock crosshair suppresses the camera lag so the
	// crosshair stays at the true aim; only the gun free-aims (via VmOffset).
	m_camDeltaPitch = m_lockCrosshair ? 0.0 : -m_freeAimPitch;
	m_camDeltaYaw = m_lockCrosshair ? 0.0 : -m_freeAimYaw;
}

ViewFxVmOffset ViewFxDeadzoneAim::VmOffset() const {
	ViewFxVmOffset o;
	if (!Enabled()) return o;
	o.x = m_camDeltaYaw * kVmShiftPerDegYaw;
	o.z = m_camDeltaPitch * kVmShiftPerDegPitch;
	return o;
}

} // namespace Filmmaker
