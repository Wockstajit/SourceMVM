#include "CrashReport.h"
#include "CrashReportMath.h"

#include "BugReport.h"
#include "BugReportMath.h"
#include "BugReportSend.h"
#include "../Demo/PlayingDemoPath.h"          // ResolvePlayingDemoPath
#include "../Environment/EnvironmentScene.h"  // CurrentMapName
#include "../Debug/MvmDebugLog.h"
#include "../Platform/TextEncoding.h"

#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <DbgHelp.h>  // MiniDumpWriteDump types (resolved at runtime from CS2's own dbghelp.dll)
#include <TlHelp32.h> // module snapshot: ReShade, add-ons and overlays named in reports
#include <intrin.h>   // __fastfail

#include "../../../Deps/release/Detours/src/detours.h" // after Windows.h: it reads the target arch from it

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace Filmmaker {

namespace {

using namespace CrashReport;

// ---- State the crash handler reads. Fixed buffers: it must not allocate. ------------------------

wchar_t g_dir[MAX_PATH] = {};       // ...\HLAE\bugreports\ (trailing backslash)
char g_map[128] = {};
char g_demo[260] = {};
char g_modules[1024] = {};          // third-party graphics code in the process (NotableModule)
double g_worstP95 = 0;              // worst heartbeat p95 frame time once it was slow (ms), else 0
DWORD g_mainThreadId = 0;
ULONGLONG g_sessionStartTick = 0;

std::atomic<bool> g_handlersInstalled{false};
std::atomic<bool> g_crashing{false};
std::atomic<bool> g_sessionStarted{false};

HANDLE g_sessionLog = INVALID_HANDLE_VALUE;
size_t g_sessionLogBytes = 0;

// Filled by the handler, read by its worker thread.
char g_header[kHeaderBytes] = {};
char g_summary[512] = {};
char g_base[96] = {};
CONTEXT g_walkContext = {};
CONTEXT g_captureContext = {};
// The crashing thread's exception, copied for the worker's minidump (the originals live on a stack
// the worker cannot rely on). g_dumpHasException is false for an abort or a test.
EXCEPTION_RECORD g_dumpRecord = {};
CONTEXT g_dumpContext = {};
EXCEPTION_POINTERS g_dumpPointers = {};
DWORD g_dumpThreadId = 0;
bool g_dumpHasException = false;

// CS2 catches most of its own crashes (SEH in the engine), writes cs2_*.mdmp and terminates: no
// top-level filter ever runs. Its dump writer is the reliable crash signal, and the comment it
// attaches (fatal error text, demo and tick, memory) goes into our report.
using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
	PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDump_t g_origMiniDumpWriteDump = nullptr;
char g_engineComment[6144] = {};
// "AfxHookSource2.dll/5F2A1B3C1a2b000": this DLL's symbol-store key, filled once at session start
// so the crash header can name the exact build without the handler touching the PE header.
char g_hookId[96] = {};
char g_hookModule[64] = {};

LPTOP_LEVEL_EXCEPTION_FILTER g_chainedFilter = nullptr;
using SetUnhandledExceptionFilter_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetUnhandledExceptionFilter_t g_origSetFilter = nullptr;
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
TerminateProcess_t g_origTerminateProcess = nullptr;
_invalid_parameter_handler g_chainedInvalidParameter = nullptr;

// What happened to the last crash or pending send, for "community crash status".
std::mutex g_statusMutex;
std::string g_lastSend = "nothing sent this session";

// Reports waiting for the player's decision (main menu prompt), refreshed off the main thread's
// hot path. Guarded by g_statusMutex.
CrashReportPending g_pending;

void SetStatus(const std::string& s) {
	std::lock_guard<std::mutex> lock(g_statusMutex);
	g_lastSend = s;
}

void CopyText(char* dst, size_t cap, const std::string& src) {
	const size_t n = (std::min)(cap - 1, src.size());
	memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

std::wstring Path(const wchar_t* name) { return std::wstring(g_dir) + name; }
std::wstring Path(const std::string& name) { return std::wstring(g_dir) + Utf8ToWide(name); }

// ---- Heap-free helpers for the handler -----------------------------------------------------------

void WriteWholeFile(const wchar_t* path, const char* data, size_t len) {
	HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(f, data, DWORD(len), &written, nullptr);
	CloseHandle(f);
}

// "engine2.dll+0x35630" for an address.
void Describe(uint64_t address, char* out, size_t cap) {
	HMODULE module = nullptr;
	char name[MAX_PATH] = {};
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCWSTR>(address), &module) && module) {
		wchar_t wide[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(module, wide, MAX_PATH);
		const wchar_t* base = wide;
		for (DWORD i = 0; i < n; ++i)
			if (wide[i] == L'\\' || wide[i] == L'/') base = wide + i + 1;
		WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
	}
	FormatFrame(out, cap, name, address, reinterpret_cast<uint64_t>(module));
}

// Unwinds from `start` with the x64 unwind tables. Guarded: a broken stack ends the walk.
int WalkStack(const CONTEXT* start, uint64_t* frames, int maxFrames) {
	g_walkContext = *start;
	CONTEXT& c = g_walkContext;
	int n = 0;
	__try {
		while (n < maxFrames && c.Rip) {
			frames[n++] = c.Rip;
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
			if (fn) {
				PVOID handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
			} else {
				// A leaf function: the return address is on top of the stack.
				c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
				c.Rsp += 8;
			}
			if (!c.Rsp) break;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return n;
}

// A small minidump of this process beside the report: every thread's stack and registers, the
// module list and the exception, so a crash opens in a debugger at the faulting function and line
// (against the archived PDB). No heap or full memory. Sent only with the report, when the player
// presses Send.
void WriteCrashDump() {
	MiniDumpWriteDump_t write = g_origMiniDumpWriteDump; // CS2's dbghelp, already loaded at session start
	if (!write) return;
	wchar_t path[MAX_PATH + 96];
	swprintf(path, sizeof(path) / sizeof(path[0]), L"%s%hs.dmp", g_dir, g_base);
	const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	MINIDUMP_EXCEPTION_INFORMATION info = {g_dumpThreadId, &g_dumpPointers, FALSE};
	const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
	const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type,
		g_dumpHasException ? &info : nullptr, nullptr, nullptr);
	CloseHandle(file);
	if (!ok) DeleteFileW(path);
}

// The full report, on its own thread: the crashed one may hold locks or have a broken stack.
// Nothing is sent from here. The .pending marker stays, and the next launch asks the player.
DWORD WINAPI CrashWorker(void*) {
	BugReportCrash crash;
	crash.summary = g_summary;
	crash.details = g_header;
	crash.baseName = g_base;
	crash.map = g_map;
	crash.demo = g_demo;
	BugReport_Write("", BugReportSender{}, &crash);
	WriteCrashDump();
	return 0;
}

// Everything a crash does. `what` names the exception; `record` may be null (abort, invalid
// parameter). Runs once per process.
void HandleCrash(const char* what, uint32_t code, const CONTEXT* context, const EXCEPTION_RECORD* record) {
	if (!g_dir[0]) return; // the session never started: nowhere to write

	SYSTEMTIME st;
	GetLocalTime(&st);
	BaseName(g_base, sizeof(g_base), "crash", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	g_dumpThreadId = GetCurrentThreadId();
	g_dumpHasException = record && context;
	if (g_dumpHasException) {
		g_dumpRecord = *record;
		g_dumpContext = *context;
		g_dumpPointers = {&g_dumpRecord, &g_dumpContext};
	}

	char at[192] = "(unknown)";
	uint64_t frames[kMaxFrames] = {};
	const int frameCount = context ? WalkStack(context, frames, kMaxFrames) : 0;
	const uint64_t faultAddress = record ? reinterpret_cast<uint64_t>(record->ExceptionAddress)
	                                     : (frameCount ? frames[0] : 0);
	if (faultAddress) Describe(faultAddress, at, sizeof(at));

	char line[512];
	size_t len = 0;
	g_header[0] = '\0';
	// A CS2 fatal error carries its own text ("OUT OF MEMORY", an assert): that is the headline.
	char abortText[320] = {};
	if (const char* a = strstr(g_engineComment, "AbortMessage({")) {
		a += strlen("AbortMessage({");
		size_t n = 0;
		while (a[n] && !(a[n] == '}' && a[n + 1] == ')') && n < sizeof(abortText) - 1) {
			abortText[n] = (a[n] == '\n' || a[n] == '\r') ? ' ' : a[n];
			++n;
		}
	}
	if (abortText[0]) snprintf(g_summary, sizeof(g_summary), "CS2 fatal error: %s", abortText);
	else snprintf(g_summary, sizeof(g_summary), "%s at %s", what, at);
	snprintf(line, sizeof(line), "Summary   : %s\n", g_summary);
	len = Append(g_header, sizeof(g_header), len, line);
	if (code) {
		snprintf(line, sizeof(line), "Exception : 0x%08X %s", code, ExceptionName(code));
		len = Append(g_header, sizeof(g_header), len, line);
		if (record && code == 0xC0000005u && record->NumberParameters >= 2) {
			snprintf(line, sizeof(line), " (%s of 0x%016llx)", AccessKind(record->ExceptionInformation[0]),
			         (unsigned long long)record->ExceptionInformation[1]);
			len = Append(g_header, sizeof(g_header), len, line);
		}
		len = Append(g_header, sizeof(g_header), len, "\n");
	}
	snprintf(line, sizeof(line), "At        : %s\n", at);
	len = Append(g_header, sizeof(g_header), len, line);
	if (g_hookId[0]) {
		snprintf(line, sizeof(line), "Hook ID   : %s\n", g_hookId);
		len = Append(g_header, sizeof(g_header), len, line);
	}
	const DWORD tid = GetCurrentThreadId();
	snprintf(line, sizeof(line), "Thread    : %lu%s\n", tid, tid == g_mainThreadId ? " (main thread)" : "");
	len = Append(g_header, sizeof(g_header), len, line);
	snprintf(line, sizeof(line), "Uptime    : %llu s\n",
	         (unsigned long long)(g_sessionStartTick ? (GetTickCount64() - g_sessionStartTick) / 1000 : 0));
	len = Append(g_header, sizeof(g_header), len, line);
	snprintf(line, sizeof(line), "Map       : %s\nDemo      : %s\n",
	         g_map[0] ? g_map : "(unknown)", g_demo[0] ? g_demo : "(none playing)");
	len = Append(g_header, sizeof(g_header), len, line);
	len = Append(g_header, sizeof(g_header), len, "Injected  : ");
	len = Append(g_header, sizeof(g_header), len, g_modules[0] ? g_modules : "(none found)");
	len = Append(g_header, sizeof(g_header), len, "\n");
	len = Append(g_header, sizeof(g_header), len, "Stack (module+offset, innermost first):\n");
	// A fault inside CS2 or a driver usually passed through our code first; that frame says whose
	// bug it is, so it gets its own line when the faulting frame is not already ours.
	int firstOwn = -1;
	char firstOwnFrame[192] = {};
	for (int i = 0; i < frameCount; ++i) {
		char frame[192];
		Describe(frames[i], frame, sizeof(frame));
		snprintf(line, sizeof(line), "  #%02d %s\n", i, frame);
		len = Append(g_header, sizeof(g_header), len, line);
		if (firstOwn < 0 && FrameInModule(frame, g_hookModule)) {
			firstOwn = i;
			strcpy_s(firstOwnFrame, frame);
		}
	}
	if (firstOwn > 0) {
		snprintf(line, sizeof(line), "First SOURCE:MVM frame: #%02d %s\n", firstOwn, firstOwnFrame);
		len = Append(g_header, sizeof(g_header), len, line);
	}
	if (!frameCount) len = Append(g_header, sizeof(g_header), len, "  (no stack)\n");
	if (g_engineComment[0]) {
		len = Append(g_header, sizeof(g_header), len, "CS2's own crash note (from its minidump comment):\n");
		len = Append(g_header, sizeof(g_header), len, g_engineComment);
		len = Append(g_header, sizeof(g_header), len, "\n");
	}

	// 1. The header alone: this much survives whatever happens next.
	wchar_t pending[MAX_PATH + 96];
	swprintf(pending, sizeof(pending) / sizeof(pending[0]), L"%s%hs.pending", g_dir, g_base);
	WriteWholeFile(pending, g_header, len);

	// 2. The session ends here: keep its log next to the header, release the lock so the next launch
	//    does not also call this an unexpected exit.
	const HANDLE log = g_sessionLog;
	g_sessionLog = INVALID_HANDLE_VALUE;
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	wchar_t from[MAX_PATH + 32], to[MAX_PATH + 96];
	swprintf(from, sizeof(from) / sizeof(from[0]), L"%ssession.log", g_dir);
	swprintf(to, sizeof(to) / sizeof(to[0]), L"%s%hs.session.log", g_dir, g_base);
	MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
	swprintf(from, sizeof(from) / sizeof(from[0]), L"%ssession.lock", g_dir);
	DeleteFileW(from);

	// 3. Full report, bounded: a wedged process still dies, and the next launch builds it instead.
	if (HANDLE worker = CreateThread(nullptr, 0, CrashWorker, nullptr, 0, nullptr)) {
		WaitForSingleObject(worker, 25000);
		CloseHandle(worker);
	}
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
	if (!g_crashing.exchange(true) && ep && ep->ExceptionRecord) {
		const uint32_t code = ep->ExceptionRecord->ExceptionCode;
		HandleCrash(ExceptionName(code), code, ep->ContextRecord, ep->ExceptionRecord);
	}
	return g_chainedFilter ? g_chainedFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// CS2's crash handler writing its dump: the crash it caught itself. Its dump is written first
// (untouched), then ours. A dump of another process is not a crash of ours.
BOOL WINAPI New_MiniDumpWriteDump(HANDLE process, DWORD pid, HANDLE file, MINIDUMP_TYPE type,
	PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION user,
	PMINIDUMP_CALLBACK_INFORMATION callback) {
	const BOOL written = g_origMiniDumpWriteDump(process, pid, file, type, exception, user, callback);
	if (pid != GetCurrentProcessId() || g_crashing.exchange(true)) return written;

	// The comment CS2 attaches: fatal error text, demo and tick, memory. Text up to the ConVar dump.
	size_t len = 0;
	g_engineComment[0] = '\0';
	for (ULONG i = 0; user && i < user->UserStreamCount; ++i) {
		const MINIDUMP_USER_STREAM& s = user->UserStreamArray[i];
		if (s.Type != CommentStreamA || !s.Buffer || !s.BufferSize) continue;
		const char* text = static_cast<const char*>(s.Buffer);
		const char* end = text + s.BufferSize;
		if (const char* cut = strstr(text, "ConVars (non-default)")) end = (std::min)(end, cut);
		const size_t n = (std::min)(size_t(end - text), sizeof(g_engineComment) - 1 - len);
		memcpy(g_engineComment + len, text, n);
		len += n;
		g_engineComment[len] = '\0';
	}

	// Same-process exception pointers are directly readable; a "_error" dump may have none.
	const EXCEPTION_POINTERS* ep = (exception && !exception->ClientPointers) ? exception->ExceptionPointers : nullptr;
	if (ep && ep->ExceptionRecord) {
		const uint32_t code = ep->ExceptionRecord->ExceptionCode;
		HandleCrash(ExceptionName(code), code, ep->ContextRecord, ep->ExceptionRecord);
	} else {
		RtlCaptureContext(&g_captureContext);
		HandleCrash("CS2 crash handler (no exception record)", 0, &g_captureContext, nullptr);
	}
	return written;
}

// Hooks the dbghelp.dll CS2 ships in game\bin\win64 (the one its crash handler loads).
void InstallDumpWriterHook() {
	if (g_origMiniDumpWriteDump) return;
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring path = exe;
	path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"dbghelp.dll";
	HMODULE dbghelp = LoadLibraryW(path.c_str());
	if (!dbghelp) dbghelp = LoadLibraryW(L"dbghelp.dll");
	g_origMiniDumpWriteDump = dbghelp ? (MiniDumpWriteDump_t)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
	if (!g_origMiniDumpWriteDump) return;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_origMiniDumpWriteDump, New_MiniDumpWriteDump);
	if (DetourTransactionCommit() != NO_ERROR) g_origMiniDumpWriteDump = nullptr;
}

// CS2 (or a library) installing its own top-level filter keeps ours on top: theirs is chained.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI New_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
	if (filter == OnUnhandledException) return g_origSetFilter(filter);
	const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_chainedFilter;
	g_chainedFilter = filter;
	return previous;
}

// abort(), std::terminate and pure virtual calls end here. Windows reports these as 0xC0000409 in
// ucrtbase.dll, and no exception filter ever sees them.
void __cdecl OnAbortSignal(int) {
	if (g_crashing.exchange(true)) return;
	RtlCaptureContext(&g_captureContext);
	HandleCrash("abort() (SIGABRT)", 0, &g_captureContext, nullptr);
}

void __cdecl OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                unsigned int line, uintptr_t reserved) {
	if (!g_crashing.exchange(true)) {
		RtlCaptureContext(&g_captureContext);
		HandleCrash("CRT invalid parameter", 0, &g_captureContext, nullptr);
	}
	// Behave exactly as before: the previous handler, or the CRT's own fast-fail.
	if (g_chainedInvalidParameter) g_chainedInvalidParameter(expression, function, file, line, reserved);
	__fastfail(FAST_FAIL_INVALID_ARG);
}

// A session that ends itself (quit paths that skip ExitProcess) is not an unexpected exit.
BOOL WINAPI New_TerminateProcess(HANDLE process, UINT code) {
	if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) CrashReport_CleanExit();
	return g_origTerminateProcess(process, code);
}

// ---- Session, pending reports ---------------------------------------------------------------------

std::string ReadText(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

// "Label     : value" from a crash header or report.
std::string Field(const std::string& text, const std::string& label) {
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (line.rfind(label, 0) != 0) continue;
		const size_t colon = line.find(':', label.size());
		if (colon == std::string::npos) continue;
		std::string value = line.substr(colon + 1);
		while (!value.empty() && (value.front() == ' ')) value.erase(value.begin());
		while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
		return value;
	}
	return {};
}

bool ProcessAlive(uint32_t pid) {
	HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!p) return false;
	const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
	CloseHandle(p);
	return alive;
}

