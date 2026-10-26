#include "MvmDebugLog.h"
#include "MvmDebugLogHost.h"           // log folder, demo tick and frame from the hook linking this
#include "DebugLogDedupMath.h"
#if !defined(GAME_DEADLOCK) && !defined(GAME_SOURCE1)
#include "../../ConsoleLoggingListener.h"
#include "../Community/BugReport.h" // BugReport_Record: the always-on recorder
#endif

#include "../Platform/TextEncoding.h"  // WideToUtf8
#include "../../../Shared/AfxConsole.h"

// Isolated translation unit so <windows.h> (GetLocalTime / CreateDirectoryW) does not leak its
// min/max macros into the SDK-heavy cosmetics files. NOMINMAX/LEAN keep it tidy regardless.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <winioctl.h>
#pragma comment(lib, "winhttp.lib")
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace Filmmaker {

// Cursor Debug Logs panel reads NDJSON from the local ingest server (127.0.0.1:7571). Optional file
// mirror via MVM_AGENT_DEBUG_LOG / MVM_AGENT_LOG / CURSOR_DEBUG_LOG env vars (workspace debug-af2ef9.log).
namespace {
constexpr wchar_t kAgentIngestHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kAgentIngestPort = 7571;
constexpr wchar_t kAgentIngestPath[] =
	L"/ingest/5987dfd1-d151-4549-a5c3-03dddc72dd02";
constexpr wchar_t kAgentIngestHdr[] =
	L"Content-Type: application/json\r\nX-Debug-Session-Id: af2ef9\r\n";

DWORD WINAPI AgentLogHttpThread(LPVOID param) {
	char* body = (char*)param;
	if (!body)
		return 0;
	HINTERNET ses = WinHttpOpen(L"MvmAgentLog/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (ses) {
		HINTERNET conn = WinHttpConnect(ses, kAgentIngestHost, kAgentIngestPort, 0);
		if (conn) {
			HINTERNET req = WinHttpOpenRequest(conn, L"POST", kAgentIngestPath,
				nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
			if (req) {
				const DWORD bodyLen = (DWORD)std::strlen(body);
				if (WinHttpSendRequest(req, kAgentIngestHdr, (DWORD)-1L, body, bodyLen, bodyLen, 0))
					WinHttpReceiveResponse(req, nullptr);
				WinHttpCloseHandle(req);
			}
			WinHttpCloseHandle(conn);
		}
		WinHttpCloseHandle(ses);
	}
	free(body);
	return 0;
}

void PostAgentLogAsync(const char* line, int lineLen) {
	if (!line || lineLen <= 0 || lineLen >= 2040)
		return;
	char* copy = (char*)malloc((size_t)lineLen + 1);
	if (!copy)
		return;
	memcpy(copy, line, (size_t)lineLen);
	copy[lineLen] = '\0';
	HANDLE th = CreateThread(nullptr, 0, AgentLogHttpThread, copy, 0, nullptr);
	if (th)
		CloseHandle(th);
	else
		free(copy);
}

void WrapAgentDataJson(const char* dataJson, char* out, size_t outSize) {
	if (!out || outSize < 3) {
		if (out && outSize > 0)
			out[0] = '\0';
		return;
	}
	if (!dataJson || !*dataJson) {
		std::snprintf(out, outSize, "{}");
		return;
	}
	while (*dataJson == ' ' || *dataJson == '\t')
		++dataJson;
	if (dataJson[0] == '{')
		std::snprintf(out, outSize, "%s", dataJson);
	else
		std::snprintf(out, outSize, "{%s}", dataJson);
}
} // namespace (agent helpers)

namespace {
FILE* g_file = nullptr;
std::wstring g_folderW;
std::wstring g_fileW;

struct EventMeta {
	SYSTEMTIME time = {};
	DWORD threadId = 0;
	int tick = -1;
	int frame = 0;
	uint64_t sequence = 0;
};

struct PendingEvent {
	std::string category;
	std::string message;
	EventMeta first;
	EventMeta last;
	uint64_t occurrences = 0;
};

std::mutex g_mutex;
std::atomic<bool> g_active(false);
DebugLogDedupCache<PendingEvent> g_eventsByIdentity;
MvmDebugLogStats g_stats;
uint64_t g_sequence = 0;

// --- crash breadcrumb (vectored exception handler) ------------------------------------------------
PVOID g_vehHandle = nullptr;
std::atomic<unsigned long long> g_crashWatchUntilMs(0); // GetTickCount64() deadline; 0 = window closed
std::atomic<int> g_crashLogged(0);                      // cap how many AVs we record per session
std::atomic<int> g_lastSwapIdx(-1);
char g_lastSwapModel[260] = {};                         // racy w.r.t. the VEH thread, acceptable for a crumb

// Our own module handle, resolved once from the address of a global in our data section -- used to skip
// faults that originate INSIDE AfxHookSource2.dll (those are our own SEH-guarded reads faulting and being
// caught by __except; noise, not the crash, which is in client.dll/engine).
HMODULE SelfModule() {
	static HMODULE s_self = nullptr;
	if (!s_self)
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&g_lastSwapModel, &s_self);
	return s_self;
}

// First-chance AV logger. Returns CONTINUE_SEARCH ALWAYS -- it never swallows, so the game's own SEH /
// crash handling proceeds exactly as before; we only observe. Gated to (a) a log being open and (b) the
// post-swap watch window. SKIPS faults inside our own module (our SEH-guarded reads fault+catch every
// frame -- in a non-crashing run that flooded the log with hundreds of identical AfxHookSource2.dll lines
// and could mask the real client.dll crash) and de-dupes consecutive identical fault sites. Defensive:
// AV only, recursion-guarded, capped.
LONG CALLBACK MvmVectoredHandler(EXCEPTION_POINTERS* ep) {
	if (!ep || !ep->ExceptionRecord)
		return EXCEPTION_CONTINUE_SEARCH;
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (!g_active.load(std::memory_order_acquire))
		return EXCEPTION_CONTINUE_SEARCH;
	// Previously gated to a 30s window opened by MvmCrashWatch_Arm() around specific operations
	// (rebuild/knife-swap/paint-apply) -- but that means any crash NOT adjacent to one of those calls
	// (e.g. during rapid spectate switching, as observed) writes NOTHING to the log: no crash.veh line,
	// no clue what happened, just the process restarting. The handler is already safe to run
	// unconditionally whenever debug logging is on -- it never swallows the exception (always
	// CONTINUE_SEARCH), skips our own module's routine caught faults, de-dupes consecutive identical
	// fault sites, and caps at 64 entries -- so there is no flooding risk from removing the window gate.
	// g_lastSwapIdx/g_lastSwapModel (set by MvmCrashWatch_Arm) still tag the entry with whatever
	// cosmetics operation happened most recently, even if it is not what caused this particular fault.
	static thread_local int s_inHandler = 0; // avoid recursion if logging itself faults
	if (s_inHandler)
		return EXCEPTION_CONTINUE_SEARCH;
	s_inHandler = 1;

	void* addr = ep->ExceptionRecord->ExceptionAddress;

	// Resolve the faulting module FIRST, before the cap, so our own caught faults can't burn the budget.
	char modName[64] = "?";
	unsigned long long off = 0;
	HMODULE hmod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &hmod) && hmod) {
		char full[MAX_PATH];
		if (GetModuleFileNameA(hmod, full, MAX_PATH)) {
			const char* base = full;
			for (const char* p = full; *p; ++p)
				if (*p == '\\' || *p == '/') base = p + 1;
			size_t i = 0;
			for (; base[i] && i + 1 < sizeof(modName); ++i) modName[i] = base[i];
			modName[i] = '\0';
		}
		off = (unsigned long long)((unsigned char*)addr - (unsigned char*)hmod);
	}

	// Skip our own module (caught SEH reads) and consecutive repeats of the same fault site.
	static unsigned long long s_lastRip = 0;
	if ((hmod && hmod == SelfModule()) || (unsigned long long)addr == s_lastRip) {
		s_inHandler = 0;
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (g_crashLogged.fetch_add(1, std::memory_order_acq_rel) >= 64) {
		s_inHandler = 0;
		return EXCEPTION_CONTINUE_SEARCH;
	}
	s_lastRip = (unsigned long long)addr;

	const char* access = "?";
	unsigned long long target = 0;
	if (ep->ExceptionRecord->NumberParameters >= 2) {
		const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
		target = (unsigned long long)info[1];
		access = (info[0] == 0) ? "read" : (info[0] == 1 ? "write" : (info[0] == 8 ? "execute" : "?"));
	}

	MvmDebugLog_LinefAlways("crash.veh",
		"ACCESS_VIOLATION %s faultRip=%p mod=%s+0x%llx target=0x%llx thread=%lu lastSwapIdx=%d lastModel='%s'",
		access, addr, modName, off, target, (unsigned long)GetCurrentThreadId(),
		g_lastSwapIdx.load(std::memory_order_relaxed), g_lastSwapModel);

	s_inHandler = 0;
	return EXCEPTION_CONTINUE_SEARCH;
}

EventMeta CaptureMetaLocked() {
	EventMeta out;
	GetLocalTime(&out.time);
	out.threadId = GetCurrentThreadId();
	int tick = -1;
	if (MvmDebugLogHost_DemoTick(tick))
		out.tick = tick;
	out.frame = MvmDebugLogHost_FrameCount();
	out.sequence = ++g_sequence;
	return out;
}

void PrintMetaLocked(const EventMeta& meta, const char* category) {
	fprintf(g_file,
		"[%02d:%02d:%02d.%03d] [seq=%06llu] [%s] [tick=%d frame=%d thread=%lu] ",
		meta.time.wHour, meta.time.wMinute, meta.time.wSecond, meta.time.wMilliseconds,
		(unsigned long long)meta.sequence, category, meta.tick, meta.frame,
		(unsigned long)meta.threadId);
}

void FlushRepeatLocked(const PendingEvent& pending) {
	if (!g_file || pending.occurrences <= 1)
		return;
	PrintMetaLocked(pending.last, "repeat");
	fprintf(g_file,
		"category=%s repeated=%llu total=%llu first=%02d:%02d:%02d.%03d seq=%llu "
		"last=%02d:%02d:%02d.%03d seq=%llu tick=%d..%d frame=%d..%d\n",
		pending.category.c_str(),
		(unsigned long long)(pending.occurrences - 1),
		(unsigned long long)pending.occurrences,
		pending.first.time.wHour, pending.first.time.wMinute, pending.first.time.wSecond,
		pending.first.time.wMilliseconds, (unsigned long long)pending.first.sequence,
		pending.last.time.wHour, pending.last.time.wMinute, pending.last.time.wSecond,
		pending.last.time.wMilliseconds, (unsigned long long)pending.last.sequence,
		pending.first.tick, pending.last.tick, pending.first.frame, pending.last.frame);
}

void FlushAllRepeatsLocked() {
	for (const auto& entry : g_eventsByIdentity.All())
		FlushRepeatLocked(entry.second);
}

std::string FormatMessage(const char* fmt, va_list args) {
	va_list sizeArgs;
	va_copy(sizeArgs, args);
	const int required = _vscprintf(fmt, sizeArgs);
	va_end(sizeArgs);
	if (required < 0)
		return "<formatting failed>";
	std::string out((size_t)required + 1, '\0');
	vsnprintf(&out[0], out.size(), fmt, args);
	out.resize((size_t)required);
	return out;
}

std::string SingleLine(const char* text) {
	std::string out;
	if (!text)
		return out;
	for (const char* p = text; *p; ++p) {
		if (*p == '\r')
			continue;
		if (*p == '\n') {
			if (!out.empty() && out.back() != ' ')
				out += " | ";
			continue;
		}
		out += *p;
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
		out.pop_back();
	return out;
}

void LineV(const char* category, bool dedup, const char* fmt, va_list args) {
#if !defined(GAME_DEADLOCK) && !defined(GAME_SOURCE1)
    Cs2ConsoleLogScope callback;
    if(!callback)return;
#endif
	if (!g_active.load(std::memory_order_acquire))
		return;
	std::string message = FormatMessage(fmt, args);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
		return;

	const EventMeta meta = CaptureMetaLocked();
	++g_stats.eventsReceived;
	const char* safeCategory = category ? category : "event";

	// Non-deduped path: write the line immediately, every time. Discrete user actions (a skin click)
	// must never be folded into a repeat summary, or a click that repeats an earlier state would look
	// like "nothing happened" until the log is stopped.
	if (!dedup) {
		PrintMetaLocked(meta, safeCategory);
		fprintf(g_file, "%s\n", message.c_str());
		fflush(g_file);
		++g_stats.uniqueEventsWritten;
		return;
	}

	std::string identity(safeCategory);
	identity.push_back('\n');
	identity += message;
	auto* found = g_eventsByIdentity.Find(identity);
	if (found) {
		PendingEvent& pending = *found;
		pending.last = meta;
		++pending.occurrences;
		++g_stats.repeatsCombined;
		return;
	}

	auto* retained = g_eventsByIdentity.Retain(identity, std::strlen(safeCategory) + message.size(),
        [&](const auto& events) {
            for (const auto& entry : events) FlushRepeatLocked(entry.second);
            PrintMetaLocked(meta, "debuglog.retention");
            fprintf(g_file, "repeat window flushed entries=%zu payloadBytes=%zu limits=4096/4194304\n",
                g_eventsByIdentity.Size(), g_eventsByIdentity.PayloadBytes());
        });
    if (retained) {
        retained->category = safeCategory;
        retained->message = message;
        retained->first = meta;
        retained->last = meta;
        retained->occurrences = 1;
    }

	PrintMetaLocked(meta, safeCategory);
	fprintf(g_file, "%s\n", message.c_str());
	fflush(g_file);
	++g_stats.uniqueEventsWritten;
}
}

namespace {
// Logs were never removed and reached 1.3 GB across ~90 sessions. Names sort by start time
// (mvm_debug_YYYYMMDD_HHMMSS.log), so keep the newest ones and delete the rest at each start.
constexpr size_t kKeptDebugLogs = 20;

void PruneOldLogs(const std::wstring& folder) {
	std::vector<std::wstring> logs;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((folder + L"\\mvm_debug_*.log").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) logs.push_back(fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	// The new log is about to be added, so leave room for it.
	if (logs.size() < kKeptDebugLogs) return;
	std::sort(logs.begin(), logs.end());
	for (size_t i = 0; i + kKeptDebugLogs - 1 < logs.size(); ++i)
		DeleteFileW((folder + L"\\" + logs[i]).c_str());
}

// NTFS compression: transparent (the logs stay plain text to every reader) and about 3x on these
// logs. A file created in a compressed folder is compressed as it is written.
bool SetNtfsCompressed(const std::wstring& path, bool folder) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, folder ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	USHORT format = COMPRESSION_FORMAT_DEFAULT;
	DWORD returned = 0;
	const BOOL ok = DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned, nullptr);
	CloseHandle(h);
	return ok != FALSE;
}

void CompressLogFolder(const std::wstring& folder) {
	const DWORD attributes = GetFileAttributesW(folder.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_COMPRESSED)) return;
	if (!SetNtfsCompressed(folder, true)) return; // not NTFS, or not allowed: logs stay as they are
	// Logs written before the folder was compressed: rewritten off the game's threads.
	std::thread([folder] {
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((folder + L"\\mvm_debug_*.log").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED)))
				SetNtfsCompressed(folder + L"\\" + fd.cFileName, false);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}).detach();
}
} // namespace

