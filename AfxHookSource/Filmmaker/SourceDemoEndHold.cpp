// CS:S's answers to the end-of-demo hold (AfxHookSource2/Filmmaker/Movie/DemoEndHold.cpp, the rules
// shared with CS2): the demo's state and length from the engine's demo player (SourceEngine.h
// Demo_State, whose tick counts from the demo's start, as the demo bar reads it), its file from the
// demo index. CS:S has no game phase to read and no end-of-match line-up, so the hold stops a second
// before the last tick, before the engine reaches the demo's stop record and drops to the menu.

#include "stdafx.h"

#include "SourceDemoIndex.h"
#include "SourceEngine.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Movie/DemoEndHoldHost.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>

#include <algorithm>
#include <string>

extern WrpVEngineClient* g_VEngineClient;

namespace Filmmaker {

bool EndHoldHost_DemoState(bool& playing, bool& paused) {
	if (!g_VEngineClient) return false;
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	playing = demo.playing && !demo.paused; // the engine reads a paused demo as playing
	paused = demo.playing && demo.paused;
	return true;
}

std::wstring EndHoldHost_DemoPath() { return Utf8ToWide(SourceFilmmaker::DemoIndex_Get().file); }

int EndHoldHost_PlaybackTicks(const std::wstring&) {
	// The engine's count once the demo is open; the file's own header as well, whichever is known.
	const SourceFilmmaker::DemoIndex index = SourceFilmmaker::DemoIndex_Get();
	const int fromFile = index.scan ? index.scan->header.playbackTicks : 0;
	const int ticks = (std::max)(SourceFilmmaker::Demo_State().totalTicks, fromFile);
	return ticks > 0 ? ticks : -1;
}

bool EndHoldHost_Tick(int& tick) {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	if (!demo.playing) return false;
	tick = demo.tick;
	return true;
}

int EndHoldHost_StartTick() { return 0; } // Demo_State's tick already counts from the start

int EndHoldHost_GamePhase() { return -1; }

bool EndHoldHost_ScrubActive() { return false; } // CS:S's seeks are single demo_gototick commands

void EndHoldHost_Pause() {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted("demo_pause");
}

} // namespace Filmmaker
