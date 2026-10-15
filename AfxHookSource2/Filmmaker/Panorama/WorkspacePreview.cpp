#include "WorkspacePreview.h"
#include "../Movie/HudBlackout.h" // F8 blackout releases the scaled preview

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (ui.preview probe)
#include "../Render/RenderSessionDriver.h" // capture gate: never bake the workspace blit into a plate

#include "../../ViewportScaler.h"
#include "../../WrpConsole.h"

namespace Filmmaker {

WorkspacePreview& WorkspacePreviewRef() {
	static WorkspacePreview s_instance;
	return s_instance;
}

const char* WorkspacePreview::OwnerName(Owner owner) {
	switch (owner) {
	case Owner::CameraEditor: return "CameraEditor";
	case Owner::Config: return "Config";
	default: return "none";
	}
}

void WorkspacePreview::Submit(Owner owner, float x0, float y0, float x1, float y1) {
	if (owner == Owner::None)
		return;
	// Reject a degenerate rect here rather than at each call site: this is the one place that
	// knows what the render layer will accept, and a bad rect must never reach it.
	if (!(x1 - x0 > 0.01f) || !(y1 - y0 > 0.01f)
		|| x0 < 0 || y0 < 0 || x1 > 1.0001f || y1 > 1.0001f)
		return;
	// Highest precedence wins regardless of who ran first this frame.
	if ((int)owner < (int)m_pendingOwner)
		return;
	m_pendingOwner = owner;
	m_pendingX0 = x0; m_pendingY0 = y0; m_pendingX1 = x1; m_pendingY1 = y1;
}

void WorkspacePreview::Release(Owner owner) {
	if (m_pendingOwner == owner)
		m_pendingOwner = Owner::None;
}

void WorkspacePreview::Commit() {
	const Owner owner = m_pendingOwner;
	// The scaler is a render-layer blit, not a Panorama child. Hiding the workspace root therefore
	// does not hide this copy of the Beauty frame; override streams would capture it as a small
	// picture inside the plate. Use the same capture window as the chrome gate.
	// Same reasoning covers the F8 blackout: the scaler is a render-layer blit, not a Panorama
	// child, so hiding the workspace chrome does NOT undo the scaling. Without this the clean
	// screen would be the game shrunk into a corner with black around it, which is the opposite
	// of what F8 is for.
	const bool active = owner != Owner::None && !RenderSessionDriver_WantsChromeHidden()
		&& !HudBlackout_Active();

	if (active) {
		m_curOwner = owner;
		m_curX0 = m_pendingX0; m_curY0 = m_pendingY0;
		m_curX1 = m_pendingX1; m_curY1 = m_pendingY1;
	} else {
		m_curOwner = Owner::None;
		m_curX0 = m_curY0 = m_curX1 = m_curY1 = 0;
	}

	AfxViewportScaler::SetRequest(active, m_curX0, m_curY0, m_curX1, m_curY1);

	// Change-gated: an owner handover, or the preview turning on/off, is exactly the transition
	// that used to depend on call order. A per-frame line would bury it.
	static Owner s_lastOwner = Owner::None;
	static float s_lastX0 = -1, s_lastY0 = -1, s_lastX1 = -1, s_lastY1 = -1;
	if (MvmDebugLog_Active()
		&& (s_lastOwner != m_curOwner || s_lastX0 != m_curX0 || s_lastY0 != m_curY0
			|| s_lastX1 != m_curX1 || s_lastY1 != m_curY1)) {
		s_lastOwner = m_curOwner;
		s_lastX0 = m_curX0; s_lastY0 = m_curY0; s_lastX1 = m_curX1; s_lastY1 = m_curY1;
		MvmDebugLog_Linef("ui.preview", "commit owner=%s active=%d rect=%.5f %.5f %.5f %.5f",
			OwnerName(m_curOwner), active ? 1 : 0, m_curX0, m_curY0, m_curX1, m_curY1);
	}

	// The pending slot is per-frame: every owner must re-submit each frame it still wants the
	// viewport, so a panel that stops running (torn down, demo unloaded, force-exit) releases it
	// automatically instead of leaving a stale blit behind. That auto-release is what all the
	// scattered SetRequest(false, ...) calls used to be doing by hand.
	m_pendingOwner = Owner::None;
}

bool WorkspacePreview::Active(float& x0, float& y0, float& x1, float& y1) const {
	if (m_curOwner == Owner::None)
		return false;
	x0 = m_curX0; y0 = m_curY0; x1 = m_curX1; y1 = m_curY1;
	return true;
}

} // namespace Filmmaker
