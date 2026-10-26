#pragma once

// Entry points of the SOURCE:MVM filmmaker in the Source 1 hook (AfxHookSource). Everything the
// port adds lives under AfxHookSource/Filmmaker/; the inherited HLAE files only call in here.
//
// Threading: the window procedure and the client frame stages both run on the game's main thread.

#include <windows.h>

namespace SourceFilmmaker {

// Once the engine interfaces exist (MySetup).
void Init();

// From the hook's window procedure, for every message. Returns true when the message was ours.
bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// From the client's FRAME_RENDER_START.
void OnFrameRenderStart();

// From HLAE's Hook_VClient_RenderView::OnViewOverride after its own overrides: the view (pitch yaw
// roll in rx ry rz) for the filmmaker's view effects (Smooth View), then, final, for the camera
// path's markers drawn in the world. cameraOverridden when an HLAE override (mirv_input, a campath)
// moved the camera.
void OnViewOverride(float tx, float ty, float tz, float& rx, float& ry, float& rz, float& fov, bool cameraOverridden);

// From HLAE's Hook_VClient_RenderView::GetSuspendMirvInput: the cursor is the filmmaker's, so HLAE's
// free camera waits (SourceUi.h Ui_SuspendMirvInput).
bool SuspendMirvInput();

} // namespace SourceFilmmaker
