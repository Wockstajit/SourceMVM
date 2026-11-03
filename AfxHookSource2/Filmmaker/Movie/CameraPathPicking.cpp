// Camera-path viewport hover picking: which marker the free-cam crosshair is aiming at.
// Split out of CameraPath.cpp (the editing/curve/playback/persistence core) because this
// is the only UI-viewport-dependent piece -- it reads the LIVE camera pose off
// CameraBridge every frame and feeds m_hovered, which the K/L/F hotkeys, the marker
// menu, and the drawer highlight all key off.

#include "CameraPath.h"

#include "CameraBridge.h"
#include "CamMarkerPickMath.h"

#include "../../../Shared/AfxConsole.h"

#include <cmath>

namespace Filmmaker {

// Opt-in: print "[campath] aiming at marker #N" on hover change. Off by default -- it floods
// the console during freecam. Flip to true here (or wire a command) if you want the feedback.
static bool s_LogAiming = false;

void CameraPath::UpdateHover() {
	const int n = m_data.Count();
	if (n == 0) { m_hovered.store(-1); return; }

	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	const int best = CamMarkerPick::Aimed(m_data.All(), o, a); // CamMarkerPickMath.h, shared with CS:S
	// Track the hovered marker for the visual highlight. The per-frame "aiming at marker"
	// console message was pure spam (the marker highlight already shows the aim), so it is
	// gated behind an opt-in flag that defaults off to keep the console readable.
	int prev = m_hovered.exchange(best);
	if (s_LogAiming && prev != best && best >= 0)
		advancedfx::Message("[campath] aiming at marker #%d.\n", best);
}

} // namespace Filmmaker
