#pragma once

// THE SCALED PREVIEW, done the way CS2 does it.
//
// The world renders FULL-SCREEN with the camera, projection and viewport untouched. Then, at the
// pre-UI moment (DeadlockPreUiHook.h -- world drawn, Panorama not yet composited), this copies the
// backbuffer into a temp texture, CLEARS the backbuffer, and redraws the copy scaled into the
// preview rect. Panorama then composites the editor chrome on top; the chrome leaves the preview
// region transparent, so the scaled world shows through, and opaque chrome covers the black
// remainder.
//
// WHY THIS AND NOT THE VIEWPORT WRITE IT REPLACES. Shrinking the engine's own viewport (the first
// attempt, DeadlockViewSetup.cpp) fails three ways at once, and all three were reported as one
// "the preview doesn't render correctly":
//
//   * culling follows the viewport, so geometry near the frame edge is culled away;
//   * nothing clears the remainder, so every pixel outside the rect keeps the previous frame;
//   * the view block's other fields still say full screen, so consumers reading those -- the
//     world-space HUD projection most visibly -- place against a rect the world is not in.
//
// A full-screen render fixes the first, the explicit clear fixes the second, and the third simply
// never arises because the view struct is not touched at all.
//
// THREADING. Blit() runs on the RENDER thread, from inside the engine's own callback. Everything
// it draws is recorded on a DEFERRED context and replayed with
// ExecuteCommandList(RestoreContextState = TRUE), so the immediate context's state is intact for
// the UI pass that follows. The rect is published from the main thread and read here; a torn read
// costs one frame of a slightly wrong rectangle, which is why it is four floats and not a pointer.

#include <d3d11.h>

namespace Deadlock {

// Create and destroy the D3D objects. Driven from the Present hook, which is where the swapchain
// and therefore the device first become known.
void ViewportBlit_BeginDevice(ID3D11Device* device);
void ViewportBlit_EndDevice();

bool ViewportBlit_Ready();

// Publish the rect to scale into, as normalised backbuffer fractions: (x0,y0) top-left,
// (x1,y1) bottom-right. `active = false` leaves the frame full-screen.
void ViewportBlit_SetRequest(bool active, float x0, float y0, float x1, float y1);

bool ViewportBlit_Active();

// A DIAGNOSTIC STAND-DOWN, separate from the request above.
//
// The request says "the editor wants the frame scaled into this rect"; this says "do not touch the
// frame at all, whatever anyone wants". They are different questions, and keeping them apart is
// what lets a single command answer "is the blit the thing that is eating this?" without closing
// the workspace and changing five other variables at the same time.
void ViewportBlit_SetSuppressed(bool suppressed);
bool ViewportBlit_Suppressed();

// Called on the render thread at the pre-UI moment. Finds the bound render target and scales the
// frame into the published rect.
void ViewportBlit_RunPreUi();

// Safe viewport-only boundary used by Deadlock's framed workspace. The world is rendered into the
// reduced view rectangle; immediately before Panorama starts, restore a full-target D3D viewport
// and scissor so the editor itself is never clipped to the world rectangle. This binds no custom
// vertex buffer and records no deferred command list.
void ViewportBlit_RestoreFullViewportPreUi();

void ViewportBlit_Report();

} // namespace Deadlock
