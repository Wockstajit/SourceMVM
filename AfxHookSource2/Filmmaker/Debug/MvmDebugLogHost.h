#pragma once

// What MvmDebugLog.cpp needs from the hook it is linked into. The logger itself is engine-free and
// is compiled into the CS2, Deadlock and Source 1 hooks; each hook defines these once:
// MvmDebugLogHostSource2.cpp for CS2 and Deadlock, AfxHookSource/Filmmaker/SourceDebugLog.cpp for
// Source 1.

#include <string>

namespace Filmmaker {

// %APPDATA%\HLAE (the logger adds "debuglogs").
std::wstring MvmDebugLogHost_RoamingFolder();

// The demo tick to stamp on each line. False when no demo is playing.
bool MvmDebugLogHost_DemoTick(int& outTick);

// The engine frame to stamp on each line.
int MvmDebugLogHost_FrameCount();

} // namespace Filmmaker
