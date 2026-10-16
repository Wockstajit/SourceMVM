// Behavior tests for Community/CrashReportMath.h: automatic crash report rules.
#include "../../AfxHookSource2/Filmmaker/Community/CrashReportMath.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

using namespace Filmmaker::CrashReport;

void FramesNameModuleAndOffset() {
	char out[128];
	// The crash the user kept hitting: the module-relative offset is what matches across launches.
	FormatFrame(out, sizeof(out), "engine2.dll", 0x7FFB12345630ull, 0x7FFB12310000ull);
	CHECK(std::string(out) == "engine2.dll+0x35630");
	FormatFrame(out, sizeof(out), "", 0x1234ull, 0);
	CHECK(std::string(out) == "0x0000000000001234 (no module)");
	// A tiny buffer truncates instead of overflowing.
	char tiny[8];
	FormatFrame(tiny, sizeof(tiny), "engine2.dll", 0x10, 0);
	CHECK(std::string(tiny).size() == 7);
}

void SymbolKeysMatchTheSymbolStore() {
	char key[32];
	// Microsoft's layout: 8-digit upper-case link stamp, then the image size in lower-case hex.
	SymbolStoreKey(key, sizeof(key), 0x68ED90B7u, 0xE2A000u);
	CHECK(std::string(key) == "68ED90B7e2a000");
	SymbolStoreKey(key, sizeof(key), 0x1u, 0x1000u);
	CHECK(std::string(key) == "000000011000");
}

void OwnFramesAreFoundByModule() {
	CHECK(FrameInModule("AfxHookSource2.dll+0x4e24e0", "AfxHookSource2.dll"));
	CHECK(FrameInModule("afxhooksource2.DLL+0x10", "AfxHookSource2.dll"));
	// Another module whose name merely starts the same is not ours.
	CHECK(!FrameInModule("AfxHookSource2.dll.bak+0x10", "AfxHookSource2.dll"));
	CHECK(!FrameInModule("d3d11.dll+0x168d0c", "AfxHookSource2.dll"));
	CHECK(!FrameInModule("0x0000000000001234 (no module)", "AfxHookSource2.dll"));
	CHECK(!FrameInModule("AfxHookSource2.dll+0x10", ""));
}

void ExceptionCodesHaveNames() {
	CHECK(std::string(ExceptionName(0xC0000005u)) == "ACCESS_VIOLATION");
	CHECK(std::string(ExceptionName(0xC0000409u)).find("FAST_FAIL") != std::string::npos);
	CHECK(std::string(ExceptionName(0x12345678u)) == "UNKNOWN");
	CHECK(std::string(AccessKind(0)) == "read");
	CHECK(std::string(AccessKind(1)) == "write");
	CHECK(std::string(AccessKind(8)) == "execute");
}

void BaseNamesSortByTime() {
	char a[96], b[96];
	BaseName(a, sizeof(a), "crash", 2026, 10, 10, 22, 0, 57);
	CHECK(std::string(a) == "SourceMVM-crash-2026-10-10_22-00-57");
	BaseName(b, sizeof(b), "exit", 2026, 10, 9, 9, 5, 3);
	CHECK(std::string(b) == "SourceMVM-exit-2026-10-09_09-05-03");
}

void LockRoundTrips() {
	uint32_t pid = 0;
	int64_t start = 0;
	CHECK(ParseLock(FormatLock(50076, 1791687657), pid, start));
	CHECK(pid == 50076 && start == 1791687657);
	CHECK(!ParseLock("", pid, start));
	CHECK(!ParseLock("garbage", pid, start));
	CHECK(!ParseLock("pid=0 start=5", pid, start)); // no process has pid 0
}

void StartupFramesAreNotACrawl() {
	// User report 2026-10-15: the first heartbeat held 108 frames, 26 of them CS2's startup load
	// (one 70 s); its p95 of 240-495 ms made a normal quit 26 minutes later arrive as a crash.
	CHECK(!IsSlowHeartbeat(495.0, 108));
	CHECK(!IsSlowHeartbeat(6303.0, 12));
	// Once the window is full of frames, a slow p95 is the game crawling.
	CHECK(IsSlowHeartbeat(371.0, 4096));
	CHECK(IsSlowHeartbeat(kSlowSessionP95Ms, kSlowSessionMinFrames));
	CHECK(!IsSlowHeartbeat(kSlowSessionP95Ms - 1.0, 4096));
	CHECK(!IsSlowHeartbeat(1000.0, kSlowSessionMinFrames - 1));
}

