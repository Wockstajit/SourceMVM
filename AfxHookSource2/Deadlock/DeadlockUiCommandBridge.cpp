#include "DeadlockUiCommandBridge.h"

#include "DeadlockBoot.h"
#include "DeadlockCommands.h"
#include "DeadlockEngine.h"

#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace Deadlock {

// STX. No engine message contains it and no console font draws it, so a queued command does not
// also become a visible line of console noise.
const char* const kUiCommandMarker = "\x02MVMCMD ";

namespace {

const size_t kMarkerLength = 8; // strlen(kUiCommandMarker)

// A bound on the backlog. If the pump stops -- the main thread is blocked loading a map, say --
// a panel rendering at 60fps could otherwise queue thousands of commands that all run at once the
// moment it resumes. Dropping the oldest is the right end to drop from: the newest click is the
// one the user is waiting on.
const size_t kMaxQueued = 64;

std::mutex g_mutex;
std::deque<std::string> g_queue;

bool g_registered = false;
bool g_draining = false;

std::atomic<unsigned long long> g_queued{ 0 };
std::atomic<unsigned long long> g_executed{ 0 };
std::atomic<unsigned long long> g_dropped{ 0 };

// tier0's logging listener shape: one virtual, taking the message. Matching the layout is all that
// is required; the class itself is never handed to C++ code that knows its type.
class UiCommandListener {
public:
	virtual void Log(const void*, const char* message) {
		if (!message) return;

		// Cheapest possible rejection first. Every console line in the process passes through here,
		// including every line our own commands print, so the common case has to be one compare.
		if (message[0] != kUiCommandMarker[0]) return;
		if (0 != std::strncmp(message, kUiCommandMarker, kMarkerLength)) return;

		// Dispatching prints, and printing comes back here. The marker already makes that
		// impossible for our own output, but a command whose OUTPUT echoed the marker would
		// otherwise recurse until the stack ran out.
		if (g_draining) return;

		std::string line(message + kMarkerLength);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
		if (line.empty()) return;

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_queue.size() >= kMaxQueued) {
				g_queue.pop_front();
				g_dropped.fetch_add(1);
			}
			g_queue.push_back(std::move(line));
		}
		g_queued.fetch_add(1);

		static bool firstLine = true;
		if (firstLine) {
			firstLine = false;
			DL_BOOT("ui command bridge: first command received from Panorama");
		}
	}
};

typedef void(__fastcall* LoggingSystem_ListenerFn_t)(void* listener);

UiCommandListener g_listener;
LoggingSystem_ListenerFn_t g_unregister = nullptr;

} // namespace

bool UiCommandBridge_Start() {
	if (g_registered) return true;

	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0) return false;

	auto reg = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_RegisterLoggingListener");
	g_unregister = (LoggingSystem_ListenerFn_t)GetProcAddress(
		tier0, "LoggingSystem_UnregisterLoggingListener");
	if (!reg || !g_unregister) {
		advancedfx::Warning(
			"AfxHookDeadlock: tier0 has no logging-listener export, so Panorama buttons cannot\n"
			"reach the console. The UI will draw but do nothing.\n");
		return false;
	}

	// Registered ALONGSIDE the netcon and debug-log listeners rather than instead of them; tier0
	// supports several, which is what makes this channel free to add.
	reg(&g_listener);
	g_registered = true;
	DL_BOOT("ui command bridge: tier0 logging listener registered");
	return true;
}

void UiCommandBridge_Stop() {
	if (!g_registered || !g_unregister) return;
	g_unregister(&g_listener);
	g_registered = false;
}

bool UiCommandBridge_Active() { return g_registered; }

void UiCommandBridge_PumpMainThread() {
	if (!g_registered) return;

	for (;;) {
		std::string line;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_queue.empty()) return;
			line = std::move(g_queue.front());
			g_queue.pop_front();
		}

		g_draining = true;
		// Ours first, the engine's second. `mirv_deadlock ...` and `mvm_debug ...` are ours and
		// never reach the engine; everything else -- demo_pause, demo_gototick, demo_timescale --
		// is an engine command and goes through ExecuteClientCmd.
		if (!Commands_DispatchLine(line.c_str()))
			Engine_ExecuteClientCmd(line.c_str());
		g_draining = false;

		g_executed.fetch_add(1);

		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.ui", "cmd '%s'", line.c_str());
	}
}

unsigned long long UiCommandBridge_Queued() { return g_queued.load(); }
unsigned long long UiCommandBridge_Executed() { return g_executed.load(); }
unsigned long long UiCommandBridge_Dropped() { return g_dropped.load(); }

void UiCommandBridge_Report() {
	advancedfx::Message(
		"  ui cmds:   %s  queued=%llu executed=%llu dropped=%llu\n",
		g_registered ? "listening" : "NOT REGISTERED",
		g_queued.load(), g_executed.load(), g_dropped.load());
	if (!g_registered)
		advancedfx::Warning(
			"  every Panorama button is inert without this: Deadlock's HUD context has no\n"
			"  GameInterfaceAPI and no CitadelConCommand, so a marked $.Msg line is the only way\n"
			"  a panel can reach the console.\n");
	else if (g_queued.load() == 0)
		advancedfx::Message(
			"             nothing queued yet -- expected until a panel button is actually clicked.\n");
}

} // namespace Deadlock
