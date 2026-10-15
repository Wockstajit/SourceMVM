#pragma once

// Recording-side viewport-command probe for the render-resolution investigation.
//
// The D3D11-level stack capture proved the source-sized full-scene scissor is replayed from a
// command buffer on the rendersystemdx11 job thread, so an execution-time stack can never name
// the upstream owner. This probe detours the rendersystemdx11 function that RECORDS the
// viewport command (opcode 0x15: one logical rectangle that later becomes BOTH the D3D viewport
// and scissor), which runs on the frame-building thread -- its call stack points directly at the
// client/SceneSystem code that constructed the logical view rectangle.
//
// Diagnostic only: installs nothing unless MVM_RENDER_ENGINESIZE is active, and logs nothing
// unless mvm_debug is running.

namespace Filmmaker {

// Installs the detour once rendersystemdx11.dll is loaded and the engine-size experiment is
// active. Safe to call repeatedly from any thread; no-ops after the first success.
void RenderViewportRecordProbe_EnsureInstalled();

} // namespace Filmmaker
