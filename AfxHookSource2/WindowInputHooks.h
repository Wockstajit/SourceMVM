#pragma once

#include <Windows.h>

// UI cursor probe for the experimental graph editor. Captured in the WndProc (below) from the
// raw Windows mouse messages so the Panorama JS can do continuous After-Effects-style dragging
// (Panorama itself has no mouse-move event). Plain scalars; written on the message thread and
// read on the main thread once per frame -- a torn read is harmless (next frame corrects it).
struct AfxUiCursorProbe { int x = 0, y = 0; bool lmb = false; bool rmb = false; unsigned seq = 0; HWND hwnd = nullptr; };
extern AfxUiCursorProbe g_UiCursorProbe;

// Client-rect override for the render-resolution stage-2 path: while active, the GAME window's
// client rect is reported as width x height to the hooked modules (SDL3 / rendersystemdx11), so
// the engine sizes its swapchain and scene render targets to the requested output size while the
// OS window keeps its real size (DXGI then scales the present). Owned and driven exclusively by
// Filmmaker/Render/RenderResolution.cpp. Plain fields on purpose (same pattern as
// AfxUiCursorProbe): written on the main thread, read from whichever thread the engine queries
// on; a torn read is corrected by the next query.
struct AfxClientRectOverride { bool active = false; int width = 0; int height = 0; };
extern AfxClientRectOverride g_AfxClientRectOverride;

extern WNDPROC g_NextWindProc;
extern bool g_afxWindowProcSet;

// --- Window / cursor hook functions ---
LRESULT CALLBACK new_Afx_WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
BOOL WINAPI new_GetClientRect(HWND hWnd, LPRECT lpRect);
LONG_PTR WINAPI new_GetWindowLongPtrW(HWND hWnd, int nIndex);
LONG_PTR WINAPI new_SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
BOOL WINAPI new_GetCursorPos(LPPOINT lpPoint);
BOOL WINAPI new_SetCursorPos(int X, int Y);
HCURSOR WINAPI new_SetCursor(HCURSOR hCursor);
int WINAPI new_ShowCursor(BOOL bShow);
HWND WINAPI new_SetCapture(HWND hWnd);
BOOL WINAPI new_ReleaseCapture();
BOOL WINAPI new_ClipCursor(const RECT* lpRect);
