#pragma once

// Engine-free rules for automatic crash reports (CrashReport.cpp): exception names, the crash
// header's frame lines, file names, the session lock, and which pending reports a launch sends.
// Tested by Automation/tests/crash-report-math-tests.cpp. Everything here that the crash handler
// itself calls writes into caller-owned char buffers: the handler runs on a crashed process and must
// not touch the heap.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::CrashReport {

constexpr int kMaxFrames = 48;                         // stack frames in the crash header
constexpr size_t kHeaderBytes = 16 * 1024;             // the whole crash header
constexpr size_t kSessionLogMaxBytes = 32 * 1024 * 1024; // the on-disk mirror of the recorder
constexpr size_t kSessionLogBudget = 768 * 1024;       // its tail, in a next-launch report
constexpr int kMaxSendsPerLaunch = 3;                  // older pending reports are kept, not sent
constexpr double kSlowSessionP95Ms = 250.0;            // a heartbeat p95 this slow: the game crawled
constexpr unsigned kSlowSessionMinFrames = 600;        // ...over at least this many frames
constexpr int kMaxKeptReports = 10;                    // reports kept on disk (all their files)
constexpr uint64_t kMaxKeptReportBytes = 200ull * 1024 * 1024; // and at most this much of them

// Readable names for the exception codes a crash can carry.
inline const char* ExceptionName(uint32_t code) {
	switch (code) {
	case 0xC0000005u: return "ACCESS_VIOLATION";
	case 0xC000001Du: return "ILLEGAL_INSTRUCTION";
	case 0xC0000094u: return "INTEGER_DIVIDE_BY_ZERO";
	case 0xC0000095u: return "INTEGER_OVERFLOW";
	case 0xC00000FDu: return "STACK_OVERFLOW";
	case 0xC0000006u: return "IN_PAGE_ERROR";
	case 0xC0000008u: return "INVALID_HANDLE";
	case 0xC0000374u: return "HEAP_CORRUPTION";
	case 0xC0000409u: return "STACK_BUFFER_OVERRUN / FAST_FAIL";
	case 0xC000041Du: return "FATAL_USER_CALLBACK_EXCEPTION";
	case 0x80000003u: return "BREAKPOINT";
	case 0xE06D7363u: return "C++ EXCEPTION (uncaught throw)";
	case 0xE0434352u: return ".NET EXCEPTION";
	default: return "UNKNOWN";
	}
}

// "engine2.dll+0x35630", or the raw address when no module owns it.
inline void FormatFrame(char* out, size_t cap, const char* module, uint64_t address, uint64_t moduleBase) {
	if (module && *module)
		snprintf(out, cap, "%s+0x%llx", module, (unsigned long long)(address - moduleBase));
	else
		snprintf(out, cap, "0x%016llx (no module)", (unsigned long long)address);
}

// Microsoft symbol-store key of a PE image: link stamp as 8 hex digits, then the image size
// ("%08X%x"). The release script archives each hook DLL and its PDB under this key
// (Automation/symbols), and Automation/tools/discord-reports.py resolves report frames with it.
inline void SymbolStoreKey(char* out, size_t cap, uint32_t timeDateStamp, uint32_t sizeOfImage) {
	snprintf(out, cap, "%08X%x", timeDateStamp, sizeOfImage);
}

// Whether a formatted frame ("AfxHookSource2.dll+0x4e24e0") lies in `module`. Case-insensitive,
// like Windows module names, and anchored on the '+' so "a.dll" never matches "ba.dll+0x1".
inline bool FrameInModule(const char* frame, const char* module) {
	if (!frame || !module || !*module) return false;
	const size_t n = strlen(module);
	for (size_t i = 0; i < n; ++i) {
		const char a = frame[i], b = module[i];
		if (!a) return false;
		if ((a >= 'A' && a <= 'Z' ? a + 32 : a) != (b >= 'A' && b <= 'Z' ? b + 32 : b)) return false;
	}
	return frame[n] == '+';
}

// What an access violation tried to do, from ExceptionInformation[0].
inline const char* AccessKind(uint64_t info0) {
	return info0 == 0 ? "read" : info0 == 1 ? "write" : info0 == 8 ? "execute" : "access";
}

// "SourceMVM-crash-2026-10-10_22-00-57" (no extension). The report is <base>.txt; the header that
// marks it unsent is <base>.pending.
inline void BaseName(char* out, size_t cap, const char* kind, int year, int month, int day, int hour, int minute, int second) {
	snprintf(out, cap, "SourceMVM-%s-%04d-%02d-%02d_%02d-%02d-%02d", kind, year, month, day, hour, minute, second);
}

