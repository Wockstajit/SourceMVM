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

// From HLAE's Hook_VClient_RenderView::GetSuspendMirvInput: the cursor is the filmmaker's, so HLAE's
// free camera waits (SourceUi.h Ui_SuspendMirvInput).
bool SuspendMirvInput();

} // namespace SourceFilmmaker
