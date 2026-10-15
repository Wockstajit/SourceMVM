#pragma once

// Startup-only, same-aspect scene-render scaling that leaves the game window and swapchain at
// their real video-mode dimensions.
//
// FOUND BY REVERSE ENGINEERING (Ghidra, client.dll, 2026-08-01; see the size-source section of
// Docs/agent/render-system-contract.md). CS2's render pipeline builder asks the render device
// for the frame size exactly once and derives every render-target dimension from it:
//
//     lVar14 = renderDevice->vtable[+0x20](renderDevice, name);   // index 4
//     width  = *(uint32*)(lVar14 + 0x8);
//     height = *(uint32*)(lVar14 + 0xC);
//     ... w>>2, w/6, h/2, h/3, FUN_181168890(this+0x128, w, h, ...)
//
// The working path is deliberately composed instead of globally lying about the render-window
// size:
//   * CreateTexture2D descriptors allocate the engine2-owned colour/depth targets physically at
//     the scaled dimensions;
//   * this size query returns a private scaled copy only to the client target builder, its frame
//     size consumer and SceneSystem;
//   * the common SceneSystem scratch allocator scales matching client requests through another
//     private descriptor copy;
//   * D3D viewports and scissors that match the real scene extent are scaled to the target.
// Render-system/swapchain callers continue to see the real window size. That separation is what
// keeps presentation correct while the recorder captures the larger multisampled scene target.
//
// VERSION-COUPLED. The device-interface global is a hardcoded client.dll offset and WILL move on
// a CS2 update. Everything here validates before acting and degrades to a warning rather than
// crashing: the interface pointer must look like a live object, and the size it reports must
// look like a plausible resolution, or the hook passes through untouched.
//
// STATUS 2026-08-01: proven at a 1.25 factor from a 1600x1200 launch. Runtime evidence showed a
// 2000x1500 captured MSAA colour target, a 1600x1200 backbuffer, no scratch-allocation/bind
// warnings, and a correct full-frame image without tiling, cropping or black presentation.
// This remains experimental, version-coupled and opt-in: without MVM_RENDER_ENGINESIZE the
// detours are not installed. The factor cannot be changed live because startup-created resource
// families would disagree. It is a uniform multiplier, not yet the arbitrary per-job output-size
// control used by the render-session driver.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Installs the detour if it is not installed yet and the interface can be resolved+validated.
// Safe to call every frame; cheap once resolved. Returns true when the hook is live.
bool RenderEngineSize_EnsureInstalled();

// Multiplier applied to the size the engine reads. 1.0 (default) = completely inert.
float RenderEngineSize_Factor();

// Last validated source/output pair produced by the scoped size-query hook. Returns false until
// one has been observed. The viewport/scissor bridge uses this exact pair instead of independently
// rounding the factor again.
bool RenderEngineSize_TargetDimensions(
	unsigned& sourceWidth, unsigned& sourceHeight,
	unsigned& targetWidth, unsigned& targetHeight);

// Human-readable state for the console.
std::string RenderEngineSize_Status();

// "mirv_filmmaker render enginesize [status|<factor>|off]". Factor/off explain the required
// restart; they intentionally never mutate a live render-resource graph.
void RenderEngineSize_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
