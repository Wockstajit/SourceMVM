// CS2's answers to the end-of-demo hold's host (DemoEndHoldHost.h).

#include "DemoEndHoldHost.h"

#include "../Filmmaker.h"               // PlayingDemoPath()
#include "DemoScrub.h"                  // DemoScrub_Active
#include "../Demo/DemoHeaderReader.h"   // playback_ticks from the .dem header
#include "../../MirvTime.h"
#include "../../ClientEntitySystem.h"   // cs_gamerules lookup
#include "../../SchemaSystem.h"         // C_CSGameRules::m_gamePhase offset

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Current CCSGameRules::m_gamePhase, or -1 when it cannot be read (no level, schema field
// unresolved after a CS2 update, or a demo with no game rules entity). -1 always means "do not
// act on this" -- the tick-based guard is what covers those cases.
int ReadGamePhase() {
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return -1;
	if (!g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules) return -1;
	if (!g_clientDllOffsets.C_CSGameRules.m_gamePhase) return -1;
	// Deliberately NOT cached across frames. The proxy entity is recreated on every level load
	// and the editor lives through demo changes; a stale pointer here would be read as a game
	// phase. The scan is a pointer compare per entity over a few hundred slots, once a frame,
	// and it stops at the first hit -- cs_gamerules sits at a low index.
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		auto* ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!ent) continue;
		const char* className = ent->GetClassName();
		if (!className || 0 != strcmp("cs_gamerules", className)) continue;
		auto* rules = *(unsigned char**)((unsigned char*)ent + g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules);
		if (!rules) return -1;
		return *(int*)(rules + g_clientDllOffsets.C_CSGameRules.m_gamePhase);
	}
	return -1;
}

void DemoCmd(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

} // namespace

bool EndHoldHost_DemoState(bool& playing, bool& paused) {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	playing = demo && demo->IsPlayingDemo();
	paused = demo && demo->IsDemoPaused();
	return true;
}

std::wstring EndHoldHost_DemoPath() { return PlayingDemoPath(); }

int EndHoldHost_PlaybackTicks(const std::wstring& path) {
	const DemoHeaderInfo hi = ReadDemoHeader(path);
	return hi.ok ? hi.playbackTicks : -1;
}

bool EndHoldHost_Tick(int& tick) { return g_MirvTime.GetCurrentDemoTick(tick); }

int EndHoldHost_StartTick() {
	auto* demo = g_pEngineToClient ? g_pEngineToClient->GetDemoFile() : nullptr;
	return demo ? demo->GetDemoStartTick() : 0;
}

int EndHoldHost_GamePhase() { return ReadGamePhase(); }

bool EndHoldHost_ScrubActive() { return DemoScrub_Active(); }

void EndHoldHost_Pause() { DemoCmd("demo_pause"); }

} // namespace Filmmaker
