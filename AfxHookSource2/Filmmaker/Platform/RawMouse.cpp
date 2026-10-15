#include "RawMouse.h"

#include "../Debug/MvmDebugLog.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace Filmmaker {
namespace {

std::atomic<long long> g_totalX{ 0 }, g_totalY{ 0 };
std::atomic<HWND> g_wnd{ nullptr };
std::atomic<int> g_state{ 0 };
std::mutex g_claimMx;
unsigned g_owners = 0;
RAWINPUTDEVICE g_saved{}; bool g_haveSaved = false; // touched only on the sink thread
constexpr UINT kRawOn = WM_APP + 41, kRawOff = WM_APP + 42;

LRESULT CALLBACK SinkProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (m == WM_INPUT) {
		alignas(8) unsigned char buf[256];
		UINT sz = sizeof(buf);
		if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
			const RAWINPUT* ri = (const RAWINPUT*)buf;
			if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
				g_totalX.fetch_add(ri->data.mouse.lLastX);
				g_totalY.fetch_add(ri->data.mouse.lLastY);
			}
		}
		return DefWindowProcW(h, m, w, l);
	}
	if (m == kRawOn) {
		// Remember the process's current mouse registration (SDL's), then take the mouse.
		g_haveSaved = false;
		UINT n = 0;
		GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
		std::vector<RAWINPUTDEVICE> cur(n);
		if (n && GetRegisteredRawInputDevices(cur.data(), &n, sizeof(RAWINPUTDEVICE)) != (UINT)-1)
			for (UINT i = 0; i < n; ++i)
				if (cur[i].usUsagePage == 1 && cur[i].usUsage == 2 && cur[i].hwndTarget != h) { g_saved = cur[i]; g_haveSaved = true; }
		RAWINPUTDEVICE rid{ 1, 2, RIDEV_INPUTSINK, h };
		const BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof(rid));
		g_state.store(ok ? 1 : -1);
		MvmDebugLog_LinefAlways("raw.mouse", "raw mouse %s (saved: %s flags=0x%x target=%p) err=%lu", ok ? "on" : "FAILED",
			g_haveSaved ? "yes" : "none", g_haveSaved ? g_saved.dwFlags : 0,
			g_haveSaved ? (void*)g_saved.hwndTarget : nullptr, ok ? 0ul : GetLastError());
		return 0;
	}
	if (m == kRawOff) {
		BOOL ok;
		if (g_haveSaved) ok = RegisterRawInputDevices(&g_saved, 1, sizeof(g_saved));
		else { RAWINPUTDEVICE rid{ 1, 2, RIDEV_REMOVE, nullptr }; ok = RegisterRawInputDevices(&rid, 1, sizeof(rid)); }
		g_state.store(0);
		MvmDebugLog_LinefAlways("raw.mouse", "raw mouse off: %s registration %s (err=%lu)", g_haveSaved ? "CS2's" : "no",
			ok ? "restored" : "NOT restored", ok ? 0ul : GetLastError());
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

// The sink window lives on its own thread so WM_INPUT is pumped whatever the game's threads do.
HWND SinkWindow() {
	static std::once_flag once;
	std::call_once(once, [] {
		std::thread([] {
			WNDCLASSW wc{};
			wc.lpfnWndProc = SinkProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"MvmRawMouseSink";
			RegisterClassW(&wc);
			HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
			g_wnd.store(h);
			MSG msg;
			while (h && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		}).detach();
	});
	return g_wnd.load();
}

} // namespace

void RawMouse_Claim(RawMouseOwner owner, bool want) {
	std::lock_guard<std::mutex> lock(g_claimMx);
	const unsigned before = g_owners;
	const unsigned after = want ? (before | owner) : (before & ~(unsigned)owner);
	if (!before == !after) { g_owners = after; return; }
	HWND h = SinkWindow();
	if (!h) return; // the sink thread is still starting: try again next frame
	g_owners = after;
	PostMessageW(h, after ? kRawOn : kRawOff, 0, 0);
}

int RawMouse_State() { return g_state.load(); }

void RawMouse_Totals(long long& x, long long& y) { x = g_totalX.load(); y = g_totalY.load(); }

} // namespace Filmmaker