// session.lock holds "pid=<pid> start=<unix seconds>".
inline std::string FormatLock(uint32_t pid, int64_t startUnix) {
	return "pid=" + std::to_string(pid) + " start=" + std::to_string(startUnix);
}

inline bool ParseLock(const std::string& text, uint32_t& pid, int64_t& startUnix) {
	unsigned long p = 0;
	long long s = 0;
	if (sscanf(text.c_str(), "pid=%lu start=%lld", &p, &s) != 2 || p == 0) return false;
	pid = uint32_t(p);
	startUnix = s;
	return true;
}

// Whether a heartbeat says the game crawled. Its frame window is the last 4096 presents since the hook
// loaded, so the first heartbeats hold only CS2's startup: a few long loading frames among a hundred
// set their p95. Those made normal quits arrive as crash reports (user reports 2026-10-15: one 70 s
// and one 11 s startup frame, then normal play). A real crawl fills the window with slow frames.
inline bool IsSlowHeartbeat(double p95Ms, unsigned frames) {
	return frames >= kSlowSessionMinFrames && p95Ms >= kSlowSessionP95Ms;
}

// A previous session ended without a clean exit and without the crash handler noticing: it left
// its lock behind, that process is gone, and it is not this process (pid reuse).
inline bool IsUnexpectedExit(bool lockFound, bool lockParsed, uint32_t lockPid, bool lockPidAlive, uint32_t currentPid) {
	return lockFound && lockParsed && lockPid != currentPid && !lockPidAlive;
}

// Which pending reports a launch sends: the newest kMaxSendsPerLaunch, newest first. Names sort by
// time because BaseName is zero-padded; "crash" and "exit" kinds sort on the time part.
inline std::vector<std::string> PickPendingToSend(std::vector<std::string> baseNames) {
	auto timePart = [](const std::string& n) {
		const size_t dash = n.find('-', strlen("SourceMVM-"));
		return dash == std::string::npos ? n : n.substr(dash + 1);
	};
	std::sort(baseNames.begin(), baseNames.end(), [&](const std::string& a, const std::string& b) {
		return timePart(a) > timePart(b);
	});
	if (baseNames.size() > size_t(kMaxSendsPerLaunch)) baseNames.resize(kMaxSendsPerLaunch);
	return baseNames;
}

// One report on disk: every file that shares its base name (.txt, .pending, .dmp, .session.log, ...).
struct KeptReport {
	std::string base;        // "SourceMVM-crash-2026-10-14_09-34-39"
	uint64_t bytes = 0;      // all its files together
	bool awaitingDecision;   // a .pending: the player has not chosen yet, so it is never pruned
};

// Which reports to delete so the folder stays small: the newest kMaxKeptReports, within
// kMaxKeptReportBytes, survive; reports still awaiting the player's decision always survive (and
// count toward both limits). A single report larger than the whole budget is not kept unless it is
// awaiting a decision.
inline std::vector<std::string> PickReportsToPrune(std::vector<KeptReport> reports) {
	auto timePart = [](const std::string& n) {
		const size_t dash = n.find('-', strlen("SourceMVM-"));
		return dash == std::string::npos ? n : n.substr(dash + 1);
	};
	std::sort(reports.begin(), reports.end(), [&](const KeptReport& a, const KeptReport& b) {
		return timePart(a.base) > timePart(b.base);
	});
	int kept = 0;
	uint64_t bytes = 0;
	for (const KeptReport& r : reports)
		if (r.awaitingDecision) { ++kept; bytes += r.bytes; }
	std::vector<std::string> prune;
	for (const KeptReport& r : reports) {
		if (r.awaitingDecision) continue;
		if (kept < kMaxKeptReports && bytes + r.bytes <= kMaxKeptReportBytes) { ++kept; bytes += r.bytes; }
		else prune.push_back(r.base);
	}
	return prune;
}

// CS2 writes its own dump (cs2_<date>_<time>_0_<kind>.mdmp) for most crashes, sometimes without our
// handler ever running. A dump that the dead session wrote close to its last log line means that
// session CRASHED; it was not a hang, a kill or a power loss. Times are unix seconds.
constexpr int64_t kDumpMatchSeconds = 120;
inline bool DumpBelongsToSession(int64_t dumpWrite, int64_t sessionStart, int64_t lastLogWrite) {
	if (dumpWrite < sessionStart) return false;
	const int64_t gap = dumpWrite > lastLogWrite ? dumpWrite - lastLogWrite : lastLogWrite - dumpWrite;
	return gap <= kDumpMatchSeconds;
}

