#pragma once

// A boot trace for the part of startup where nothing else works yet.
//
// The hook's real diagnostics -- `mirv_filmmaker health`, the mvm_debug log, the console -- all need
// tier0, a console printer, and a game that is still running. None of that is true while DllMain
// and the module dispatcher are executing, and that is exactly the window where a mistake kills the
// process outright: no window, no dump, no log, indistinguishable from the game refusing to start.
// Two separate bring-up crashes lived in that window.
//
// So this writes through raw kernel32 only -- no CRT, no allocation, no tier0, no locks of our own
// -- which is the only thing safe to call while the loader lock is held. Every line is flushed as
// it is written, because the process may not survive to the next one.
//
//   %TEMP%\afxhookdeadlock_boot.log
//
// It is always on. It costs one file append per startup milestone and it is the difference between
// diagnosing a silent death in one launch and guessing at it for six.

#include <windows.h>

namespace Deadlock {

// On by default DURING BRING-UP, and that is the right default while addresses and signatures are
// still hypotheses: the cost is one file append per startup milestone, and the alternative is
// diagnosing a silent process death with no information at all. `-mvmNoBootTrace` turns it off for
// a build that is past this stage.
inline bool& Boot_Enabled() {
	static bool enabled = true;
	return enabled;
}

inline void Boot_Line(const char* text) {
	if (!Boot_Enabled()) return;
	char path[MAX_PATH * 2];
	const DWORD n = GetTempPathA(MAX_PATH, path);
	if (!n || n > MAX_PATH) return;
	lstrcatA(path, "afxhookdeadlock_boot.log");

	HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;

	SYSTEMTIME now;
	GetLocalTime(&now);
	char stamp[64];
	wsprintfA(stamp, "[%02d:%02d:%02d.%03d tid %u] ",
	          now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentThreadId());

	DWORD written = 0;
	WriteFile(file, stamp, lstrlenA(stamp), &written, nullptr);
	WriteFile(file, text, lstrlenA(text), &written, nullptr);
	WriteFile(file, "\r\n", 2, &written, nullptr);
	CloseHandle(file);
}

// Truncates the log. Called once at the very top of DllMain, so each launch is read on its own
// rather than appended to the wreckage of the last one.
inline void Boot_Begin() {
	char path[MAX_PATH * 2];
	const DWORD n = GetTempPathA(MAX_PATH, path);
	if (!n || n > MAX_PATH) return;
	lstrcatA(path, "afxhookdeadlock_boot.log");
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

} // namespace Deadlock

#define DL_BOOT(text) ::Deadlock::Boot_Line(text)
