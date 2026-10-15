// See DeadlockDebug.h for why this exists.

#include "DeadlockDebug.h"

#include "DeadlockBoot.h"

#include "../ConsoleCapture.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <string>

namespace Deadlock {

namespace {

typedef void(WINAPI* ExitProcessFn_t)(UINT);
typedef BOOL(WINAPI* TerminateProcessFn_t)(HANDLE, UINT);

ExitProcessFn_t g_origExitProcess = nullptr;
TerminateProcessFn_t g_origTerminateProcess = nullptr;
bool g_installed = false;

// module+offset for an address, or "?" -- the same resolution the logger's crash handler uses,
// because "who called ExitProcess" is only useful as something you can open in a disassembler.
void Describe(void* addr, char* out, size_t cap) {
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod) && mod) {
		char full[MAX_PATH];
		const char* base = "?";
		if (GetModuleFileNameA(mod, full, MAX_PATH)) {
			base = full;
			for (const char* p = full; *p; ++p)
				if (*p == '\\' || *p == '/') base = p + 1;
		}
		wsprintfA(out, "%.40s+0x%I64x", base,
		          (unsigned long long)((unsigned char*)addr - (unsigned char*)mod));
	} else {
		wsprintfA(out, "%p", addr);
	}
	out[cap - 1] = '\0';
}

// Records the exit: the immediate caller, then up to 12 frames of stack above it. Goes to the boot
// trace first because that path allocates nothing and cannot fail on the way out, then to the
// debug log, which is stopped so the file is closed and complete rather than truncated by exit.
void RecordExit(const char* what, UINT code, void* returnAddress) {
	static long s_once = 0;
	if (InterlockedExchange(&s_once, 1)) return;

	char who[96];
	Describe(returnAddress, who, sizeof(who));

	char line[256];
	wsprintfA(line, "EXIT: %s(%u) called from %s", what, (unsigned)code, who);
	DL_BOOT(line);

	void* frames[16] = {};
	const USHORT n = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
	for (USHORT i = 0; i < n; ++i) {
		char f[96];
		Describe(frames[i], f, sizeof(f));
		wsprintfA(line, "EXIT: stack[%u] %s", (unsigned)i, f);
		DL_BOOT(line);
	}

	if (Filmmaker::MvmDebugLog_Active()) {
		Filmmaker::MvmDebugLog_LinefAlways("exit", "%s(%u) called from %s", what, (unsigned)code, who);
		for (USHORT i = 0; i < n; ++i) {
			char f[96];
			Describe(frames[i], f, sizeof(f));
			Filmmaker::MvmDebugLog_LinefAlways("exit", "stack[%u] %s", (unsigned)i, f);
		}
		Filmmaker::MvmConsoleCapture_Stop();
		Filmmaker::MvmDebugLog_Stop(nullptr, nullptr, nullptr);
	}
}

void WINAPI New_ExitProcess(UINT code) {
	RecordExit("ExitProcess", code, _ReturnAddress());
	g_origExitProcess(code);
}

BOOL WINAPI New_TerminateProcess(HANDLE process, UINT code) {
	// Only the process ending ITSELF is the event of interest. Terminating a child is not an exit.
	if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
		RecordExit("TerminateProcess", code, _ReturnAddress());
	return g_origTerminateProcess(process, code);
}

} // namespace

void Debug_Install() {
	if (g_installed) return;
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32) return;
	g_origExitProcess = (ExitProcessFn_t)GetProcAddress(k32, "ExitProcess");
	g_origTerminateProcess = (TerminateProcessFn_t)GetProcAddress(k32, "TerminateProcess");
	if (!g_origExitProcess || !g_origTerminateProcess) return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origExitProcess, New_ExitProcess);
	DetourAttach(&(PVOID&)g_origTerminateProcess, New_TerminateProcess);
	const LONG rc = DetourTransactionCommit();
	g_installed = (rc == NO_ERROR);
	DL_BOOT(g_installed ? "exit trace installed" : "exit trace FAILED to install");
}

void Debug_Command(advancedfx::ICommandArgs* args) {
	const int argc = args ? args->ArgC() : 0;
	const char* mode = (argc >= 2) ? args->ArgV(1) : "";

	// Mirrors CS2's handler so the two read the same from automation.
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Command(args);

	if (0 == _stricmp(mode, "start")) {
		std::string file;
		if (Filmmaker::MvmDebugLog_Start(&file)) {
			const bool consoleCapture = Filmmaker::MvmConsoleCapture_Start();
			Filmmaker::MvmDebugLog_Linef("command", "mvm_debug start");
			Filmmaker::MvmDebugLog_Linef("system", "target=deadlock fullConsoleCapture=%d exitTrace=%d",
			                             consoleCapture ? 1 : 0, g_installed ? 1 : 0);
			advancedfx::Message("mvm_debug: STARTED. file: %s (type 'mvm_debug stop' when done).\n",
			                    file.c_str());
		} else {
			advancedfx::Warning("mvm_debug: already running (or file could not be opened).\n");
		}
		return;
	}

	if (0 == _stricmp(mode, "stop")) {
		std::string folder, file;
		Filmmaker::MvmDebugLogStats stats;
		Filmmaker::MvmConsoleCapture_Stop();
		if (Filmmaker::MvmDebugLog_Stop(&folder, &file, &stats)) {
			advancedfx::Message("mvm_debug: STOPPED. captured %llu events; combined %llu repeats.\n",
			                    (unsigned long long)stats.eventsReceived,
			                    (unsigned long long)stats.repeatsCombined);
			advancedfx::Message("  file:   %s\n", file.c_str());
			advancedfx::Message("  folder: %s\n", folder.c_str());
		} else {
			advancedfx::Warning("mvm_debug: no debug log is running.\n");
		}
		return;
	}

	if (0 == _stricmp(mode, "status") || mode[0] == '\0') {
		const Filmmaker::MvmDebugLogStats stats = Filmmaker::MvmDebugLog_GetStats();
		advancedfx::Message(
			"mvm_debug: %s. events=%llu unique=%llu repeats_combined=%llu exitTrace=%s. "
			"Usage: mvm_debug start|stop|status\n",
			Filmmaker::MvmDebugLog_Active() ? "RUNNING" : "stopped",
			(unsigned long long)stats.eventsReceived,
			(unsigned long long)stats.uniqueEventsWritten,
			(unsigned long long)stats.repeatsCombined,
			g_installed ? "installed" : "NOT installed");
		return;
	}

	advancedfx::Warning("mvm_debug: unknown action '%s'. Usage: mvm_debug start|stop|status\n", mode);
}

void Debug_OnProcessDetach() {
	DL_BOOT("DllMain PROCESS_DETACH (process is exiting)");
	if (Filmmaker::MvmDebugLog_Active()) {
		Filmmaker::MvmDebugLog_LinefAlways("exit", "DLL_PROCESS_DETACH");
		Filmmaker::MvmConsoleCapture_Stop();
		Filmmaker::MvmDebugLog_Stop(nullptr, nullptr, nullptr);
	}
}

} // namespace Deadlock
