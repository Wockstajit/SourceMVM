#include "MvmDebugLogHost.h"

#include "../../hlaeFolder.h" // GetHlaeRoamingAppDataFolderW
#include "../../MirvTime.h"

// The CS2 and Deadlock hooks' answers to MvmDebugLogHost.h.

namespace Filmmaker {

std::wstring MvmDebugLogHost_RoamingFolder() {
	return GetHlaeRoamingAppDataFolderW();
}

bool MvmDebugLogHost_DemoTick(int& outTick) {
	return g_MirvTime.GetCurrentDemoTick(outTick);
}

int MvmDebugLogHost_FrameCount() {
	return g_MirvTime.framecount_get();
}

} // namespace Filmmaker
