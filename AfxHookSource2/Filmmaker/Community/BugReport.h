#pragma once

// "Report Bugs": one plain-text report about what went wrong, always saved locally and sent to the
// developer's Discord channel (BugReportSend) only after the user has reviewed it and pressed Send.
// It holds what the user typed, an optional Discord name they typed, the build and system facts a bug needs,
// the always-on recorder's recent history (console output and SOURCE:MVM events), the end of the
// newest mvm_debug log and CS2's console.log, and the names of recent crash dumps.
// Size and text rules: BugReportMath.h.

#include <string>

namespace Filmmaker {

// Who is reporting: only what they chose to type. No Steam name or ID is ever read or sent.
struct BugReportSender {
	std::string discord;     // optional contact the user typed, UTF-8
};

// An automatic crash report (CrashReport.cpp) instead of a typed one.
struct BugReportCrash {
	std::string summary;      // one line: "ACCESS_VIOLATION at engine2.dll+0x35630"
	std::string details;      // the crash header: exception, stack, map, demo
	std::string baseName;     // "SourceMVM-crash-<time>": the report is written as <baseName>.txt
	std::string map, demo;    // as known when it crashed
	// Sent on a later launch: the live map/demo/health belong to the NEW session, so they are left
	// out, and `history` (the dead session's on-disk log) replaces the in-memory recorder.
	bool previousSession = false;
	std::string history;
};

struct BugReportResult {
	bool ok = false;
	std::string crash;      // crash summary; empty for a typed report
	std::wstring path;      // the written file
	std::string fileName;   // its name, UTF-8, for the UI
	std::string error;      // why it failed, UTF-8
	bool hadDebugLog = false;
	size_t bytes = 0;
	// The facts, again, for the Discord message itself (UTF-8).
	std::string description, map, demo, cs2Build, hookBuild, windows, health;
	BugReportSender sender;
	std::wstring dumpPath;  // a crash's minidump, attached when the player sends it; empty for none
};

// Writes the report to %APPDATA%\HLAE\bugreports\ and returns where. `description` is UTF-8.
// With `crash`, it is a crash report: its details come first and engine state is not queried
// (the caller may be a worker thread of a crashed process).
BugReportResult BugReport_Write(const std::string& description, const BugReportSender& sender,
	const BugReportCrash* crash = nullptr);

// This hook DLL's file name ("AfxHookSource2.dll") and its symbol-store key (CrashReport::
// SymbolStoreKey of its own PE header). Reports carry both so a frame can be resolved against the
// archived PDB of that exact build.
std::string BugReport_HookModuleName();
std::string BugReport_HookImageKey();

// `text` without anything that names the player: every Windows user folder becomes %USERPROFILE%
// (BugReport::ScrubUserPaths), and Steam IDs and the local Steam name become placeholders
// (BugReport::ScrubSteamIdentity). Applied to every report file when written and again when sent.
std::string BugReport_ScrubPrivate(const std::string& text);
// Main thread, now and then: remembers the local Steam name (CS2's `name` cvar) so the scrub can
// remove it, including from a crash report written on another thread.
void BugReport_NoteLocalPlayerName();

// The folder reports are written to, with a trailing backslash.
std::wstring BugReport_Folder();

// The always-on flight recorder: every console line and SOURCE:MVM event lands here (thread-safe),
// keeping the most recent ~768 KB in memory, so a report has history even when nobody turned on
// mvm_debug. `category` is a short tag ("console.message", "engine", ...).
// Each recorded line is also mirrored to the session log on disk (CrashReport), so a session that
// dies without a crash handler running still leaves its history behind.
void BugReport_Record(const char* category, const std::string& line);

} // namespace Filmmaker
