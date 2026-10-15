#pragma once

// Self-service debug logger for SOURCE:MVM. The implementation lives in the Filmmaker/Debug folder;
// it originated as a cosmetics-only logger, but the log now covers the complete
// filmmaker command/UI surface, console output, camera state, and cosmetics runtime events.
//
// Console: `mvm_debug start` / `mvm_debug stop|status` (short alias) OR
//          `mirv_filmmaker cosmetics debuglog start|stop|status`.
// Identical events are written once per bounded window (4096 entries / 4 MiB string payload).
// A repeat summary records count and first/last occurrence, including interleaved repeats.
// Pending summaries flush before the cache rolls; stopping releases all retained messages.
// Logs are written to %APPDATA%\HLAE\debuglogs\mvm_debug_<YYYYMMDD_HHMMSS>.log.

#include <cstdint>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct MvmDebugLogStats {
	uint64_t eventsReceived = 0;
	uint64_t uniqueEventsWritten = 0;
	uint64_t repeatsCombined = 0;
};

// Begins a new log file. Returns false if one is already running or the file could not be opened.
// On success, outFile (optional) receives the full UTF-8 file path.
bool MvmDebugLog_Start(std::string* outFile);

// Stops the active log. Returns false if none was running. On success, outFolder/outFile (optional)
// receive the UTF-8 folder and file paths (paste the folder into File Explorer).
bool MvmDebugLog_Stop(std::string* outFolder, std::string* outFile,
	MvmDebugLogStats* outStats = nullptr);

// True while a log is open.
bool MvmDebugLog_Active();

// Snapshot of the current compression counters. Safe to call from any thread.
MvmDebugLogStats MvmDebugLog_GetStats();

// Full CS2 tier0 console capture. Registration is deliberately deferred until mvm_debug starts so
// no logging-state APIs run while Windows is initializing tier0/engine2 under the loader lock.
bool MvmConsoleCapture_Start();
void MvmConsoleCapture_KeepAlive(); // whole-session capture for the bug report recorder
void MvmConsoleCapture_Stop();
bool MvmConsoleCapture_Active();

// Appends a categorized event. The category + exact payload form the deduplication identity.
void MvmDebugLog_Linef(const char* category, const char* fmt, ...);

// Like MvmDebugLog_Linef but NEVER deduplicated -- every call writes its own line immediately. Use
// for discrete user actions (e.g. a UI skin click) where each occurrence matters even when its
// payload repeats an earlier one; the normal dedup would hide repeat clicks until the log is stopped.
void MvmDebugLog_LinefAlways(const char* category, const char* fmt, ...);

// Cursor/agent NDJSON hook (see MvmAgentLog in MvmDebugLog.cpp). dataJson must be a JSON object
// fragment like "\"key\":1" or empty/null for {}.
void MvmAgentLog(const char* hypothesisId, const char* location, const char* message, const char* dataJson);

// Crash breadcrumb: a vectored exception handler (registered while a log is open) logs first-chance
// ACCESS VIOLATIONS -- faulting module+offset, access type, thread, and the last knife swap context --
// but ONLY during the short window after MvmCrashWatch_Arm() is called. The step breadcrumbs only
// cover OUR calls; these crashes fault downstream in the engine while it animates/renders a swapped
// model, so the LAST "crash.veh" line before the process dies pinpoints WHERE that fault is. Arm() is
// called by the knife model swap and glove apply right before they touch entities, so the window covers the
// post-swap animation/render frames without logging unrelated handled exceptions the rest of the time.
void MvmCrashWatch_Arm(int entityIndex, const char* model);

// Logs an engine-tokenized command without losing quoted/space-containing arguments.
void MvmDebugLog_Command(advancedfx::ICommandArgs* args);

// Captures text emitted through the Source 2 advancedfx console wrapper.
void MvmDebugLog_ConsoleLine(const char* level, int devLevel, const char* text);

// Compatibility aliases for the original cosmetics-only call sites.
bool CosmeticsDebugLog_Start(std::string* outFile);
bool CosmeticsDebugLog_Stop(std::string* outFolder, std::string* outFile);
bool CosmeticsDebugLog_Active();
void CosmeticsDebugLog_Linef(const char* fmt, ...);

} // namespace Filmmaker