void UnexpectedExitNeedsAGoneOtherProcess() {
	// Lock left behind by a process that no longer runs: the session died without a clean exit.
	CHECK(IsUnexpectedExit(true, true, 100, false, 200));
	// No lock: the last session exited cleanly or the crash handler already reported it.
	CHECK(!IsUnexpectedExit(false, false, 0, false, 200));
	// Unreadable lock: nothing to say about it.
	CHECK(!IsUnexpectedExit(true, false, 0, false, 200));
	// That process is still running (a second CS2): not an exit.
	CHECK(!IsUnexpectedExit(true, true, 100, true, 200));
	// Our own pid (the lock was rewritten by this session): not an exit.
	CHECK(!IsUnexpectedExit(true, true, 200, false, 200));
}

void LaunchSendsOnlyTheNewestFew() {
	const std::vector<std::string> pending = {
		"SourceMVM-crash-2026-10-09_15-00-00",
		"SourceMVM-exit-2026-10-10_22-00-57",
		"SourceMVM-crash-2026-10-10_09-00-00",
		"SourceMVM-crash-2026-10-08_01-00-00",
		"SourceMVM-crash-2026-10-10_21-03-00",
	};
	const auto picked = PickPendingToSend(pending);
	CHECK(picked.size() == size_t(kMaxSendsPerLaunch));
	// Newest first, across both kinds.
	CHECK(picked[0] == "SourceMVM-exit-2026-10-10_22-00-57");
	CHECK(picked[1] == "SourceMVM-crash-2026-10-10_21-03-00");
	CHECK(picked[2] == "SourceMVM-crash-2026-10-10_09-00-00");
	CHECK(PickPendingToSend({}).empty());
}

void AppendNeverOverflows() {
	char buf[10];
	size_t len = 0;
	buf[0] = '\0';
	len = Append(buf, sizeof(buf), len, "hello ");
	len = Append(buf, sizeof(buf), len, "world");
	CHECK(len == 9 && std::string(buf) == "hello wor");
	len = Append(buf, sizeof(buf), len, "more");
	CHECK(len == 9 && std::string(buf) == "hello wor");
}

void CsDumpAtTheLastLogLineMeansACrash() {
	// User report 2026-10-12: last log line 19:04:55, cs2_..._190455_0_accessviolation.mdmp.
	const int64_t start = 1000, lastLog = 1210;
	CHECK(DumpBelongsToSession(1210, start, lastLog));
	CHECK(DumpBelongsToSession(1210 + kDumpMatchSeconds, start, lastLog));
	// An old dump from 17 days ago is not this session's.
	CHECK(!DumpBelongsToSession(1210 - 17 * 86400, start, lastLog));
	CHECK(!DumpBelongsToSession(999, start, lastLog));
	CHECK(!DumpBelongsToSession(1210 + kDumpMatchSeconds + 1, start, lastLog));
}

void DumpKindComesFromTheFileName() {
	CHECK(DumpKind("cs2_2026_1012_190455_0_accessviolation.mdmp") == "accessviolation");
	CHECK(DumpKind("cs2_2026_0606_213502_0_intdividebyzero.mdmp") == "intdividebyzero");
	CHECK(DumpKind("cs2_2026_1012_190455_0.mdmp").empty());
	CHECK(DumpKind("crash.mdmp").empty());
}

void NotableModulesAreNamed() {
	const std::string sys = "c:\\windows\\system32";
	CHECK(NotableModule("c:\\steam\\cs2\\game\\bin\\win64\\renodx-dlss.addon64", sys) == "ReShade add-on");
	CHECK(NotableModule("c:\\steam\\cs2\\game\\bin\\win64\\dxgi.dll", sys) == "graphics wrapper (ReShade?)");
	CHECK(NotableModule("c:\\windows\\system32\\dxgi.dll", sys).empty());
	CHECK(NotableModule("c:\\program files (x86)\\rivatuner statistics server\\rtsshooks64.dll", sys) == "RivaTuner / Afterburner overlay");
	CHECK(NotableModule("c:\\users\\a\\appdata\\local\\discord\\discordhook64.dll", sys) == "Discord overlay");
	CHECK(NotableModule("c:\\windows\\system32\\kernel32.dll", sys).empty());
	CHECK(NotableModule("c:\\steam\\cs2\\game\\bin\\win64\\engine2.dll", sys).empty());
}