std::string LocalTimeText(int64_t unixSeconds) {
	const time_t t = time_t(unixSeconds);
	tm local{};
	localtime_s(&local, &t);
	char text[64];
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
	return text;
}

std::string Lower(std::wstring text) {
	for (wchar_t& c : text) c = wchar_t(towlower(c));
	return WideToUtf8(text);
}

// "ReShade add-on: renodx-dlss.addon64; Discord overlay: discordhook64.dll". Main thread; it changes
// when ReShade or an overlay loads after the session started.
std::string ScanNotableModules() {
	wchar_t system[MAX_PATH] = {};
	GetSystemDirectoryW(system, MAX_PATH);
	const std::string systemDir = Lower(system);
	std::string out;
	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE) return out;
	MODULEENTRY32W m{};
	m.dwSize = sizeof(m);
	for (BOOL more = Module32FirstW(snap, &m); more; more = Module32NextW(snap, &m)) {
		const std::string label = NotableModule(Lower(m.szExePath), systemDir);
		if (label.empty()) continue;
		if (!out.empty()) out += "; ";
		out += label + ": " + WideToUtf8(m.szModule);
	}
	CloseHandle(snap);
	return out;
}

// The lock names the session, plus what a later launch needs to describe it.
void WriteLock() {
	static int64_t start = int64_t(time(nullptr));
	std::ostringstream s;
	s << FormatLock(GetCurrentProcessId(), start) << "\n"
	  << "map=" << g_map << "\ndemo=" << g_demo << "\nmodules=" << g_modules << "\n";
	if (g_worstP95 > 0) s << "slowp95=" << int(g_worstP95) << "\n";
	const std::string text = s.str();
	WriteWholeFile(Path(L"session.lock").c_str(), text.data(), text.size());
}

