#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Independent render bridge and runtime diagnostics for the Customize Player composition panel.
//
// Installs detours on RTTI/vtable-resolved methods:
//   * CViewRender::Render (slot 5; identified by its profiling strings),
//   * CUI_Player3dPanel's per-frame dispatcher (slot 99), and
//   * the CUI_3dPanel ready path (slot 89), composition renderer (slot 88), and its
//     frame-gated scene submission helper.
//
// While the Customize modal is open, only its active preview panel enters the engine's existing
// draw body on each Panorama submission instead of waiting for the demo-driven three-frame
// throttle. It does not change a demo/frame counter, clock, or play state. See
// Docs/panorama/panorama-player-preview-summary.md.
// drawAllPanels: arm the independent draw for every submitted 3D panel (the Config picker's
// death-animation preview, whose panel never reaches the ready hook).
void PreviewRenderProbe_RunFrame(bool customizeModalOpen, bool drawAllPanels = false);

void PreviewRenderProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
