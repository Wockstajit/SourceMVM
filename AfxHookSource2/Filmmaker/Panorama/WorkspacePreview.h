#pragma once

// THE single owner of the scaled-preview viewport.
//
// WHY THIS EXISTS. "Scale the rendered frame down into the workspace's preview rectangle" was
// implemented twice -- once in CameraEditorHud, once in ConfigHud -- with eleven separate
// AfxViewportScaler::SetRequest call sites between them, each guarded by an ad-hoc
// `if (!ConfigHud_Enabled())` so the two would not clobber each other. Three things went wrong
// with that arrangement:
//
//   * BOTH panels wrote the request every frame, so which one actually took effect depended on
//     Filmmaker::RunMainThreadFrame's call order (ConfigHud runs at 476, CameraEditorHud at 484)
//     rather than on any stated rule;
//   * the consumers that scale the native game HUD into the SAME rect (CameraTimelineHud at 466,
//     AimCrosshairHud at 474) run BEFORE both producers, so they read whichever panel's rect was
//     left over from the previous frame -- and they asked the two panels in a fixed order, so
//     during a Config<->Editor swap they could follow a different panel than the world blit did;
//   * "is the scaled preview on?" had two independent spellings (the editor's persisted toggle
//     AND m_previewValid, versus Config's demo-and-valid), which is how the Camera Editor ended
//     up showing an unscaled CROP while Config on the same screen scaled correctly.
//
// So: panels SUBMIT what they want, this module decides, and exactly one SetRequest is issued per
// frame from one place (Commit).
//
// FRAME PIPELINE. Submit() writes a pending slot; Commit() -- called once per frame, after every
// panel has run -- promotes pending to current and pushes it to the render layer. Consumers read
// the CURRENT slot, so they always agree with the world blit even though they run earlier in the
// frame; the cost is a deterministic one-frame lag instead of an order-dependent race.
//
// SCALING IS NOT OPTIONAL. A workspace preview that is not scaled is a crop: it shows more of the
// world than the frame it claims to preview, and the native HUD lands outside the box entirely.
// Config never had a toggle for this and was always correct; the Camera Editor's persisted
// on/off toggle is what left users staring at a crop. Ownership of a valid preview rect is now
// the whole condition -- see CameraEditor_SetScale for what became of the toggle.

namespace Filmmaker {

class WorkspacePreview {
public:
	// Precedence order, highest first. The Camera Editor outranks Config because it is the full
	// workspace and Config is a sidebar; in practice they are mutually exclusive, but a stated
	// rule beats "whichever RunFrame happened to be last".
	enum class Owner { None = 0, Config = 1, CameraEditor = 2 };

	// A panel wants the viewport scaled into `rect` this frame. Ignored when a higher-precedence
	// owner has already submitted for the same frame.
	void Submit(Owner owner, float x0, float y0, float x1, float y1);

	// A panel no longer wants the viewport (closed, no valid rect, not in a demo). Safe to call
	// every frame; only clears if that owner is the one currently pending.
	void Release(Owner owner);

	// Main thread, once per frame, AFTER every panel has run. Promotes the pending submission to
	// current and issues the single AfxViewportScaler::SetRequest.
	void Commit();

	// The committed rect, for the bridges that scale the native game HUD into the same region.
	// False when nothing owns the viewport.
	bool Active(float& x0, float& y0, float& x1, float& y1) const;
	bool Active() const { return m_curOwner != Owner::None; }
	Owner CurrentOwner() const { return m_curOwner; }

	static const char* OwnerName(Owner owner);

private:
	Owner m_pendingOwner = Owner::None;
	float m_pendingX0 = 0, m_pendingY0 = 0, m_pendingX1 = 0, m_pendingY1 = 0;

	Owner m_curOwner = Owner::None;
	float m_curX0 = 0, m_curY0 = 0, m_curX1 = 0, m_curY1 = 0;
};

WorkspacePreview& WorkspacePreviewRef();

} // namespace Filmmaker