std::string LockValue(const std::string& lock, const char* key) {
	std::istringstream in(lock);
	std::string line;
	const std::string prefix = std::string(key) + "=";
	while (std::getline(in, line))
		if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
	return {};
}

// Windows' own record of how process `pid` ended (Application log Events 1000/1002 since `startUnix`).
// wevtapi is loaded on demand; a missing or locked-down event log just yields nothing.
std::vector<std::string> WindowsAppEvents(uint32_t pid, int64_t startUnix) {
	std::vector<std::string> lines;
	HMODULE wevt = LoadLibraryExW(L"wevtapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!wevt) return lines;
	using Query_t = HANDLE(WINAPI*)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
	using Next_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE*, DWORD, DWORD, DWORD*);
	using Render_t = BOOL(WINAPI*)(HANDLE, HANDLE, DWORD, DWORD, PVOID, DWORD*, DWORD*);
	using Close_t = BOOL(WINAPI*)(HANDLE);
	auto query = (Query_t)GetProcAddress(wevt, "EvtQuery");
	auto next = (Next_t)GetProcAddress(wevt, "EvtNext");
	auto render = (Render_t)GetProcAddress(wevt, "EvtRender");
	auto close = (Close_t)GetProcAddress(wevt, "EvtClose");
	if (query && next && render && close) {
		const int64_t ageMs = (std::max<int64_t>)(0, int64_t(time(nullptr)) - startUnix + 300) * 1000;
		const std::wstring xpath = L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= "
			+ std::to_wstring(ageMs) + L"]]]";
		constexpr DWORD kChannelPath = 0x1, kRenderXml = 1;
		if (HANDLE results = query(nullptr, L"Application", xpath.c_str(), kChannelPath)) {
			HANDLE events[16];
			DWORD returned = 0;
			while (lines.size() < 4 && next(results, 16, events, 1000, 0, &returned)) {
				for (DWORD i = 0; i < returned; ++i) {
					DWORD used = 0, count = 0;
					render(nullptr, events[i], kRenderXml, 0, nullptr, &used, &count);
					std::vector<wchar_t> xml(used / sizeof(wchar_t) + 1);
					if (used && render(nullptr, events[i], kRenderXml, DWORD(xml.size() * sizeof(wchar_t)), xml.data(), &used, &count)) {
						const std::string line = WindowsAppEventLine(WideToUtf8(xml.data()), pid);
						if (!line.empty() && lines.size() < 4) lines.push_back(line);
					}
					close(events[i]);
				}
			}
			close(results);
		}
	}
	FreeLibrary(wevt);
	return lines;
}

