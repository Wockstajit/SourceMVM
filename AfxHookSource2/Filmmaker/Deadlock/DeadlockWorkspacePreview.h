#pragma once

// THE single owner of the scaled-preview rectangle.
//
// A workspace that frames the game as a preview has to agree with itself about exactly one
// rectangle, in three places at once: the Panorama chrome that draws the frame and the letterbox
// around it, the engine that renders the world into it, and anything that maps a screen position
// back into the world. The CS2 side proved what happens when two panels each keep their own copy --
// whichever one ran last in the frame won, so a Config/Editor swap could leave the chrome following
// one rect and the world blit another, and the visible symptom was a workspace showing an unscaled
// CROP with the frame drawn somewhere else.
//
// So: panels SUBMIT what they want, this module decides, and exactly one rect is published per
// frame from one place.
//
// FRAME PIPELINE. Submit() writes a pending slot; Commit() -- once per frame, after every panel has
// run -- promotes pending to current and pushes it to the view layer. Consumers read the CURRENT
// slot, so a panel that runs earlier in the frame still agrees with the engine; the cost is a
// deterministic one-frame lag rather than an order-dependent race.
//
// THE PENDING SLOT IS PER-FRAME. Every owner re-submits each frame it still wants the preview, so a
// panel that stops running -- torn down, demo unloaded, workspace closed -- releases the rect
// automatically instead of leaving a stale one behind.
//
// HOW THE WORLD IS ACTUALLY SHRUNK. Not by a post-process blit. Deadlock's only render-thread hook
// here is the swapchain Present, and by then Panorama has already composited the UI into the back
// buffer -- a blit at that point would shrink the chrome along with the world, which is the opposite
// of a preview. Instead the rect is written into the view struct's own VIEWPORT fields at the
// view-setup hook, so the engine renders the world into the sub-rectangle natively and the chrome
// composites over the rest at full size. See Deadlock::ViewSetup_SetPreviewViewport.
//
// That write is a hypothesis until it is confirmed in game, so it is validated on read-back and
// stands itself down when the engine does not take it. Chrome does not depend on it: with the
// viewport write off, the workspace still draws a framed preview window, it just frames a crop of
// the full-screen render instead of the whole frame. That is a degraded preview, not a broken one,
// and `mirv_filmmaker preview` says which of the two you are looking at.

namespace Filmmaker {

enum class PreviewOwner {
	None = 0,
	Config = 1,
	CameraEditor = 2, // outranks Config: the full workspace beats a sidebar
};

// A panel wants the world scaled into `rect` this frame, as normalised screen fractions in [0,1].
// Ignored when a higher-precedence owner has already submitted for the same frame.
void WorkspacePreview_Submit(PreviewOwner owner, float x0, float y0, float x1, float y1);

// A panel no longer wants the preview. Safe every frame; only clears if that owner is pending.
void WorkspacePreview_Release(PreviewOwner owner);

// Main thread, once per frame, AFTER every panel has run.
void WorkspacePreview_Commit();

// The committed rect. False when nothing owns the preview.
bool WorkspacePreview_Active(float& x0, float& y0, float& x1, float& y1);
bool WorkspacePreview_Active();

PreviewOwner WorkspacePreview_CurrentOwner();
const char* WorkspacePreview_OwnerName(PreviewOwner owner);

// Whether the engine is actually rendering into the rect (true) or the chrome is framing a crop of
// the full-screen render (false). This is the difference the user sees, so it is reported rather
// than assumed.
bool WorkspacePreview_Scaling();

void WorkspacePreview_Report();

} // namespace Filmmaker