bool MvmDebugLog_Active() { return g_active.load(std::memory_order_acquire); }

bool MvmDebugLog_Start(std::string* outFile) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file)
		return false;

	std::wstring folder = MvmDebugLogHost_RoamingFolder();
	if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
		folder += L'\\';
	folder += L"debuglogs";
	CreateDirectoryW(folder.c_str(), nullptr); // ok if it already exists
	PruneOldLogs(folder);
	CompressLogFolder(folder);

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t name[160];
	swprintf(name, 160, L"\\mvm_debug_%04d%02d%02d_%02d%02d%02d.log",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	g_folderW = folder;
	g_fileW = folder + name;
	g_file = _wfopen(g_fileW.c_str(), L"w");
	if (!g_file)
		return false;

	g_eventsByIdentity.Clear();
	g_stats = {};
	g_sequence = 0;
	fprintf(g_file, "=== SOURCE:MVM debug log started %04d-%02d-%02d %02d:%02d:%02d ===\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	fprintf(g_file,
		"line format: [time] [sequence] [category] [demo tick, engine frame, thread] message\n"
		"compression: identical category+payload events are written once per bounded window (4096 entries / 4 MiB payload); repeat summaries include count and time range\n\n");
	fflush(g_file);
	g_active.store(true, std::memory_order_release);

	// Arm the crash breadcrumb handler for this session (only logs within a post-swap window).
	g_crashLogged.store(0, std::memory_order_release);
	g_crashWatchUntilMs.store(0, std::memory_order_release);
	if (!g_vehHandle)
		g_vehHandle = AddVectoredExceptionHandler(1 /*call first*/, MvmVectoredHandler);

	if (outFile)
		*outFile = WideToUtf8(g_fileW);
	return true;
}

