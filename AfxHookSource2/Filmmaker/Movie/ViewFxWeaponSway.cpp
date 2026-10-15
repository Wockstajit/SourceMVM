#include "ViewFxWeaponSway.h"

namespace Filmmaker {

ViewFxVmOffset ViewFxWeaponSway::Offset(float nowSeconds, double speed, unsigned long long speedSerial) {
	ViewFxVmOffset out;
	if (!Enabled()) {
		Reset();
		m_lastTarget = 0.0;
		m_lastStale = false;
		return out;
	}

	const ViewFxWeaponSwayOutput o =
		ViewFxWeaponSwayStep(m_state, (double)nowSeconds, speed, speedSerial, m_intensity);
	m_lastTarget = o.target;
	m_lastStale = o.stale;

	out.x = o.x;
	out.y = o.y;
	out.z = o.z;
	return out;
}

} // namespace Filmmaker
