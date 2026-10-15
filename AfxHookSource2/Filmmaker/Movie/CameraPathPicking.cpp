// Camera-path viewport hover picking: which marker the free-cam crosshair is aiming at.
// Split out of CameraPath.cpp (the editing/curve/playback/persistence core) because this
// is the only UI-viewport-dependent piece -- it reads the LIVE camera pose off
// CameraBridge every frame and feeds m_hovered, which the K/L/F hotkeys, the marker
// menu, and the drawer highlight all key off.

#include "CameraPath.h"

#include "CameraBridge.h"

#include "../../../Shared/AfxConsole.h"

#include <cmath>

namespace Filmmaker {

namespace {
	constexpr double kPi = 3.14159265358979323846;
	// Marker picking. A NARROW, well-centred base cone (~5 deg) so selection feels precise,
	// WIDENED up close by the marker's subtended angle (kAimMarkerRadius / distance) so near
	// markers stay easy to grab and being right on top of one always registers.
	constexpr double kAimConeCos = 0.9962;    // cos(~5 deg): base acceptance cone
	constexpr double kAimMarkerRadius = 30.0; // world half-size for the near-range cone widen
}

// Opt-in: print "[campath] aiming at marker #N" on hover change. Off by default -- it floods
// the console during freecam. Flip to true here (or wire a command) if you want the feedback.
static bool s_LogAiming = false;

void CameraPath::UpdateHover() {
	const int n = m_data.Count();
	if (n == 0) { m_hovered.store(-1); return; }

	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	double pr = a[0] * kPi / 180.0; // pitch
	double yr = a[1] * kPi / 180.0; // yaw
	double fx = std::cos(pr) * std::cos(yr);
	double fy = std::cos(pr) * std::sin(yr);
	double fz = -std::sin(pr);

	int best = -1;
	double bestDot = -2.0;
	for (int i = 0; i < n; ++i) {
		const CamMarker& mk = m_data.At(i);
		double dx = mk.x - o[0];
		double dy = mk.y - o[1];
		double dz = mk.z - o[2];
		double len = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (len < 1e-3) { best = i; bestDot = 1.0; continue; } // basically inside the marker
		double inv = 1.0 / len;
		double dot = (dx * fx + dy * fy + dz * fz) * inv;
		if (dot <= 0.0) continue; // behind the camera -- never pick

		// Acceptance cone widens up close (marker subtends a bigger angle), so near markers
		// are easy and being on top always registers; far markers use the narrow base cone.
		double coneCos = kAimConeCos;
		double sinHalf = kAimMarkerRadius * inv;
		if (sinHalf > 0.85) sinHalf = 0.85; // clamp so the sqrt stays sane very close in
		double nearCos = std::sqrt(1.0 - sinHalf * sinHalf);
		if (nearCos < coneCos) coneCos = nearCos; // take the WIDER of base / subtended cone

		if (dot >= coneCos && dot > bestDot) { bestDot = dot; best = i; } // most-centred wins
	}
	// Track the hovered marker for the visual highlight. The per-frame "aiming at marker"
	// console message was pure spam (the marker highlight already shows the aim), so it is
	// gated behind an opt-in flag that defaults off to keep the console readable.
	int prev = m_hovered.exchange(best);
	if (s_LogAiming && prev != best && best >= 0)
		advancedfx::Message("[campath] aiming at marker #%d.\n", best);
}

} // namespace Filmmaker