bool MvmDebugLog_Stop(std::string* outFolder, std::string* outFile, MvmDebugLogStats* outStats) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
		return false;
	g_active.store(false, std::memory_order_release);
	g_crashWatchUntilMs.store(0, std::memory_order_release);
	if (g_vehHandle) {
		RemoveVectoredExceptionHandler(g_vehHandle);
		g_vehHandle = nullptr;
	}
	FlushAllRepeatsLocked();
	g_eventsByIdentity.Clear();

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(g_file,
		"\n=== stopped %02d:%02d:%02d events=%llu unique=%llu repeats_combined=%llu ===\n",
		st.wHour, st.wMinute, st.wSecond,
		(unsigned long long)g_stats.eventsReceived,
		(unsigned long long)g_stats.uniqueEventsWritten,
		(unsigned long long)g_stats.repeatsCombined);
	fclose(g_file);
	g_file = nullptr;

	if (outFolder)
		*outFolder = WideToUtf8(g_folderW);
	if (outFile)
		*outFile = WideToUtf8(g_fileW);
	if (outStats)
		*outStats = g_stats;
	return true;
}

MvmDebugLogStats MvmDebugLog_GetStats() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

void MvmDebugLog_Linef(const char* category, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LineV(category, /*dedup=*/true, fmt, ap);
	va_end(ap);
}