// The previous session left its lock: it neither exited cleanly nor reached the crash handler.
void RecordUnexpectedExit() {
	const std::string lock = ReadText(Path(L"session.lock"));
	uint32_t pid = 0;
	int64_t start = 0;
	const bool parsed = ParseLock(lock, pid, start);
	if (!IsUnexpectedExit(!lock.empty(), parsed, pid, parsed && ProcessAlive(pid), GetCurrentProcessId())) return;

	// Named for when its log was last written: about when it died.
	SYSTEMTIME st;
	GetLocalTime(&st);
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (GetFileAttributesExW(Path(L"session.log").c_str(), GetFileExInfoStandard, &info)) {
		FILETIME local{};
		FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
		FileTimeToSystemTime(&local, &st);
	}
	char base[96];
	BaseName(base, sizeof(base), "exit", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	char lastWrite[32];
	snprintf(lastWrite, sizeof(lastWrite), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);

	// CS2 may have written its own dump for this session without our handler running (user report
	// 2026-10-12: an access-violation .mdmp at the last log line, reported as "no caught crash").
	auto unixSeconds = [](const FILETIME& ft) {
		const uint64_t ticks = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		return int64_t(ticks / 10000000ull) - 11644473600ll;
	};
	const int64_t lastLog = info.ftLastWriteTime.dwHighDateTime || info.ftLastWriteTime.dwLowDateTime
		? unixSeconds(info.ftLastWriteTime) : int64_t(time(nullptr));
	std::string dumpName;
	{
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::error_code ec;
		int64_t best = 0;
		for (const auto& e : fs::directory_iterator(fs::path(exe).parent_path(), ec)) {
			if (!e.is_regular_file(ec) || _wcsicmp(e.path().extension().c_str(), L".mdmp") != 0) continue;
			WIN32_FILE_ATTRIBUTE_DATA dump{};
			if (!GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &dump)) continue;
			const int64_t written = unixSeconds(dump.ftLastWriteTime);
			if (DumpBelongsToSession(written, start, lastLog) && written > best) {
				best = written;
				dumpName = WideToUtf8(e.path().filename().wstring());
			}
		}
	}

	const bool quit = LockValue(lock, "exit") == "clean";
	const std::string slow = LockValue(lock, "slowp95");
	const std::vector<std::string> windows = quit ? std::vector<std::string>() : WindowsAppEvents(pid, start);
	std::ostringstream h;
	if (!windows.empty()) {
		// "Windows   : Application Error (Event 1000), <what>" -> "CS2 crashed outside our handler: <what>"
		const std::string& w = windows.front();
		const size_t comma = w.find("), ");
		h << "Summary   : CS2 " << (w.find("(Event 1002)") != std::string::npos ? std::string("hung and Windows ended it")
			: "crashed outside our handler: " + (comma == std::string::npos ? w : w.substr(comma + 3)))
		  << " (session started " << LocalTimeText(start) << ", last log line " << lastWrite << ")\n";
	} else if (quit && dumpName.empty()) {
		h << "Summary   : CS2 was closed normally after running very slowly (worst p95 frame " << slow
		  << " ms; session started " << LocalTimeText(start) << ", last log line " << lastWrite << ")\n";
	} else if (!dumpName.empty()) {
		const std::string kind = DumpKind(dumpName);
		h << "Summary   : CS2 crashed" << (kind.empty() ? "" : " (" + kind + ")") << "; it wrote " << dumpName
		  << " (session started " << LocalTimeText(start) << ", last log line " << lastWrite << ")\n";
	} else {
		h << "Summary   : CS2 closed without a clean exit and without a caught crash (session started "
		  << LocalTimeText(start) << ", last log line " << lastWrite << ")\n";
	}
	h << "Map       : " << LockValue(lock, "map") << "\n"
	  << "Demo      : " << LockValue(lock, "demo") << "\n"
	  << "Injected  : " << (LockValue(lock, "modules").empty() ? "(none found)" : LockValue(lock, "modules")) << "\n";
	if (!slow.empty() && !(quit && dumpName.empty()))
		h << "Slowness  : frames were slow before the end (worst p95 " << slow << " ms)\n";
	for (const std::string& line : windows) h << line << "\n";
	if (!windows.empty()) {
		h << "Likely    : what the Windows line above names (the faulting module and offset, or a hang).\n";
	} else if (quit && dumpName.empty()) {
		h << "Likely    : the game crawled (frames taking hundreds of ms or more) and the player quit.\n"
		  << "            The heartbeat lines in the history show frame times, VRAM and the GPU.\n";
	} else if (!dumpName.empty()) {
		h << "Likely    : a crash our handler did not see. The faulting module is in that dump: ask for it\n"
		  << "            (game\\bin\\win64) and open it against the matching PDBs.\n";
	} else {
		h << "Likely    : a hang that was force-closed (Event Viewer > Application, Event 1002), a fast-fail\n"
		  << "            Windows handles itself (heap corruption, stack cookie; Event 1000 names the module),\n"
		  << "            the process killed from Task Manager or by a tool, or power loss.\n";
	}
	const std::string header = h.str();
	WriteWholeFile(Path(std::string(base) + ".pending").c_str(), header.data(), header.size());
	MoveFileExW(Path(L"session.log").c_str(), Path(std::string(base) + ".session.log").c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::vector<std::string> PendingBases() {
	std::vector<std::string> bases;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(fs::path(g_dir), ec)) {
		if (!e.is_regular_file(ec) || e.path().extension() != L".pending") continue;
		bases.push_back(WideToUtf8(e.path().stem().wstring()));
	}
	return bases;
}

// What the main menu offers: pending reports whose .txt is written, newest first.
void RefreshPending() {
	std::vector<std::string> ready;
	std::error_code ec;
	for (const std::string& b : PickPendingToSend(PendingBases()))
		if (fs::is_regular_file(Path(b + ".txt"), ec)) ready.push_back(b);
	std::lock_guard<std::mutex> lock(g_statusMutex);
	g_pending.count = int(ready.size());
	g_pending.newestReport = ready.empty() ? std::wstring() : Path(ready.front() + ".txt");
	g_pending.newestFile = ready.empty() ? std::string() : ready.front() + ".txt";
	g_pending.newestSummary = ready.empty() ? std::string() : Field(ReadText(Path(ready.front() + ".pending")), "Summary");
	g_pending.hasDump = !ready.empty() && fs::is_regular_file(Path(ready.front() + ".dmp"), ec);
}

// Keeps the folder small (CrashReportMath kMaxKeptReports / kMaxKeptReportBytes): every file of an
// old report goes, bug and crash alike. This session's own session.log and session.lock are never
// touched, nor anything not named SourceMVM-*.
int PruneOldReports() {
	std::map<std::string, KeptReport> byBase;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(fs::path(g_dir), ec)) {
		if (!e.is_regular_file(ec)) continue;
		const std::string name = WideToUtf8(e.path().filename().wstring());
		if (name.rfind("SourceMVM-", 0) != 0) continue;
		const std::string base = name.substr(0, name.find('.'));
		KeptReport& r = byBase.emplace(base, KeptReport{base, 0, false}).first->second;
		r.bytes += e.file_size(ec);
		if (e.path().extension() == L".pending") r.awaitingDecision = true;
	}
	std::vector<KeptReport> reports;
	for (const auto& kv : byBase) reports.push_back(kv.second);
	const std::vector<std::string> prune = PickReportsToPrune(reports);
	// Collect first, delete after: removing entries while iterating the same directory is unspecified.
	std::vector<fs::path> doomed;
	for (const auto& e : fs::directory_iterator(fs::path(g_dir), ec)) {
		const std::string name = WideToUtf8(e.path().filename().wstring());
		const std::string base = name.substr(0, name.find('.'));
		if (std::find(prune.begin(), prune.end(), base) != prune.end()) doomed.push_back(e.path());
	}
	int removed = 0;
	for (const fs::path& path : doomed) {
		std::error_code removeError;
		if (fs::remove(path, removeError)) ++removed;
	}
	MvmDebugLog_LinefAlways("crashreport.prune", "reports=%zu prune=%zu files_removed=%d of %zu",
		reports.size(), prune.size(), removed, doomed.size());
	return removed;
}

