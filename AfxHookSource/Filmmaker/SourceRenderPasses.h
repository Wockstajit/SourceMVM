#pragma once

// Extra passes for CS:S x64, rendered again in one engine frame before simulation advances.
// HLAE owns capture nodes, image buffers, presets and encoding. Depth uses its INTZ surface and
// depth shader. Normal uses that draw path with CS2's shared normal reconstruction. The model draw
// hook drops player/equipment draws for World only, or removes their fog for Players only.
// Key passes keep a fixed HDR scale for the fog and clear colour; other passes restore that scale.
// The final Beauty redraw restores the visible game frame before HLAE's screen capture.
// Main thread only, with mat_queue_mode 0 for the job.

#include "SourceRenderPassTable.h"

#include <string>
#include <vector>

namespace advancedfx {
class COutVideoStreamCreator;
}

namespace SourceFilmmaker {

struct ExtraPassJob {
	ExtraPass kind = ExtraPass::NoGun;
	advancedfx::COutVideoStreamCreator* creator = nullptr; // HLAE's writer for the pass's folder
};

struct ExtraPassLook {
	float depthNear = 8, depthFar = 4096;
	unsigned char key[3] = { 0, 205, 0 };
};

// Once a frame until the client is up and View_Render is hooked.
void RenderPasses_Install();
bool RenderPasses_Installed();
// Opens a capture for each pass (each keeps its creator). False with the reason when it cannot.
bool RenderPasses_Begin(const std::vector<ExtraPassJob>& passes, const ExtraPassLook& look, std::string& error);
// This frame's View_Render draws and captures the passes.
void RenderPasses_Arm();
// One live pass while the workspace is open. No capture or encoder is created; jobs take priority.
void RenderPasses_Preview(ExtraPass kind, const ExtraPassLook& look, bool enabled);
// Closes the captures; their writers finish what they hold on their own threads.
void RenderPasses_End();
// Release the world-reference surface before D3D9 Reset; a job interrupted by it fails safely.
void RenderPasses_DeviceLost();
// Frames whose passes were drawn since Begin, and what last went wrong ("" when nothing did).
int RenderPasses_Frames();
const std::string& RenderPasses_Error();
void RenderPasses_PrintHealth();

} // namespace SourceFilmmaker
