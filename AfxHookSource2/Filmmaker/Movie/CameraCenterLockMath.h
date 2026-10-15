#pragma once

namespace Filmmaker {

inline double CameraCenterLockScale(bool enabled, double strengthPct) {
	if (!enabled) return 1.0;
	if (strengthPct < 0.0) strengthPct = 0.0;
	else if (strengthPct > 100.0) strengthPct = 100.0;
	return 1.0 - strengthPct / 100.0;
}

inline void CameraCenterLockApply(bool enabled, double strengthPct, double& pitch, double& yaw) {
	const double scale = CameraCenterLockScale(enabled, strengthPct);
	pitch *= scale;
	yaw *= scale;
}

} // namespace Filmmaker