// On launch: earlier sessions' reports are written out locally for the player to review. The newest
// few are offered; the rest stay on disk as .unsent. Nothing is sent here.
void PreparePending() {
	const std::vector<std::string> bases = PendingBases();
	const std::vector<std::string> picked = PickPendingToSend(bases);
	for (const std::string& b : bases) {
		const bool offered = std::find(picked.begin(), picked.end(), b) != picked.end();
		// Older builds put the Steam name and ID into the header. Those are never offered for sending.
		const bool oldFormat = ReadText(Path(b + ".pending")).find("\nSteamID   :") != std::string::npos;
		if (!offered || oldFormat)
			MoveFileExW(Path(b + ".pending").c_str(), Path(b + ".unsent").c_str(), MOVEFILE_REPLACE_EXISTING);
	}
	std::error_code ec;
	for (const std::string& b : PickPendingToSend(PendingBases())) {
		if (fs::is_regular_file(Path(b + ".txt"), ec)) continue;
		// Only the header survived (or an unexpected exit): build the report from it and the dead
		// session's log.
		const std::string header = ReadText(Path(b + ".pending"));
		BugReportCrash crash;
		crash.summary = Field(header, "Summary");
		crash.details = header;
		crash.baseName = b;
		crash.map = Field(header, "Map");
		crash.demo = Field(header, "Demo");
		crash.previousSession = true;
		crash.history = BugReport::TailText(ReadText(Path(b + ".session.log")), kSessionLogBudget);
		if (BugReport_Write("", BugReportSender{}, &crash).ok)
			DeleteFileW(Path(b + ".session.log").c_str());
	}
	PruneOldReports();
	RefreshPending();
}

