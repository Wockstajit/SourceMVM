#include "SmoothViewTest.h"
#include "SmoothView.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <cstdlib>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Same test as DemoAimResampler's -- duplicated rather than shared because it is three lines and
// exporting it would couple this file into DemoAimResampler's header for no reason.
bool IsDemoPlayback() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
	return false;
}

} // namespace

void SmoothViewTest_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* a = argc >= 3 ? args->ArgV(2) : "";

	if (0 == _stricmp(a, "start")) {

		const bool alreadyActive = MvmDebugLog_Active();
		std::string file;
		if (!alreadyActive && !MvmDebugLog_Start(&file)) {
			advancedfx::Message("mirv_filmmaker smoothtest: could not start the mvm_debug log.\n");
			return;
		}

		int tick = 0;
		g_MirvTime.GetCurrentDemoTick(tick);
		const bool inDemo = IsDemoPlayback();

		// Record the remaining motion settings with the capture.
		MvmDebugLog_LinefAlways(
			"smoothtest.session",
			"leg=%s tick=%d smoothview=%s",
			inDemo ? "demo" : "live", tick,
			SmoothViewRef().DebugStateJson().c_str());

		advancedfx::Message(
			"mirv_filmmaker smoothtest: recording motion on the %s leg%s. Settings snapshot written "
			"to smoothtest.session. Perform the movement you want to inspect, then `smoothtest stop`.\n",
			inDemo ? "DEMO" : "LIVE",
			alreadyActive ? " (mvm_debug log already running)" : "");
		return;
	}

	if (0 == _stricmp(a, "stop")) {
		std::string folder, file;
		MvmDebugLogStats stats;
		if (MvmDebugLog_Stop(&folder, &file, &stats)) {
			advancedfx::Message(
				"mirv_filmmaker smoothtest: stopped. %llu events, %llu repeats combined.\n"
				"  file:   %s\n"
				"  folder: %s\n"
				"  compare: python Automation/verify/smoothview-compare.py leg=<path> [leg2=<path>]\n",
				(unsigned long long)stats.eventsReceived,
				(unsigned long long)stats.repeatsCombined,
				file.c_str(), folder.c_str());
		} else {
			advancedfx::Message("mirv_filmmaker smoothtest: no active log to stop.\n");
		}
		return;
	}

	advancedfx::Message("%s smoothtest start - log breathing, tilt and movement settings.\n"
		"%s smoothtest stop - stop the motion log.\n", cmd, cmd);
}

} // namespace Filmmaker
