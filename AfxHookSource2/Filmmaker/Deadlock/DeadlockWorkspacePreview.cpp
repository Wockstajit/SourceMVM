#include "DeadlockWorkspacePreview.h"

#include "../Debug/MvmDebugLog.h"
#include "../../Deadlock/DeadlockViewSetup.h"
#include "../../Deadlock/DeadlockViewportBlit.h"
#include "../../../Shared/AfxConsole.h"

namespace Filmmaker {

namespace {

PreviewOwner g_pendingOwner = PreviewOwner::None;
float g_pendingX0 = 0, g_pendingY0 = 0, g_pendingX1 = 0, g_pendingY1 = 0;

PreviewOwner g_curOwner = PreviewOwner::None;
float g_curX0 = 0, g_curY0 = 0, g_curX1 = 0, g_curY1 = 0;

} // namespace

const char* WorkspacePreview_OwnerName(PreviewOwner owner) {
	switch (owner) {
	case PreviewOwner::CameraEditor: return "CameraEditor";
	case PreviewOwner::Config: return "Config";
	default: return "none";
	}
}

void WorkspacePreview_Submit(PreviewOwner owner, float x0, float y0, float x1, float y1) {
	if (owner == PreviewOwner::None) return;

	// Reject a degenerate rect HERE rather than at each call site. This is the one place that knows
	// what the view layer will accept, and a bad rect must never reach it: a zero-width viewport is
	// not a small picture, it is a division by zero inside the engine's projection.
	if (!(x1 - x0 > 0.01f) || !(y1 - y0 > 0.01f)
		|| x0 < 0 || y0 < 0 || x1 > 1.0001f || y1 > 1.0001f)
		return;

	if ((int)owner < (int)g_pendingOwner) return; // highest precedence wins, whoever ran first

	g_pendingOwner = owner;
	g_pendingX0 = x0; g_pendingY0 = y0; g_pendingX1 = x1; g_pendingY1 = y1;
}

void WorkspacePreview_Release(PreviewOwner owner) {
	if (g_pendingOwner == owner) g_pendingOwner = PreviewOwner::None;
}

void WorkspacePreview_Commit() {
	const PreviewOwner owner = g_pendingOwner;
	const bool active = owner != PreviewOwner::None;

	if (active) {
		g_curOwner = owner;
		g_curX0 = g_pendingX0; g_curY0 = g_pendingY0;
		g_curX1 = g_pendingX1; g_curY1 = g_pendingY1;
	} else {
		g_curOwner = PreviewOwner::None;
		g_curX0 = g_curY0 = g_curX1 = g_curY1 = 0;
	}

	// Keep Deadlock's view full-size so culling and cached paused frames remain complete. At the
	// world/UI boundary, copy that complete frame into the solved preview rectangle.
	Deadlock::ViewSetup_SetPreviewViewport(false, 0, 0, 1, 1);
	Deadlock::ViewportBlit_SetRequest(active, g_curX0, g_curY0, g_curX1, g_curY1);

	// Change-gated. An owner handover, or the preview turning on or off, is exactly the transition
	// that used to depend on call order; a per-frame line would bury it.
	static PreviewOwner s_lastOwner = PreviewOwner::None;
	static float s_lastX0 = -1, s_lastY0 = -1, s_lastX1 = -1, s_lastY1 = -1;
	if (MvmDebugLog_Active()
		&& (s_lastOwner != g_curOwner || s_lastX0 != g_curX0 || s_lastY0 != g_curY0
			|| s_lastX1 != g_curX1 || s_lastY1 != g_curY1)) {
		s_lastOwner = g_curOwner;
		s_lastX0 = g_curX0; s_lastY0 = g_curY0; s_lastX1 = g_curX1; s_lastY1 = g_curY1;
		MvmDebugLog_Linef("deadlock.preview", "commit owner=%s active=%d rect=%.5f %.5f %.5f %.5f",
			WorkspacePreview_OwnerName(g_curOwner), active ? 1 : 0,
			g_curX0, g_curY0, g_curX1, g_curY1);
	}

	g_pendingOwner = PreviewOwner::None;
}

bool WorkspacePreview_Active(float& x0, float& y0, float& x1, float& y1) {
	if (g_curOwner == PreviewOwner::None) return false;
	x0 = g_curX0; y0 = g_curY0; x1 = g_curX1; y1 = g_curY1;
	return true;
}

bool WorkspacePreview_Active() { return g_curOwner != PreviewOwner::None; }

PreviewOwner WorkspacePreview_CurrentOwner() { return g_curOwner; }

// EITHER mechanism counts as "the world really is scaled into the frame".
//
// It used to ask only the view-struct viewport write, which is now off by default -- so with the
// blit doing the work correctly the panel still announced "framing a crop" and the inspector
// showed a repair warning for a fault that was not happening. A status line that is wrong about a
// working feature sends the next investigation somewhere there is nothing to find.
//
// The blit is asked FIRST because it is the real mechanism; the viewport write is the fallback.
bool WorkspacePreview_Scaling() {
	return Deadlock::ViewportBlit_Active() && Deadlock::ViewportBlit_Ready();
}

void WorkspacePreview_Report() {
	if (g_curOwner == PreviewOwner::None) {
		advancedfx::Message("  preview:   nobody owns the preview rect\n");
		return;
	}
	advancedfx::Message(
		"  preview:   owner=%s rect=%.4f %.4f %.4f %.4f  world=%s\n",
		WorkspacePreview_OwnerName(g_curOwner), g_curX0, g_curY0, g_curX1, g_curY1,
		WorkspacePreview_Scaling() ? "scaled by the pre-UI frame blit"
		                           : "FULL SCREEN (the frame shows a crop, not the whole frame)");
}

} // namespace Filmmaker