// The player pressed Send on the crash prompt: post the offered reports, then forget them.
void SendApproved() {
	int sent = 0, failed = 0;
	std::string lastWhy;
	std::error_code ec;
	for (const std::string& b : PickPendingToSend(PendingBases())) {
		const fs::path report = Path(b + ".txt");
		if (!fs::is_regular_file(report, ec)) continue;
		const std::string header = ReadText(Path(b + ".pending"));
		const std::string text = ReadText(report);
		BugReportResult r;
		r.ok = true;
		r.path = report.wstring();
		r.fileName = b + ".txt";
		r.bytes = text.size();
		r.crash = Field(header, "Summary");
		r.map = Field(header, "Map");
		r.demo = Field(header, "Demo");
		r.hookBuild = Field(text, "SOURCE:MVM hook built");
		r.cs2Build = Field(text, "CS2");
		r.windows = Field(text, "Windows");
		r.health = Field(text, "Render / main pump");
		if (fs::is_regular_file(Path(b + ".dmp"), ec)) r.dumpPath = Path(b + ".dmp");
		std::string why;
		if (BugReportSend_Blocking(r, why)) {
			++sent;
			// Sent: the developer has it, so nothing of it stays on this PC.
			DeleteFileW(Path(b + ".pending").c_str());
			DeleteFileW(Path(b + ".session.log").c_str());
			DeleteFileW(Path(b + ".dmp").c_str());
			DeleteFileW(Path(b + ".txt").c_str());
		} else {
			++failed;
			lastWhy = why;
		}
		Sleep(2000); // gentle on the webhook
	}
	{
		std::lock_guard<std::mutex> lock(g_statusMutex);
		g_pending.send = failed ? "failed" : "sent";
		g_pending.sendDetail = failed ? lastWhy : std::string();
		g_lastSend = "previous sessions: " + std::to_string(sent) + " report(s) sent" +
			(failed ? ", " + std::to_string(failed) + " failed (" + lastWhy + "), kept" : "");
	}
	RefreshPending();
}

