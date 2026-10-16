#pragma once

// Crash reports. When CS2 crashes with SOURCE:MVM loaded, a report is written to the player's PC.
// It is never sent by itself: on the next launch the main menu shows what was saved and asks
// whether to send it to the developer's Discord (BugReportSend). The report carries no Steam
// name or ID.
//
//  - Caught at crash time: unhandled exceptions (access violations and the like, kept on top of any
//    filter CS2 installs later and chained to it), abort() / SIGABRT, and CRT invalid parameters.
//    The handler first writes a small header (exception, faulting module+offset, stack as
//    module+offset, map, demo) without touching the heap, then writes the full report, waiting at
//    most ~25 s.
//  - Written on the next launch: any report the crashed process could not finish, and an
//    "unexpected exit" when the previous session vanished without either a clean exit or a caught
//    crash (fast-fail, killed from Task Manager, power loss). Its history comes from the session
//    log, an on-disk mirror of the always-on recorder.
//
// Files live in %APPDATA%\HLAE\bugreports\: <base>.pending (awaiting the player's decision),
// <base>.txt (report), <base>.declined / <base>.unsent (kept, never offered), <base>.session.log,
// session.lock, session.log. Rules: CrashReportMath.h. Contract: Docs/features/filmmaker_community.md.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// DllMain (attach): installs the crash handlers. Kernel32-only work, safe under the loader lock.
void CrashReport_InstallHandlers();

// Every main-thread frame (Community_RunMainThreadFrame). The first call starts the session
// (lock, session log, previous unexpected exit) and writes earlier sessions' reports out for the
// prompt; later calls remember the map and demo for a crash.
void CrashReport_MainThreadFrame();

// Reports from earlier sessions waiting for the player's decision, for the main menu prompt.
struct CrashReportPending {
	int count = 0;                // offered reports (their .txt is written)
	std::wstring newestReport;    // full path of the newest one, to show in Explorer
	std::string newestFile;       // its file name, UTF-8
	std::string newestSummary;    // one line: what happened
	bool hasDump = false;         // a minidump sits beside it and goes along with Send
	std::string send;             // "", "sending", "sent", "failed", "declined"
	std::string sendDetail;       // why a send failed
};
CrashReportPending CrashReport_Pending();
// The player pressed Send / Don't send on the prompt. Send runs on a worker thread.
void CrashReport_SendPending();
void CrashReport_DeclinePending();

// One recorder line, mirrored to the session log. Called by BugReport_Record under its lock.
void CrashReport_SessionLogLine(const std::string& line);

// Third-party graphics code found in cs2.exe (ReShade and its add-ons, overlays), as last scanned
// by the main-thread frame: "" when none. For typed bug reports.
std::string CrashReport_InjectedModules();

// The heartbeat's p95 frame time (ms) over `frames` frames. A session that crawled is still reported
// when the player then quits normally: that is how most "it crashed" reports actually ended. Startup
// alone never counts (CrashReportMath IsSlowHeartbeat).
void CrashReport_NoteFrameTime(double p95Ms, unsigned frames);

// DllMain (detach), client shutdown and a self-terminate: the session ended on purpose, not a
// crash. A session that crawled keeps its lock, marked as a clean quit, for the next launch.
void CrashReport_CleanExit();

// mirv_filmmaker community crash <status|test|prune|now>
void CrashReport_Command(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
