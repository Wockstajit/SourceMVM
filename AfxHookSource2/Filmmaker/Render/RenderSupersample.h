#pragma once

// Physical-allocation and raster-state half of the scene render-scale path.
//
// WHY THIS CAN WORK (established by the render.capsize / render.rtcreate probes):
//   * the recorder captures the engine's multisampled SCENE colour target, NOT the swapchain
//     backbuffer (`sameObject=0`);
//   * only THREE multisampled colour targets exist at video-mode size, so the surface to
//     enlarge is small and enumerable;
//   * those targets are allocated through ID3D11Device::CreateTexture2D, which this DLL already
//     detours -- so their descriptors can be enlarged at the moment of creation.
// Enlarging the target alone would render the normal image into a corner of a bigger texture,
// because the engine sets its viewport from its own computed size; the viewport is therefore
// scaled by the same factor (RSSetViewports) so the scene fills the enlarged target.
//
// The older standalone MVM_RENDER_SS experiment established the failure ladder, in order:
//   1. targets enlarged only               -> frame entirely BLACK. Cause: D3D requires every
//      bound render target AND the depth-stencil view to share dimensions; scaling colour but
//      not depth made the runtime reject the binding. Fixed by scaling depth targets too.
//   2. + depth scaled                      -> renders, but the scene sits in the TOP-LEFT
//      QUADRANT. Cause: the scissor rectangle is a hard clip applied after the viewport
//      transform and was still the old size. Fixed by scaling scissor rects.
//   3. + scissor scaled                    -> the scene FILLS the frame, but the image is
//      TILED 2x2. Cause: downstream composite/post passes compute texture coordinates from the
//      engine's own notion of the render size (note the `InvViewportSize` shader constant found
//      in rendersystemdx11.dll), so uv runs 0..2 over the doubled texture and wraps.
//
// The production experiment now composes this module with RenderEngineSize: that scoped engine
// size path supplies the matching logical dimensions, while this module enlarges the physical
// colour/depth descriptors and bridges viewport/scissor state. The render-system's logical
// window metadata deliberately remains real-sized; scaling it made final presentation black.
//
// MVM_RENDER_ENGINESIZE drives the proven composed path. MVM_RENDER_SS and the supersample
// console command remain diagnostic standalone controls and can still produce tiled output.
// With neither enabled every function here is an exact no-op.

#include <windows.h> // RECT (scissor rects)
#include <cstddef>

struct D3D11_TEXTURE2D_DESC;
struct D3D11_VIEWPORT;

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Current factor. 1.0 = disabled.
float RenderSupersample_Factor();

// True while a factor > 1 is in effect.
bool RenderSupersample_Active();

// True when texture descriptors must be enlarged, either by the legacy standalone experiment
// or as the physical-allocation half of RenderEngineSize.
bool RenderSupersample_TextureAdjustmentActive();

// True when either the legacy descriptor experiment or RenderEngineSize needs D3D viewport and
// scissor state bridged from the real window dimensions to a larger scene target.
bool RenderSupersample_ViewportAdjustmentActive();

// Called from the CreateTexture2D detour BEFORE the real call. Enlarges the descriptor when it
// matches the scene-target signature, and remembers the base size so viewports can be scaled
// consistently. Returns true when it modified the descriptor.
bool RenderSupersample_AdjustTextureDesc(D3D11_TEXTURE2D_DESC& desc);

// Called from the RSSetViewports detour. Scales a viewport that matches the pre-scale scene
// size so the enlarged target is filled rather than rendered into a corner.
// Returns true when it modified the viewport.
bool RenderSupersample_AdjustViewport(D3D11_VIEWPORT& vp);

// Called from the RSSetScissorRects detour. The scissor is a HARD clip applied after the
// viewport transform: with the target and viewport enlarged but the scissor left at the old
// size, everything outside the original rectangle is discarded and the scene appears in the
// top-left quadrant of the frame (measured exactly that way before this was added).
// Returns true when it modified the rect.
bool RenderSupersample_AdjustScissor(RECT& rect);

// "mirv_filmmaker render supersample [factor|off|status]".
void RenderSupersample_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