// The session begins on the first main-thread frame (shell folders and file IO are safe there).
void StartSession() {
	// Same folder BugReport_Write uses, now that the shell can be asked.
	const std::wstring folder = BugReport_Folder();
	std::error_code ec;
	fs::create_directories(folder, ec);
	if (!folder.empty() && folder.size() < MAX_PATH) wcscpy_s(g_dir, folder.c_str());
	// Sending is now asked for every time; the old "never send" marker means nothing anymore.
	DeleteFileW(Path(L"crashreports.off").c_str());
	g_sessionStartTick = GetTickCount64();
	g_mainThreadId = GetCurrentThreadId();
	CopyText(g_hookModule, sizeof(g_hookModule), BugReport_HookModuleName());
	CopyText(g_hookId, sizeof(g_hookId), g_hookModule + std::string("/") + BugReport_HookImageKey());
	InstallDumpWriterHook(); // LoadLibrary: never under the loader lock, so here and not in DllMain

	RecordUnexpectedExit();
	CopyText(g_modules, sizeof(g_modules), ScanNotableModules());
	WriteLock();
	g_sessionLog = CreateFileW(Path(L"session.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
	                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	g_sessionLogBytes = 0;
}

std::atomic<bool> g_pendingBusy{false}; // preparing or sending on a worker thread
int g_frame = 0;

void RunPendingWork(void (*work)()) {
	if (g_pendingBusy.exchange(true)) return;
	std::thread([work]() { work(); g_pendingBusy = false; }).detach();
}

} // namespace

void CrashReport_InstallHandlers() {
	if (g_handlersInstalled.exchange(true)) return;
	// Until the session starts, crash files go to %APPDATA%\HLAE\bugreports\ from the environment.
	wchar_t appData[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	if (n && n < MAX_PATH - 32) {
		swprintf(g_dir, MAX_PATH, L"%s\\HLAE\\", appData);
		CreateDirectoryW(g_dir, nullptr);
		wcscat_s(g_dir, L"bugreports\\");
		CreateDirectoryW(g_dir, nullptr);
	}

	g_chainedFilter = SetUnhandledExceptionFilter(OnUnhandledException);
	g_chainedInvalidParameter = _set_invalid_parameter_handler(OnInvalidParameter);
	signal(SIGABRT, OnAbortSignal);

	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	g_origSetFilter = k32 ? (SetUnhandledExceptionFilter_t)GetProcAddress(k32, "SetUnhandledExceptionFilter") : nullptr;
	g_origTerminateProcess = k32 ? (TerminateProcess_t)GetProcAddress(k32, "TerminateProcess") : nullptr;
	if (g_origSetFilter && g_origTerminateProcess) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)g_origSetFilter, New_SetUnhandledExceptionFilter);
		DetourAttach(&(PVOID&)g_origTerminateProcess, New_TerminateProcess);
		DetourTransactionCommit();
	}
}

void CrashReport_MainThreadFrame() {
	if (!g_sessionStarted.exchange(true)) {
		StartSession();
		RunPendingWork(PreparePending); // earlier sessions' reports, written out for the prompt
	}
	// Remember where the player is, for a crash. Cheap getters, but not every frame.
	if (++g_frame % 120 == 1) {
		std::string map = Environment::CurrentMapName();
		if (map.empty() || map == "<empty>") map = "(main menu)";
		const std::wstring demo = ResolvePlayingDemoPath();
		const std::string demoName = demo.empty() ? "" : WideToUtf8(fs::path(demo).filename().wstring());
		// ReShade and overlays can load after the first frame: rescan now and then (~10 s).
		const std::string modules = g_frame % 1200 == 1 ? ScanNotableModules() : std::string(g_modules);
		if (map != g_map || demoName != g_demo || modules != g_modules) {
			CopyText(g_map, sizeof(g_map), map);
			CopyText(g_demo, sizeof(g_demo), demoName);
			CopyText(g_modules, sizeof(g_modules), modules);
			WriteLock();
		}
	}
	// A crash test in this session adds a report: notice it without waiting for a relaunch.
	if (g_frame % 240 == 2 && !g_pendingBusy) RefreshPending();
}