// "accessviolation" from "cs2_2026_1012_190455_0_accessviolation.mdmp"; empty when the name has no kind.
inline std::string DumpKind(const std::string& fileName) {
	const size_t dot = fileName.rfind('.');
	const std::string stem = fileName.substr(0, dot);
	const size_t underscore = stem.rfind('_');
	if (underscore == std::string::npos || underscore + 1 >= stem.size()) return {};
	const std::string kind = stem.substr(underscore + 1);
	return kind.find_first_not_of("0123456789") == std::string::npos ? std::string() : kind;
}

// Third-party code inside cs2.exe that crash and hang reports must name: graphics wrappers, ReShade
// add-ons and overlays all hook the same swapchain and device we do. `path` is the module's full
// path, lowercase; `systemDir` is the lowercase Windows system folder. Returns "" for anything else.
inline std::string NotableModule(const std::string& path, const std::string& systemDir) {
	const size_t slash = path.find_last_of("\\/");
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
	auto ends = [&](const char* suffix) {
		const size_t n = strlen(suffix);
		return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
	};
	if (ends(".addon64") || ends(".addon")) return "ReShade add-on";
	// A D3D/DXGI dll loaded from anywhere but the system folder is a wrapper (ReShade, usually).
	if ((name == "dxgi.dll" || name == "d3d11.dll" || name == "d3d12.dll" || name == "opengl32.dll") && dir != systemDir)
		return "graphics wrapper (ReShade?)";
	if (name.rfind("reshade", 0) == 0) return "ReShade";
	if (name.rfind("rtsshooks", 0) == 0) return "RivaTuner / Afterburner overlay";
	if (name.rfind("discordhook", 0) == 0) return "Discord overlay";
	if (name.rfind("graphics-hook", 0) == 0) return "OBS game capture";
	if (name.rfind("nvspcap", 0) == 0) return "NVIDIA overlay";
	if (name.find("overwolf") != std::string::npos) return "Overwolf";
	if (name.rfind("gameoverlayrenderer", 0) == 0) return "Steam overlay";
	return {};
}

// <Data Name='key'>value</Data> from a Windows event's XML; "" when absent.
inline std::string EventDataValue(const std::string& xml, const char* key) {
	for (const char quote : {'\'', '"'}) {
		const std::string open = std::string("<Data Name=") + quote + key + quote + ">";
		const size_t at = xml.find(open);
		if (at == std::string::npos) continue;
		const size_t from = at + open.size(), to = xml.find("</Data>", from);
		return to == std::string::npos ? std::string() : xml.substr(from, to - from);
	}
	return {};
}

// One line for a report from the Application log's "Application Error" (1000) or "Application Hang"
// (1002) event, when it is about process `pid`. These name the faulting module and offset for exits our
// handler never saw (fast-fail, heap corruption, a hang Windows ended). "" for any other event.
inline std::string WindowsAppEventLine(const std::string& xml, uint32_t pid) {
	const size_t idAt = xml.find("<EventID");
	if (idAt == std::string::npos) return {};
	const size_t idFrom = xml.find('>', idAt);
	if (idFrom == std::string::npos) return {};
	const int id = atoi(xml.c_str() + idFrom + 1);
	if (id != 1000 && id != 1002) return {};
	if (strtoul(EventDataValue(xml, "ProcessId").c_str(), nullptr, 0) != pid || pid == 0) return {};
	// Windows reuses process IDs: an event that names another program is never CS2's, even on a
	// matching ID. (Some hang events carry no AppName; the ID and time window still bound those.)
	std::string app = EventDataValue(xml, "AppName");
	for (char& c : app) c = (char)tolower((unsigned char)c);
	if (!app.empty() && app != "cs2.exe") return {};
	if (id == 1002) {
		const std::string type = EventDataValue(xml, "HangType");
		return "Windows   : Application Hang (Event 1002), the game stopped responding and was ended"
			+ (type.empty() ? std::string() : " (" + type + ")");
	}
	const uint32_t code = uint32_t(strtoul(EventDataValue(xml, "ExceptionCode").c_str(), nullptr, 16));
	const unsigned long long offset = strtoull(EventDataValue(xml, "FaultingOffset").c_str(), nullptr, 16);
	char line[256];
	snprintf(line, sizeof(line), "Windows   : Application Error (Event 1000), %s at %s+0x%llx (0x%08x)",
		ExceptionName(code), EventDataValue(xml, "ModuleName").c_str(), offset, code);
	return line;
}

// Appends `text` to a fixed buffer, always NUL-terminated, never overflowing. Returns the new length.
inline size_t Append(char* buf, size_t cap, size_t len, const char* text) {
	if (!buf || cap == 0 || len >= cap - 1) return len;
	const size_t room = cap - 1 - len;
	const size_t n = (std::min)(room, strlen(text));
	memcpy(buf + len, text, n);
	buf[len + n] = '\0';
	return len + n;
}

} // namespace Filmmaker::CrashReport
