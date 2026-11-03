#include "stdafx.h"

#include "UiInputGate.h"

#include "Filmmaker/Debug/MvmDebugLog.h"

#include <Windows.h>

#include <atomic>

bool AfxConsoleVisible(); // main.cpp: the engine's IGameUIService::Con_IsVisible

namespace {

// --- CS2 pause/main menu ---------------------------------------------------------------------
// Level supplied by the menu pump from CS2's own Panorama show/hide events. The staleness
// guard releases the gate if polling stops (menu JS context torn down while the flag was
// open), so a dead context can never wedge input off.
std::atomic<bool> g_pauseMenuOpen{false};
std::atomic<unsigned long long> g_lastMenuSampleMs{0};
constexpr unsigned long long kMenuStaleMs = 1000;

// --- ReShade overlay -------------------------------------------------------------------------
// Ground truth comes from our own ReShade addon (ReShadeStateAddon/): it tracks the
// `reshade_open_overlay` open/close event and exports the level. The key mirror below is only
// the FALLBACK when the addon is not installed -- mirroring the toggle key was measured
// unreliable (double/missed toggles desynced it, 2026-08-31).
//
// ReShade loads and frees its add-ons itself (at the latest while CS2 quits), so no pointer into
// the addon may outlive one call: a cached export called after ReShade freed the addon crashed in
// SDL's SetCursor during quit (crash report 1565165641030439023). Each query looks the addon up
// again and takes NO loader reference. Any reference of ours, even for one call, can be the one
// that makes ReShade's FreeLibrary skip the addon's detach; ReShade then edits its registration
// record while our FreeLibrary runs that detach on another thread. If ReShade frees the addon
// between the lookup and the call, the call faults and the SEH guard reads it as "not loaded".
typedef unsigned long long (*MvmReShadeOverlayOpen_t)();

std::atomic<bool> g_addonSeen{false};

int CallOverlayOpen(MvmReShadeOverlayOpen_t fn) {
	__try {
		return fn() != 0 ? 1 : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// 1 = open, 0 = closed, -1 = the addon is not loaded right now.
int QueryReShadeAddonOverlay() {
	HMODULE m = nullptr;
	int open = -1;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"mvm_reshade_state.addon64", &m) && m) {
		if (auto fn = (MvmReShadeOverlayOpen_t)GetProcAddress(m, "MvmReShadeOverlayOpen"))
			open = CallOverlayOpen(fn);
	}
	if (g_addonSeen.exchange(open >= 0, std::memory_order_relaxed) != (open >= 0) && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("input.capture", "reshade state addon %s", open >= 0 ? "loaded" : "gone (key mirror fallback)");
	return open;
}

std::atomic<bool> g_overlayOpen{false};
std::atomic<int> g_toggleVk{-1}; // -1 = not initialised, 0 = disabled (no ReShade present)

// Only arm when a ReShade module is actually in the process. It loads as a d3d11.dll proxy
// here, so the module NAME is ambiguous -- probe for a ReShade-specific export instead. Then
// read the overlay toggle key from ReShade.ini beside the exe ([INPUT] KeyOverlay=vk,...).
int ResolveToggleVk() {
	const wchar_t* const candidates[] = {L"d3d11.dll", L"dxgi.dll", L"ReShade64.dll"};
	bool reshadePresent = false;
	for (const wchar_t* name : candidates) {
		if (HMODULE m = GetModuleHandleW(name)) {
			if (GetProcAddress(m, "ReShadeRegisterAddon")) { reshadePresent = true; break; }
		}
	}
	if (!reshadePresent) return 0;

	wchar_t ini[MAX_PATH] = L"";
	GetModuleFileNameW(nullptr, ini, MAX_PATH);
	if (wchar_t* slash = wcsrchr(ini, L'\\')) *slash = 0;
	wcscat_s(ini, L"\\ReShade.ini");
	wchar_t value[64] = L"";
	GetPrivateProfileStringW(L"INPUT", L"KeyOverlay", L"36", value, 64, ini);
	const int vk = _wtoi(value); // first comma-separated field; _wtoi stops at the comma
	return vk > 0 ? vk : 36;     // VK_HOME is ReShade's default
}

} // namespace

void Filmmaker::UiInputGate_SupplyPauseMenuOpen(bool open) {
	if (open != g_pauseMenuOpen.load(std::memory_order_relaxed) && MvmDebugLog_Active())
		MvmDebugLog_Linef("input.capture", "cs2 pause/main menu %s -- director taps %s",
			open ? "OPEN" : "closed", open ? "standing down" : "active");
	g_pauseMenuOpen.store(open, std::memory_order_relaxed);
	g_lastMenuSampleMs.store(GetTickCount64(), std::memory_order_relaxed);
}

bool Filmmaker::UiInputGate_Active() {
	// The developer console: its scroll, clicks and typing are its own (user report 2026-10-18:
	// the wheel over an open console moved the camera instead of scrolling the console).
	if (AfxConsoleVisible())
		return true;
	const int addonOverlay = QueryReShadeAddonOverlay();
	if (addonOverlay >= 0) {
		if (addonOverlay != 0)
			return true;
	} else if (g_overlayOpen.load(std::memory_order_relaxed)) {
		return true; // key-mirror fallback, only without the state addon
	}
	if (g_pauseMenuOpen.load(std::memory_order_relaxed)) {
		const unsigned long long stamp = g_lastMenuSampleMs.load(std::memory_order_relaxed);
		if (stamp != 0 && (GetTickCount64() - stamp) < kMenuStaleMs)
			return true;
	}
	return false;
}

void Filmmaker::UiInputGate_OnKeyDown(unsigned vk, long long lParam) {
	int toggleVk = g_toggleVk.load(std::memory_order_relaxed);
	if (toggleVk < 0) {
		toggleVk = ResolveToggleVk();
		g_toggleVk.store(toggleVk, std::memory_order_relaxed);
	}
	if (toggleVk == 0 || (int)vk != toggleVk) return;
	if (lParam & (1ll << 30)) return; // autorepeat -- only the initial press toggles

	const bool open = !g_overlayOpen.load(std::memory_order_relaxed);
	g_overlayOpen.store(open, std::memory_order_relaxed);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("input.capture", "reshade overlay believed %s (vk=%d)",
			open ? "OPEN -- director taps standing down" : "closed", toggleVk);
}
