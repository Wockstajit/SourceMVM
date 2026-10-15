#include "Sdl3Window.h"

#include <windows.h>

namespace Filmmaker {

namespace {

// SDL3 C ABI. SDL_PropertiesID is Uint32; SDL_WindowFlags is Uint64; the boolean returns are C
// `bool` (one byte), which matches MSVC's C++ bool on x64.
using Fn_GetWindows          = void** (*)(int* count);
using Fn_GetWindowProperties = uint32_t (*)(void* window);
using Fn_GetPointerProperty  = void* (*)(uint32_t props, const char* name, void* defaultValue);
using Fn_GetWindowFlags      = uint64_t (*)(void* window);
using Fn_SetWindowResizable  = bool (*)(void* window, bool resizable);
using Fn_SetWindowSize       = bool (*)(void* window, int w, int h);
using Fn_GetWindowSize       = bool (*)(void* window, int* w, int* h);
using Fn_SetWindowMinimumSize = bool (*)(void* window, int w, int h);
using Fn_SyncWindow          = bool (*)(void* window);
using Fn_Free                = void (*)(void* mem);

struct Api {
	bool tried = false;
	bool ok = false;
	const char* status = "not probed yet";

	Fn_GetWindows getWindows = nullptr;
	Fn_GetWindowProperties getWindowProperties = nullptr;
	Fn_GetPointerProperty getPointerProperty = nullptr;
	Fn_GetWindowFlags getWindowFlags = nullptr;
	Fn_SetWindowResizable setWindowResizable = nullptr;
	Fn_SetWindowSize setWindowSize = nullptr;
	Fn_GetWindowSize getWindowSize = nullptr;
	Fn_SetWindowMinimumSize setWindowMinimumSize = nullptr;
	Fn_SyncWindow syncWindow = nullptr;
	Fn_Free free = nullptr;
};

Api& Get() {
	static Api s_api;
	return s_api;
}

// Resolution is retried until it succeeds: this can be called before SDL3.dll is loaded during
// startup, and a permanent "no" cached from frame 1 would disable the feature for the session.
bool Resolve() {
	Api& api = Get();
	if (api.ok)
		return true;

	HMODULE sdl = GetModuleHandleW(L"SDL3.dll");
	if (!sdl) {
		api.status = "SDL3.dll not loaded (yet)";
		return false;
	}

	api.getWindows           = (Fn_GetWindows)GetProcAddress(sdl, "SDL_GetWindows");
	api.getWindowProperties  = (Fn_GetWindowProperties)GetProcAddress(sdl, "SDL_GetWindowProperties");
	api.getPointerProperty   = (Fn_GetPointerProperty)GetProcAddress(sdl, "SDL_GetPointerProperty");
	api.getWindowFlags       = (Fn_GetWindowFlags)GetProcAddress(sdl, "SDL_GetWindowFlags");
	api.setWindowResizable   = (Fn_SetWindowResizable)GetProcAddress(sdl, "SDL_SetWindowResizable");
	api.setWindowSize        = (Fn_SetWindowSize)GetProcAddress(sdl, "SDL_SetWindowSize");
	api.getWindowSize        = (Fn_GetWindowSize)GetProcAddress(sdl, "SDL_GetWindowSize");
	api.setWindowMinimumSize = (Fn_SetWindowMinimumSize)GetProcAddress(sdl, "SDL_SetWindowMinimumSize");
	api.syncWindow           = (Fn_SyncWindow)GetProcAddress(sdl, "SDL_SyncWindow");
	api.free                 = (Fn_Free)GetProcAddress(sdl, "SDL_free");

	api.ok = api.getWindows && api.getWindowProperties && api.getPointerProperty
		&& api.getWindowFlags && api.setWindowResizable && api.setWindowSize
		&& api.getWindowSize && api.syncWindow && api.free;

	// setWindowMinimumSize is optional -- everything else works without it.
	api.status = api.ok ? "SDL3 window API resolved" : "SDL3.dll loaded but an export is missing";
	api.tried = true;
	return api.ok;
}

} // namespace

bool Sdl3_Available() {
	return Resolve();
}

const char* Sdl3_StatusText() {
	Resolve();
	return Get().status;
}

Sdl3Window* Sdl3_WindowForHwnd(void* hwnd) {
	if (!hwnd || !Resolve())
		return nullptr;

	// The game has exactly one window for the whole session, so a single-entry cache removes the
	// SDL_GetWindows allocation from the per-frame path.
	static void* s_cachedHwnd = nullptr;
	static void* s_cachedWindow = nullptr;
	if (s_cachedHwnd == hwnd && s_cachedWindow)
		return s_cachedWindow;

	Api& api = Get();
	int count = 0;
	void** windows = api.getWindows(&count);
	if (!windows)
		return nullptr;

	void* found = nullptr;
	for (int i = 0; i < count && !found; ++i) {
		void* window = windows[i];
		if (!window)
			continue;
		const uint32_t props = api.getWindowProperties(window);
		if (!props)
			continue;
		// SDL_PROP_WINDOW_WIN32_HWND_POINTER
		void* native = api.getPointerProperty(props, "SDL.window.win32.hwnd", nullptr);
		if (native == hwnd)
			found = window;
	}
	api.free(windows);

	if (found) {
		s_cachedHwnd = hwnd;
		s_cachedWindow = found;
	}
	return found;
}

uint64_t Sdl3_GetWindowFlags(Sdl3Window* window) {
	if (!window || !Resolve())
		return 0;
	return Get().getWindowFlags(window);
}

bool Sdl3_SetWindowResizable(Sdl3Window* window, bool resizable) {
	if (!window || !Resolve())
		return false;
	return Get().setWindowResizable(window, resizable);
}

bool Sdl3_SetWindowSize(Sdl3Window* window, int width, int height) {
	if (!window || !Resolve() || width <= 0 || height <= 0)
		return false;
	return Get().setWindowSize(window, width, height);
}

bool Sdl3_GetWindowSize(Sdl3Window* window, int& outWidth, int& outHeight) {
	outWidth = 0;
	outHeight = 0;
	if (!window || !Resolve())
		return false;
	return Get().getWindowSize(window, &outWidth, &outHeight);
}

bool Sdl3_SetWindowMinimumSize(Sdl3Window* window, int width, int height) {
	if (!window || !Resolve() || !Get().setWindowMinimumSize)
		return false;
	return Get().setWindowMinimumSize(window, width, height);
}

bool Sdl3_SyncWindow(Sdl3Window* window) {
	if (!window || !Resolve())
		return false;
	return Get().syncWindow(window);
}

} // namespace Filmmaker