void WindowsEventsNameTheFault() {
	const std::string error =
		"<Event><System><EventID>1000</EventID></System><EventData><Data Name='AppName'>cs2.exe</Data>"
		"<Data Name='ModuleName'>engine2.dll</Data><Data Name='ExceptionCode'>c0000409</Data>"
		"<Data Name='FaultingOffset'>0000000000035630</Data><Data Name='ProcessId'>0xbcd8</Data></EventData></Event>";
	CHECK(WindowsAppEventLine(error, 0xbcd8) ==
		"Windows   : Application Error (Event 1000), STACK_BUFFER_OVERRUN / FAST_FAIL at engine2.dll+0x35630 (0xc0000409)");
	CHECK(WindowsAppEventLine(error, 0xbcd9).empty()); // another process's crash
	// A reused process ID: another program's crash with CS2's old ID is still not CS2's.
	std::string other = error;
	other.replace(other.find("cs2.exe"), 7, "notepad.exe");
	CHECK(WindowsAppEventLine(other, 0xbcd8).empty());
	std::string upper = error;
	upper.replace(upper.find("cs2.exe"), 7, "CS2.EXE");
	CHECK(!WindowsAppEventLine(upper, 0xbcd8).empty());
	CHECK(WindowsAppEventLine(error, 0).empty());
	const std::string hang =
		"<Event><System><EventID>1002</EventID></System><EventData><Data Name=\"ProcessId\">0x38a0</Data>"
		"<Data Name=\"HangType\">Top level window is idle</Data></EventData></Event>";
	CHECK(WindowsAppEventLine(hang, 0x38a0) ==
		"Windows   : Application Hang (Event 1002), the game stopped responding and was ended (Top level window is idle)");
	CHECK(WindowsAppEventLine("<Event><System><EventID>1001</EventID></System></Event>", 1).empty());
	CHECK(EventDataValue(error, "Missing").empty());
}

} // namespace

void OldReportsArePruned() {
	const uint64_t MB = 1024 * 1024;
	std::vector<KeptReport> reports;
	for (int day = 1; day <= 14; ++day) {
		char base[64];
		snprintf(base, sizeof(base), "SourceMVM-%s-2026-10-%02d_12-00-00", day % 2 ? "crash" : "bug", day);
		reports.push_back({base, 1 * MB, false});
	}
	// 14 small reports: the 4 oldest go, newest first across both kinds.
	auto prune = PickReportsToPrune(reports);
	CHECK(prune.size() == 4);
	CHECK(std::find(prune.begin(), prune.end(), "SourceMVM-crash-2026-10-01_12-00-00") != prune.end());
	CHECK(std::find(prune.begin(), prune.end(), "SourceMVM-bug-2026-10-04_12-00-00") != prune.end());
	CHECK(std::find(prune.begin(), prune.end(), "SourceMVM-bug-2026-10-14_12-00-00") == prune.end());
	// One the player has not decided on survives however old, and takes one of the ten places.
	reports[0].awaitingDecision = true; // day 1
	prune = PickReportsToPrune(reports);
	CHECK(prune.size() == 4);
	CHECK(std::find(prune.begin(), prune.end(), "SourceMVM-crash-2026-10-01_12-00-00") == prune.end());
	CHECK(std::find(prune.begin(), prune.end(), "SourceMVM-crash-2026-10-05_12-00-00") != prune.end());
	// The size budget: three 90 MB reports, only two fit in 200 MB.
	const std::vector<KeptReport> big = {
		{"SourceMVM-crash-2026-10-03_00-00-00", 90 * MB, false},
		{"SourceMVM-crash-2026-10-02_00-00-00", 90 * MB, false},
		{"SourceMVM-crash-2026-10-01_00-00-00", 90 * MB, false},
	};
	prune = PickReportsToPrune(big);
	CHECK(prune.size() == 1 && prune[0] == "SourceMVM-crash-2026-10-01_00-00-00");
	CHECK(PickReportsToPrune({}).empty());
}

int main() {
	OldReportsArePruned();
	WindowsEventsNameTheFault();
	CsDumpAtTheLastLogLineMeansACrash();
	DumpKindComesFromTheFileName();
	NotableModulesAreNamed();
	FramesNameModuleAndOffset();
	SymbolKeysMatchTheSymbolStore();
	OwnFramesAreFoundByModule();
	ExceptionCodesHaveNames();
	BaseNamesSortByTime();
	LockRoundTrips();
	UnexpectedExitNeedsAGoneOtherProcess();
	StartupFramesAreNotACrawl();
	LaunchSendsOnlyTheNewestFew();
	AppendNeverOverflows();
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
	std::printf("crash-report-math-tests: all passed\n");
	return 0;
}