CrashReportPending CrashReport_Pending() {
	std::lock_guard<std::mutex> lock(g_statusMutex);
	return g_pending;
}

void CrashReport_SendPending() {
	if (!BugReportSend_Enabled()) return;
	{
		std::lock_guard<std::mutex> lock(g_statusMutex);
		if (g_pending.count == 0 || g_pending.send == "sending") return;
		g_pending.send = "sending";
		g_pending.sendDetail.clear();
	}
	RunPendingWork(SendApproved);
}

void CrashReport_DeclinePending() {
	// Kept on disk as .declined (the report .txt stays for the player); never offered again.
	for (const std::string& b : PickPendingToSend(PendingBases())) {
		MoveFileExW(Path(b + ".pending").c_str(), Path(b + ".declined").c_str(), MOVEFILE_REPLACE_EXISTING);
		DeleteFileW(Path(b + ".session.log").c_str());
		DeleteFileW(Path(b + ".dmp").c_str());
	}
	{
		std::lock_guard<std::mutex> lock(g_statusMutex);
		g_pending.send = "declined";
	}
	RefreshPending();
}

void CrashReport_SessionLogLine(const std::string& line) {
	const HANDLE log = g_sessionLog;
	if (log == INVALID_HANDLE_VALUE || g_sessionLogBytes > kSessionLogMaxBytes) return;
	DWORD written = 0;
	WriteFile(log, line.data(), DWORD(line.size()), &written, nullptr);
	WriteFile(log, "\n", 1, &written, nullptr);
	g_sessionLogBytes += line.size() + 1;
	if (g_sessionLogBytes > kSessionLogMaxBytes) {
		static const char kCap[] = "(session log reached its size cap; the in-memory recorder continues)\n";
		WriteFile(log, kCap, DWORD(sizeof(kCap) - 1), &written, nullptr);
	}
}

std::string CrashReport_InjectedModules() { return g_modules; }

void CrashReport_NoteFrameTime(double p95Ms, unsigned frames) {
	// Only a new worst slow value rewrites the lock (main thread, as the heartbeat is).
	if (!IsSlowHeartbeat(p95Ms, frames) || p95Ms <= g_worstP95 + 50.0) return;
	g_worstP95 = p95Ms;
	if (g_sessionStarted) WriteLock();
}

void CrashReport_CleanExit() {
	if (g_crashing || !g_dir[0]) return;
	wchar_t lock[MAX_PATH + 32];
	swprintf(lock, sizeof(lock) / sizeof(lock[0]), L"%ssession.lock", g_dir);
	if (!(g_worstP95 >= kSlowSessionP95Ms)) {
		DeleteFileW(lock);
		return;
	}
	// It crawled: keep the lock so the next launch reports it, marked as a quit. Kernel32 only:
	// this can run under the loader lock (DllMain detach).
	const HANDLE file = CreateFileW(lock, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	static const char kClean[] = "exit=clean\n";
	DWORD written = 0;
	WriteFile(file, kClean, DWORD(sizeof(kClean) - 1), &written, nullptr);
	CloseHandle(file);
}

void CrashReport_Command(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* verb = argc >= 4 ? args->ArgV(3) : "status";
	if (0 == _stricmp(verb, "test")) {
		// The whole pipeline without crashing: a report marked as a test, saved like a real one and
		// offered on the main menu like one.
		if (g_crashing.exchange(true)) { advancedfx::Warning("crash report: already handling one.\n"); return; }
		RtlCaptureContext(&g_captureContext);
		HandleCrash("TEST (no crash; mirv_filmmaker community crash test)", 0, &g_captureContext, nullptr);
		// The test ended this session's lock and log; start them again.
		g_crashing = false;
		WriteLock();
		g_sessionLog = CreateFileW(Path(L"session.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		g_sessionLogBytes = 0;
		RefreshPending();
		advancedfx::Message("crash report test: %s.txt written; the main menu asks before sending it\n", g_base);
		return;
	}
	if (0 == _stricmp(verb, "prune")) {
		advancedfx::Message("crash reports: removed %d old file(s)\n", PruneOldReports());
		return;
	}
	if (0 == _stricmp(verb, "now")) {
		// A real access violation, to verify the handler end to end. Only on explicit request.
		advancedfx::Warning("crash report: crashing CS2 now on purpose.\n");
		*reinterpret_cast<volatile int*>(uintptr_t(0x10)) = 1;
		return;
	}
	std::string last;
	{
		std::lock_guard<std::mutex> lock(g_statusMutex);
		last = g_lastSend;
	}
	const CrashReportPending p = CrashReport_Pending();
	advancedfx::Message(
		"crash reports: handlers %s, CS2 dump hook %s, sending %s, %d awaiting the player's decision, %s\n"
		"%s community crash status|test|prune|now\n",
		g_handlersInstalled ? "installed" : "NOT installed",
		g_origMiniDumpWriteDump ? "installed" : "NOT installed",
		BugReportSend_Enabled() ? "only when the player presses Send" : "unavailable (no webhook in this build)",
		p.count, last.c_str(), cmd);
}

} // namespace Filmmaker