void MvmDebugLog_LinefAlways(const char* category, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LineV(category, /*dedup=*/false, fmt, ap);
	va_end(ap);
}

// Cursor/agent NDJSON hook: POSTs to the Cursor debug ingest server for the chat Debug Logs panel,
// optionally mirrors to env-configured file paths, and copies to mvm_debug category "agent" when active.
void MvmAgentLog(const char* hypothesisId, const char* location, const char* message, const char* dataJson) {
	if (!hypothesisId || !location || !message)
		return;

	char dataObj[1024];
	WrapAgentDataJson(dataJson, dataObj, sizeof(dataObj));

	const unsigned long long ts = (unsigned long long)GetTickCount64();
	char line[2048];
	int n = std::snprintf(line, sizeof(line),
		"{\"sessionId\":\"af2ef9\",\"timestamp\":%llu,\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s}\n",
		ts, hypothesisId, location, message, dataObj);
	if (n <= 0 || n >= (int)sizeof(line))
		return;

	if (MvmDebugLog_Active()) {
		MvmDebugLog_LinefAlways("agent", "hyp=%s loc=%s msg=%s data=%s",
			hypothesisId, location, message, dataObj);
	}

	PostAgentLogAsync(line, n);

	static std::mutex s_agentMutex;
	std::lock_guard<std::mutex> lock(s_agentMutex);

	char envPath[MAX_PATH * 4] = {};
	DWORD envLen = GetEnvironmentVariableA("MVM_AGENT_LOG", envPath, (DWORD)sizeof(envPath));
	if (envLen == 0)
		envLen = GetEnvironmentVariableA("MVM_AGENT_DEBUG_LOG", envPath, (DWORD)sizeof(envPath));
	if (envLen == 0)
		envLen = GetEnvironmentVariableA("CURSOR_DEBUG_LOG", envPath, (DWORD)sizeof(envPath));
	if (envLen > 0 && envLen < sizeof(envPath)) {
		wchar_t pathW[MAX_PATH * 4] = {};
		if (MultiByteToWideChar(CP_UTF8, 0, envPath, -1, pathW, (int)(sizeof(pathW) / sizeof(pathW[0]))) > 0) {
			FILE* f = nullptr;
			if (_wfopen_s(&f, pathW, L"ab") == 0 && f) {
				fwrite(line, 1, (size_t)n, f);
				fclose(f);
			}
		}
	}
}

