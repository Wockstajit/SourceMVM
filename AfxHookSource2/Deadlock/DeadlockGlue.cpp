// Definitions the reused CS2-tree files link against, backed by Deadlock's own state.
//
// This is what makes reuse possible instead of duplication. The shared files
// (ConsoleCapture.cpp, WrpConsole.cpp, MouseCapture.cpp, MvmDebugLog.cpp, Globals.cpp) are
// engine-neutral in substance, but they reference a handful of globals and free functions that
// CS2 happens to define inside main.cpp -- a translation unit far too CS2-specific to compile
// here. Defining that same handful with Deadlock values lets every one of those files compile and
// link unchanged, which is a much better trade than forking five files to change one line in each.
//
// Nothing here does real work. If a function in this file grows a body worth reading, it belongs
// in its own module instead.

#include "DeadlockEngine.h"

#include "../MirvTime.h"
#include "../hlaeFolder.h"
#include "../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../Deps/release/prop/cs2/sdk_src/public/tier0/memalloc.h"

#include <windows.h>
#include <shlobj.h>
#include <string>

#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// Source 2 SDK globals. Populated in DeadlockMain.cpp from the app-system factory; the interface
// version strings are byte-identical to CS2's, so these are the same interfaces.
// ---------------------------------------------------------------------------

SOURCESDK::CS2::IMemAlloc* SOURCESDK::CS2::g_pMemAlloc = nullptr;
SOURCESDK::CS2::ICvar* SOURCESDK::CS2::cvar = nullptr;
SOURCESDK::CS2::ICvar* SOURCESDK::CS2::g_pCVar = nullptr;

// ---------------------------------------------------------------------------
// Time. MvmDebugLog stamps every line with the frame count and demo tick, so the log can be
// read as a timeline rather than a pile of lines.
//
// CS2's implementation reads these out of the client's CGlobalVars through raw offsets. Those
// offsets are unverified for Deadlock, and a wrong one here would corrupt every log stamp while
// looking plausible, so the frame count is ours and the tick comes through the engine facade.
// ---------------------------------------------------------------------------

CMirvTime g_MirvTime;

namespace {
int g_frameCount = 0;
}

int CMirvTime::framecount_get(void) {
	// Not the engine's frame counter, and does not claim to be: it is a monotonic count of log
	// stamps, which is all the log actually needs from it. Reading the engine's would mean
	// trusting an unverified CGlobalVars offset for a diagnostic value.
	return ++g_frameCount;
}

bool CMirvTime::GetCurrentDemoTick(int& outTick) {
	const int tick = Deadlock::Engine_DemoTick();
	if (tick < 0) {
		outTick = 0;
		return false;
	}
	outTick = tick;
	return true;
}

bool CMirvTime::GetCurrentDemoTime(double& outDemoTime) {
	outDemoTime = Deadlock::Engine_DemoTime();
	return outDemoTime != 0.0;
}

// ---------------------------------------------------------------------------
// Folders. The debug log writes under %APPDATA%\HLAE\debuglogs, the same place the CS2 build uses,
// so one log reader works for both games.
// ---------------------------------------------------------------------------

const wchar_t* GetHlaeRoamingAppDataFolderW() {
	static std::wstring folder;
	if (!folder.empty()) return folder.c_str();

	PWSTR appData = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
		folder.assign(appData);
		CoTaskMemFree(appData);
	} else {
		// Better a relative path than an empty one: the log still lands somewhere findable.
		folder = L".";
	}
	folder += L"\\HLAE";
	CreateDirectoryW(folder.c_str(), nullptr);
	return folder.c_str();
}

// ---------------------------------------------------------------------------
// MouseCapture asks whether the live FX menu wants the game's mouse released. That menu is CS2
// weapon-effect tooling and is not part of this port, so the answer is always no.
// ---------------------------------------------------------------------------

namespace Filmmaker {
bool LiveFxMenu_WantsGameMouseReleased() { return false; }
}
