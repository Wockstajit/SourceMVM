#include "stdafx.h"

// The Source 1 hook's side of the shared mvm_debug logger (AfxHookSource2/Filmmaker/Debug): the
// host answers MvmDebugLog.cpp asks for, console capture through SourceConsole, and the mvm_debug
// command with CS2's subcommands and messages.

#include "SourceConsole.h"
#include "SourceEngine.h"

#include "hlaeFolder.h"
#include "MirvTime.h"
#include "WrpConsole.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLogHost.h>
#include <Shared/AfxConsole.h>

#include <string>

extern WrpVEngineClient* g_VEngineClient;

namespace Filmmaker {

std::wstring MvmDebugLogHost_RoamingFolder() {
	return GetHlaeRoamingAppDataFolderW();
}

bool MvmDebugLogHost_DemoTick(int& outTick) {
	if (!g_VEngineClient || !g_VEngineClient->IsPlayingDemo()) return false;
	WrpVEngineClientDemoInfoEx* demo = g_VEngineClient->GetDemoInfoEx();
	if (!demo) return false;
	outTick = demo->GetDemoPlaybackTick();
	return true;
}

int MvmDebugLogHost_FrameCount() {
	return g_MirvTime.GetFrameCount();
}

bool MvmConsoleCapture_Start() {
	if (!SourceFilmmaker::Console_Install()) return false;
	SourceFilmmaker::Console_SetDebugLogEnabled(true);
	return true;
}

// The Source 1 hook has no bug-report recorder that would keep the capture alive.
void MvmConsoleCapture_KeepAlive() {}

void MvmConsoleCapture_Stop() {
	SourceFilmmaker::Console_SetDebugLogEnabled(false);
}

bool MvmConsoleCapture_Active() {
	return SourceFilmmaker::Console_DebugLogEnabled() && SourceFilmmaker::Console_Captured();
}

} // namespace Filmmaker

CON_COMMAND(mvm_debug, "Control the SOURCE:MVM debug log (mvm_debug start|stop|status).")
{
	const int argc = args->ArgC();
	const char* mode = (argc >= 2) ? args->ArgV(1) : "";
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Command(args);
	if (0 == _stricmp(mode, "start")) {
		std::string file;
		if (Filmmaker::MvmDebugLog_Start(&file)) {
			const bool consoleCapture = Filmmaker::MvmConsoleCapture_Start();
			Filmmaker::MvmDebugLog_Linef("command", "mvm_debug start");
			Filmmaker::MvmDebugLog_Linef("system", "fullConsoleCapture=%d", consoleCapture ? 1 : 0);
			SourceFilmmaker::Engine_LogFields();
			advancedfx::Message("mvm_debug: STARTED. file: %s (type 'mvm_debug stop' when done).\n", file.c_str());
		} else
			advancedfx::Warning("mvm_debug: already running (or file could not be opened).\n");
	} else if (0 == _stricmp(mode, "stop")) {
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
	} else if (0 == _stricmp(mode, "status") || mode[0] == '\0') {
		const Filmmaker::MvmDebugLogStats stats = Filmmaker::MvmDebugLog_GetStats();
		advancedfx::Message("mvm_debug: %s. events=%llu unique=%llu repeats_combined=%llu. "
			"Usage: mvm_debug start|stop|status\n",
			Filmmaker::MvmDebugLog_Active() ? "RUNNING" : "stopped",
			(unsigned long long)stats.eventsReceived,
			(unsigned long long)stats.uniqueEventsWritten,
			(unsigned long long)stats.repeatsCombined);
	} else {
		advancedfx::Warning("mvm_debug: unknown action '%s'. Usage: mvm_debug start|stop|status\n", mode);
	}
}
