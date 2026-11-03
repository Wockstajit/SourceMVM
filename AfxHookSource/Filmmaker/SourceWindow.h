#pragma once

// Free resizing of the CS:S window, as CS2's (AfxHookSource2/Filmmaker/Window/WindowResize.h): drag an
// edge to any size, a card in the middle of the window shows the size and the closest named aspect
// ratio while you drag, and Shift snaps to a named ratio or a standard resolution. The ratio table,
// the snap, the Windows Snap guard and the card are CS2's (Window/AspectRatioMath.h,
// WindowSnapMath.h, WindowPrecisionGuard.h, WindowSizeOverlay.h, WindowGeometry.h).
//
// What differs is the window. CS:S draws a plain Win32 window (class Valve001) with a fixed frame
// and no SDL, so this adds a sizing border while the game is windowed, and when a drag ends asks the
// engine for the new size with `mat_setvideomode w h 1`. The engine sets only a size it has as a
// mode, so the size is first written into its custom windowed mode (Engine_SetWindowedModeSize).
// The engine then centres the window on the main monitor; the move is answered with the place the
// drag left it. The mode change also shows HUD panels that are hidden until used, which only the next
// tick the demo reads hides again, so a paused demo steps one tick; and SourceTV's spectator bar
// keeps the old width until the observer mode changes, so first person and chase step to the other
// mode for a few frames.
//
// Threading: CS:S pumps its window's messages on the thread that runs its frames, so everything here
// runs on the main thread, including from inside the modal resize loop.

#include <windows.h>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// Every message of the game window, before the game sees it. Consumes none; the only writes are the
// proposed rects of WM_SIZING (the Shift snap), WM_WINDOWPOSCHANGING (Windows Snap during a Shift
// drag, and the engine's centring after a commit) and WM_GETMINMAXINFO (the smallest size).
void Window_OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Main thread, once a frame: the sizing border, a finished drag's commit, and whether the engine
// followed it.
void Window_Frame();

void Window_Shutdown();

void Window_PrintHealth();

// mirv_filmmaker window status|resizable|size|aspect|ratios|snap|overlay
void Window_Command(int argc, advancedfx::ICommandArgs* args);

} // namespace SourceFilmmaker