void MvmCrashWatch_Arm(int entityIndex, const char* model) {
	// Open a 30-second window during which the vectored handler records access violations. Re-armed on
	// every swap/re-fire, so it stays open across the post-swap animation frames where the crash lands.
	// Widened from 5s -> 30s after mvm_debug_20260629_125401.log: the crash landed at +5.24s (a delayed
	// weapon-switch/QQ pose, not the immediate post-swap frame) just outside the old window, so NO crash.veh
	// line was recorded. Self-module skip + same-RIP de-dup already keep the log from flooding over 30s.
	g_lastSwapIdx.store(entityIndex, std::memory_order_relaxed);
	if (model) {
		size_t i = 0;
		for (; model[i] && i + 1 < sizeof(g_lastSwapModel); ++i) g_lastSwapModel[i] = model[i];
		g_lastSwapModel[i] = '\0';
	}
	g_crashWatchUntilMs.store(GetTickCount64() + 30000, std::memory_order_release);
}

void MvmDebugLog_Command(advancedfx::ICommandArgs* args) {
	if (!args || !MvmDebugLog_Active())
		return;
	std::ostringstream command;
	for (int i = 0; i < args->ArgC(); ++i) {
		if (i > 0)
			command << ' ';
		const char* arg = args->ArgV(i);
		const bool quote = arg && (std::strchr(arg, ' ') || std::strchr(arg, '\t') || std::strchr(arg, '"'));
		if (quote)
			command << '"';
		for (const char* p = arg ? arg : ""; *p; ++p) {
			if (*p == '"' || *p == '\\')
				command << '\\';
			command << *p;
		}
		if (quote)
			command << '"';
	}
	MvmDebugLog_Linef("command", "%s", command.str().c_str());
}

void MvmDebugLog_ConsoleLine(const char* level, int devLevel, const char* text) {
	const std::string line = SingleLine(text);
	if (line.empty())
		return;
	char category[48];
	if (devLevel >= 0)
		snprintf(category, sizeof(category), "console.%s.dev%d", level ? level : "message", devLevel);
	else
		snprintf(category, sizeof(category), "console.%s", level ? level : "message");
#if !defined(GAME_DEADLOCK) && !defined(GAME_SOURCE1)
	// Every console line also goes to the bug report's always-on recorder, log file or not.
	BugReport_Record(category, line);
#endif
	if (!MvmDebugLog_Active())
		return;
	MvmDebugLog_Linef(category, "%s", line.c_str());
}

bool CosmeticsDebugLog_Start(std::string* outFile) {
	return MvmDebugLog_Start(outFile);
}

bool CosmeticsDebugLog_Stop(std::string* outFolder, std::string* outFile) {
	return MvmDebugLog_Stop(outFolder, outFile);
}

bool CosmeticsDebugLog_Active() {
	return MvmDebugLog_Active();
}

void CosmeticsDebugLog_Linef(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LineV("cosmetics", /*dedup=*/true, fmt, ap);
	va_end(ap);
}

} // namespace Filmmaker
