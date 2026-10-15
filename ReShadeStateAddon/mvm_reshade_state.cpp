// SOURCE:MVM ReShade state addon.
//
// One job: tell AfxHookSource2 whether the ReShade overlay is open, from inside ReShade itself.
// The `reshade_open_overlay` addon event fires exactly on open and on close (a LEVEL signal,
// never blocked here -- the callback always returns false so it cannot alter ReShade's
// behaviour). The per-frame `reshade_overlay` draw event was tried first and measured WRONG for
// this purpose: it also fires for ReShade's always-on widgets (FPS counter), reading as
// "overlay open" during normal play.
//
// WHY AN ADDON. ReShade exports no is-the-overlay-open query, and mirroring the overlay toggle
// key from the game's WndProc was measured unreliable (double/missed toggles desynced the
// mirror, 2026-08-31). This is the ground truth; AfxHookSource2's UiInputGate reads the
// exported flag via GetProcAddress and stands its director input taps down while it is set.
//
// Standalone on purpose: builds with build.ps1 in this folder, ships as
// mvm_reshade_state.addon64 beside ReShade (game/bin/win64), and depends on nothing else in
// this repo.

#include <Windows.h>

#include "include/reshade.hpp"

static volatile LONG g_overlayOpen = 0;

static bool OnReShadeOpenOverlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source) {
	InterlockedExchange(&g_overlayOpen, open ? 1 : 0);
	return false; // never veto the state change
}

extern "C" __declspec(dllexport) const char* NAME = "SOURCE:MVM overlay state";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
	"Reports whether the ReShade overlay is open to SOURCE:MVM (input gating).";

// Read by AfxHookSource2 (UiInputGate) via GetProcAddress: nonzero while the overlay is open.
extern "C" __declspec(dllexport) unsigned long long MvmReShadeOverlayOpen() {
	return (unsigned long long)g_overlayOpen;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OnReShadeOpenOverlay);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
