#pragma once

// Which camera marker the view aims at: the one most centred in a narrow cone (about 5 degrees)
// that widens up close by the angle a marker covers, so near markers stay easy to grab and standing
// on one always registers. Markers behind the camera are never picked. Engine-free, so CS2
// (CameraPathPicking.cpp) and the CS:S hook pick alike; Automation/tests/cam-marker-pick-math-tests.cpp
// pins it.

#include "CamMarkers.h"

#include <cmath>
#include <vector>

namespace Filmmaker {
namespace CamMarkerPick {

constexpr double kAimConeCos = 0.9962;    // cos(~5 deg): the base acceptance cone
constexpr double kAimMarkerRadius = 30.0; // world half-size of a marker, for the near-range widening

// The aimed marker's index, or -1. origin and angles (pitch, yaw, roll in degrees) are the view.
inline int Aimed(const std::vector<CamMarker>& markers, const double origin[3], const double angles[3]) {
	constexpr double kPi = 3.14159265358979323846;
	const double pr = angles[0] * kPi / 180.0, yr = angles[1] * kPi / 180.0;
	const double fx = std::cos(pr) * std::cos(yr), fy = std::cos(pr) * std::sin(yr), fz = -std::sin(pr);
	int best = -1;
	double bestDot = -2.0;
	for (int i = 0; i < (int)markers.size(); ++i) {
		const CamMarker& m = markers[(size_t)i];
		const double dx = m.x - origin[0], dy = m.y - origin[1], dz = m.z - origin[2];
		const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-3) { // inside the marker
			best = i;
			bestDot = 1.0;
			continue;
		}
		const double inv = 1.0 / len;
		const double dot = (dx * fx + dy * fy + dz * fz) * inv;
		if (dot <= 0.0) continue;
		double coneCos = kAimConeCos;
		double sinHalf = kAimMarkerRadius * inv;
		if (sinHalf > 0.85) sinHalf = 0.85; // keeps the sqrt sane very close in
		const double nearCos = std::sqrt(1.0 - sinHalf * sinHalf);
		if (nearCos < coneCos) coneCos = nearCos; // the wider of the base and the covered cone
		if (dot >= coneCos && dot > bestDot) {
			bestDot = dot;
			best = i;
		}
	}
	return best;
}

} // namespace CamMarkerPick
} // namespace Filmmaker
