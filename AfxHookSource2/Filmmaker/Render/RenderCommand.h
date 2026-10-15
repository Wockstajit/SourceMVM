#pragma once

#include <atomic>
#include <string>

#include "../../../Shared/AfxConsole.h"
#include "RenderTypes.h"
#include "RenderValidation.h" // RenderValidationResult (returned by Render_ValidateCurrentJobFull)

namespace Filmmaker {

struct RenderRange;
enum class RenderCameraSource;

void Render_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

const RenderRange& Render_GetRange();
void Render_SetRange(const RenderRange& range);

RenderCameraSource Render_GetCameraSource();
void Render_SetCameraSource(RenderCameraSource source);

CameraPathExtension Render_GetExtension();
void Render_SetExtension(CameraPathExtension ext);

bool Render_IsJobActive();
// The camera-path tick a Camera Path render job is evaluating right now (false outside one).
// Any thread.
bool Render_PathCameraTick(double& tick);
void Render_SetJobActive(bool active);

RenderSessionState Render_GetSessionState();
void Render_SetSessionState(RenderSessionState state);

int Render_GetModalTab();
void Render_SetModalTab(int tab);

bool Render_IsModalOpen();
void Render_SetModalOpen(bool open);
// Applies the workspace's live visibility preview (RenderPasses::preview) to the game view, or clears
// it when the workspace is closed. No-op while a job runs: the job owns what is drawn.
void Render_ApplyLivePreview();

const char* Render_GetValidationString();

// Full structured validation of the CURRENT live settings (range/camera + output/passes), the
// same result the console command and the UI token are built from. Declared here so the session
// driver can gate start on exactly what the UI showed. Defined in RenderCommand.cpp.
RenderValidationResult Render_ValidateCurrentJobFull();

// Make the LIVE view match a render camera source (the WYSIWYG switch the camera buttons do).
// The session driver reuses it at job start so the recorded view is the previewed one.
void Render_ApplyCameraSourceLive(RenderCameraSource src, const char* cmd);

// The whole render-workspace state as one JSON object for the Camera Editor state bridge
// (range, resolved ticks, demo bounds, camera source, session state, validation, output and
// pass settings). Owned here so the HUD file does not have to track this module's field set.
std::string Render_BuildStateJson();

bool Render_IsCancelRequested();
void Render_RequestCancel();

// Demo tick bounds of the currently playing demo (from the engine start tick + the .dem
// header's playback_ticks, cached per demo path). false when unavailable (no demo / no header).
bool Render_GetDemoBounds(int& outStartTick, int& outEndTick);

// Resolve the CURRENT range mode to concrete ticks for UI display (FullDemo, CameraPathBounds,
// SelectedCameraSegment resolve here; CurrentRound resolves JS-side and is pushed back via
// "render setrange"; CustomTicks is already concrete). out always receives a copy of the
// current range (with ticks overwritten when resolution succeeds); the return value says
// whether the resolved span is actually usable (out.startTick < out.endTickExclusive).
bool Render_ResolveRangeForDisplay(RenderRange& out);

// One-line dump of the whole render-workspace decision state (modal, range mode, stored vs
// RESOLVED ticks, demo bounds, camera source, extension, validation) for the mvm_debug log
// (`state.render`, emitted after any `mirv_filmmaker render ...`). The console cannot carry
// this out of the game -- see Docs/agent/mvm-debug-instrumentation.md.
std::string Render_DebugStateLine();

} // namespace Filmmaker
