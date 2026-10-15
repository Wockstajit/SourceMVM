#include "DeadlockMouseWheel.h"
#include "../../Shared/AfxConsole.h"
#include <windows.h>
#include <atomic>

namespace Deadlock {
namespace {
std::atomic<bool> g_started{false};
std::atomic<bool> g_ready{false};
std::atomic<DWORD> g_error{0};
std::atomic<long> g_units{0};
std::atomic<unsigned long long> g_events{0};
int g_remainder = 0;

// Observe wheel input without registering a second raw mouse target in the game process.
// Windows permits only one such target per device class; registering ours displaced SDL's.
LRESULT CALLBACK WheelHook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && message == WM_MOUSEWHEEL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) {
            const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            g_units.fetch_add(static_cast<SHORT>(HIWORD(event->mouseData)));
            g_events.fetch_add(1);
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
DWORD WINAPI WheelThread(void*) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&WheelHook), &module);
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, WheelHook, module, 0);
    if (!hook) { g_error.store(GetLastError()); return 0; }
    g_ready.store(true);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook);
    g_ready.store(false);
    return 0;
}
}
void MouseWheel_Initialize() {
    if (g_started.exchange(true)) return;
    HANDLE thread = CreateThread(nullptr, 0, WheelThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else g_error.store(GetLastError());
}
void MouseWheel_PumpMessages() {} // Dedicated thread keeps the observer responsive during seeks.
int MouseWheel_ConsumeDelta() {
    const int units = g_remainder + g_units.exchange(0);
    const int notches = units / WHEEL_DELTA;
    g_remainder = units - notches * WHEEL_DELTA;
    return notches;
}
void MouseWheel_Report() {
    advancedfx::Message("  mousewheel: %s events=%llu pendingUnits=%ld error=%lu\n",
        g_ready.load() ? "listening" : "not ready", g_events.load(), g_units.load(), g_error.load());
}
}
