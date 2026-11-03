#include "stdafx.h"

#include "SourceConsole.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/NetconServer.h>

#include <windows.h>

#include <atomic>

namespace SourceFilmmaker {

namespace {

// tier0's spew contract (dbg.h). The return value tells tier0 whether to break, continue or abort.
enum SpewType { kSpewMessage = 0, kSpewWarning, kSpewAssert, kSpewError, kSpewLog };
typedef int (*SpewOutputFunc_t)(int spewType, const char* message);
typedef void (*SetSpewOutputFunc_t)(SpewOutputFunc_t func);
typedef SpewOutputFunc_t (*GetSpewOutputFunc_t)();
const int kSpewContinue = 1;

SpewOutputFunc_t g_previousSpew = nullptr;
GetSpewOutputFunc_t g_getSpew = nullptr;
bool g_installed = false;

std::atomic<bool> g_netcon{ false };
std::atomic<bool> g_debugLog{ false };
std::atomic<ConsoleLineObserver> g_observer{ nullptr };

const char* LevelName(int spewType) {
	switch (spewType) {
	case kSpewWarning: return "warning";
	case kSpewAssert: return "assert";
	case kSpewError: return "error";
	case kSpewLog: return "log";
	default: return "message";
	}
}

int ConsoleSpew(int spewType, const char* message) {
	if (message) {
		if (g_netcon.load(std::memory_order_relaxed))
			advancedfx::NetconServer::Write(message);
		if (g_debugLog.load(std::memory_order_relaxed))
			Filmmaker::MvmDebugLog_ConsoleLine(LevelName(spewType), -1, message);
		if (ConsoleLineObserver observer = g_observer.load(std::memory_order_relaxed)) observer(message);
	}
	return g_previousSpew ? g_previousSpew(spewType, message) : kSpewContinue;
}

} // namespace

bool Console_Install() {
	if (g_installed) return true;
	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0) return false;
	auto setSpew = (SetSpewOutputFunc_t)GetProcAddress(tier0, "SpewOutputFunc");
	g_getSpew = (GetSpewOutputFunc_t)GetProcAddress(tier0, "GetSpewOutputFunc");
	if (!setSpew || !g_getSpew) return false;
	g_previousSpew = g_getSpew();
	setSpew(&ConsoleSpew);
	g_installed = true;
	return true;
}

bool Console_Captured() {
	return g_getSpew && g_getSpew() == &ConsoleSpew;
}

void Console_SetNetconEnabled(bool enabled) { g_netcon.store(enabled); }

void Console_SetDebugLogEnabled(bool enabled) { g_debugLog.store(enabled); }
bool Console_DebugLogEnabled() { return g_debugLog.load(); }

void Console_SetLineObserver(ConsoleLineObserver observer) { g_observer.store(observer); }

} // namespace SourceFilmmaker
