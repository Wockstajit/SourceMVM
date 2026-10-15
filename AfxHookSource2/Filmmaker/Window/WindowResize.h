#pragma once

// Free resizing of the CS2 window, with a live size + aspect-ratio readout while you drag.
//
// WHAT THIS OWNS
//   * making the game window user-resizable at all (CS2 ships a fixed-size windowed window);
//   * watching the OS resize messages and driving WindowSizeOverlay with the size and the closest
//     named aspect ratio (AspectRatioMath);
//   * checking afterwards whether the ENGINE actually followed the new window size, and saying so
//     in the mvm_debug log instead of leaving it to guesswork.
//
// WHAT IT DOES NOT OWN
//   * render-target sizing. Docs/agent/render-system-contract.md is the authority there, and its
//     rule stands: the capture is whatever the engine renders. This module never resizes a render
//     target and never lies about a client rect (that is the retired GetClientRect experiment).
//
// See Docs/features/filmmaker_window_resize.md for the reverse-engineering notes behind the SDL route and
// for what still has to be proven live.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// What to do once a resize finishes.
//
// VideoMode is the default and the only one that produces a correct picture. Resizing the OS
// window does NOT resize the engine's scene render targets -- those come from the video mode --
// so a bare window resize leaves the swapchain and the scene targets disagreeing, which renders
// as black patches. The window drag is therefore only a way to CHOOSE a size; the engine performs
// the actual change. See EngineVideoMode.h.
enum class WindowResizeCommit {
	VideoMode, // ask the engine for a real video-mode change at the new size (default)
	Auto,      // observe; if the backbuffer has not followed in time, re-assert once through SDL
	Off,       // observe and log only, never touch the window again
	Sdl,       // always re-assert the finished size through SDL immediately
};

// What holding Shift while dragging an edge does.
enum class WindowSnapMode {
	Both,       // snap the shape to a named ratio, and land exactly on a standard resolution when close
	Ratio,      // snap the shape only -- a perfect 2.39:1 at whatever size you drag to
	Resolution, // land on standard resolutions only, leave the shape alone
	Off,        // Shift does nothing
};

// Called from the hooked game WndProc (WindowInputHooks.cpp) for every message. It consumes no
// message, so it cannot change how the game sees input. The ONE thing it does write is the
// proposed rect of a WM_SIZING while Shift is held -- that is how a resize is constrained on
// Windows, and the modified rect is then passed on to SDL and DefWindowProc as usual.
//
// Runs on the game message thread, including from inside the modal resize loop, which is the
// whole point.
void WindowResize_OnWindowMessage(void* hwnd, unsigned msg, unsigned long long wParam, long long lParam);

// Game main thread, once per frame. Applies the resizable style once the window and SDL exist,
// re-checks it after a fullscreen/windowed change, and runs the "did the engine follow?" check.
void WindowResize_PumpMainThread();

void WindowResize_Shutdown();

bool WindowResize_Enabled();
void WindowResize_SetEnabled(bool enabled);

// Current client-area size of the hooked game window. Config mirrors the live value so its
// Width / Height fields report what CS2 actually renders after an SDL resize completes.
bool WindowResize_GetClientSize(int& outWidth, int& outHeight);

// Multi-line status for the console: SDL binding, window handle, current size, aspect, whether the
// engine's backbuffer agrees with the window.
std::string WindowResize_Describe();

// "mirv_filmmaker window ..." -- status | resizable | size | aspect | ratios | overlay | commit | probe.
void WindowResize_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
