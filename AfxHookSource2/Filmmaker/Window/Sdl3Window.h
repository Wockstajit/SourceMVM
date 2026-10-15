#pragma once

// Thin by-name binding to the SDL3 that CS2 already has loaded.
//
// WHY BY NAME AND NOT A SIGNATURE SCAN: CS2 ships SDL3.dll with a normal export table, so every
// call below survives a CS2 update untouched -- there is no pattern to re-find on update day. That
// makes this one of the very few engine-facing surfaces in this project with no version coupling
// at all, and it is the reason the window work goes through SDL rather than raw Win32.
//
// WHY SDL AND NOT SetWindowLongPtr/SetWindowPos DIRECTLY: Docs/agent/render-system-contract.md
// records that a raw SetWindowPos "succeeds, then CS2/SDL restores the configured size". SDL owns
// the window; a style or size change it does not know about is bookkeeping it will overwrite.
// Going through SDL_SetWindowResizable / SDL_SetWindowSize updates SDL's own record of the window
// AND makes it emit the SDL_EVENT_WINDOW_RESIZED / PIXEL_SIZE_CHANGED events the engine listens
// for (inputsystem.dll references both by name).
//
// THREADING: SDL's window functions are main-thread only. Every entry point here must be called
// from the game main thread (Filmmaker::RunMainThreadFrame), never from the render thread and
// never from inside a modal resize loop -- see WindowResize.cpp, which defers accordingly.

#include <cstdint>

namespace Filmmaker {

// SDL_Window* as an opaque pointer so no SDL headers are needed anywhere in this project.
using Sdl3Window = void;

// SDL_WindowFlags bits we care about (SDL3 values; SDL_WindowFlags is Uint64 in SDL3).
inline constexpr uint64_t kSdlWindowFullscreen = 0x0000000000000001ull;
inline constexpr uint64_t kSdlWindowResizable  = 0x0000000000000020ull;
inline constexpr uint64_t kSdlWindowBorderless = 0x0000000000000010ull;

// True once SDL3.dll is loaded and every function this module needs resolved. Cheap after the
// first call; safe to poll every frame while waiting for SDL to appear during startup.
bool Sdl3_Available();

// One-line description of what did or did not resolve, for `window status` and the log.
const char* Sdl3_StatusText();

// Finds the SDL_Window whose native handle is `hwnd` by walking SDL_GetWindows() and comparing the
// SDL.window.win32.hwnd property. Returns nullptr when SDL is unavailable or no window matches.
// Result is cached per HWND, so this is cheap to call every frame.
Sdl3Window* Sdl3_WindowForHwnd(void* hwnd);

uint64_t Sdl3_GetWindowFlags(Sdl3Window* window);
bool Sdl3_SetWindowResizable(Sdl3Window* window, bool resizable);
bool Sdl3_SetWindowSize(Sdl3Window* window, int width, int height);
bool Sdl3_GetWindowSize(Sdl3Window* window, int& outWidth, int& outHeight);
bool Sdl3_SetWindowMinimumSize(Sdl3Window* window, int width, int height);

// Blocks until the window manager has caught up with pending size/position requests. Needed
// because SDL_SetWindowSize is asynchronous on Windows; without it a size read straight after a
// set can still report the old size.
bool Sdl3_SyncWindow(Sdl3Window* window);

} // namespace Filmmaker
