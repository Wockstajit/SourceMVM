#include "stdafx.h"

#include "SourceFilmmaker.h"
#include "SourceConsole.h"
#include "SourceNetcon.h"

#include "addresses.h"
#include "WrpConsole.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/NetconServer.h>

#include <atomic>
#include <cstring>

extern WrpVEngineClient* g_VEngineClient;
extern char const* g_Info_VClient;
extern char const* g_Info_VEngineClient;
extern char const* g_Info_VEngineCvar;

namespace SourceFilmmaker {

namespace {

std::atomic<unsigned long long> g_frameRenderStarts{ 0 };
std::atomic<unsigned long long> g_windowMessages{ 0 };

const char* GameName() {
	switch (g_SourceSdkVer) {
	case SourceSdkVer_CSS: return "Counter-Strike: Source";
	case SourceSdkVer_CSSV34: return "Counter-Strike: Source v34";
	case SourceSdkVer_CSSV84: return "Counter-Strike: Source v84";
	case SourceSdkVer_CSGO: return "Counter-Strike: Global Offensive";
	case SourceSdkVer_CSCO: return "Counter-Strike: Classic Offensive";
	case SourceSdkVer_TF2: return "Team Fortress 2";
	case SourceSdkVer_TF2Classified: return "Team Fortress 2 Classified";
	case SourceSdkVer_Garrysmod: return "Garry's Mod";
	case SourceSdkVer_Insurgency2: return "Insurgency";
	case SourceSdkVer_L4D2: return "Left 4 Dead 2";
	case SourceSdkVer_SWARM: return "Alien Swarm";
	case SourceSdkVer_BM: return "Black Mesa";
	case SourceSdkVer_Momentum: return "Momentum Mod";
	case SourceSdkVer_HL2MP: return "Half-Life 2: Deathmatch";
	default: return "unknown";
	}
}

void PrintHealth() {
	advancedfx::Message("AfxHookSource health\n");
	advancedfx::Message("  build:      %s %s, %s\n", __DATE__, __TIME__,
#ifdef _WIN64
		"64-bit"
#else
		"32-bit"
#endif
	);
	advancedfx::Message("  game:       %s\n", GameName());
	advancedfx::Message("  interfaces: %s / %s / %s\n", g_Info_VClient, g_Info_VEngineClient, g_Info_VEngineCvar);

	const advancedfx::NetconServer::Stats netcon = advancedfx::NetconServer::GetStats();
	if (netcon.active) {
		advancedfx::Message("  netcon:     127.0.0.1:%u, %d client(s), %llu received, %llu executed, %llu lines sent, %llu dropped; console capture %s\n",
			(unsigned)netcon.port, netcon.clients, netcon.commandsReceived, netcon.commandsExecuted,
			netcon.linesSent, netcon.linesDropped, Console_Captured() ? "on" : "REPLACED");
	} else {
		advancedfx::Message("  netcon:     off (start the game with -mvmnetconport <port>)\n");
	}

	const Filmmaker::MvmDebugLogStats log = Filmmaker::MvmDebugLog_GetStats();
	advancedfx::Message("  mvm_debug:  %s, %llu events, console capture %s\n",
		Filmmaker::MvmDebugLog_Active() ? "running" : "stopped", (unsigned long long)log.eventsReceived,
		Filmmaker::MvmConsoleCapture_Active() ? "on" : "off");

	advancedfx::Message("  pumps:      %llu window messages, %llu netcon drains, %llu frame render starts\n",
		g_windowMessages.load(), Netcon_PumpCount(), g_frameRenderStarts.load());

	if (g_VEngineClient) {
		const bool playingDemo = g_VEngineClient->IsPlayingDemo();
		advancedfx::Message("  state:      connected=%d in-game=%d demo=%s map=%s\n",
			g_VEngineClient->IsConnected() ? 1 : 0, g_VEngineClient->IsInGame() ? 1 : 0,
			playingDemo ? "playing" : "no", g_VEngineClient->IsInGame() ? g_VEngineClient->GetLevelName() : "-");
		if (playingDemo) {
			if (WrpVEngineClientDemoInfoEx* demo = g_VEngineClient->GetDemoInfoEx()) {
				advancedfx::Message("  demo:       tick %d of %d, timescale %.2f, paused=%d\n",
					demo->GetDemoPlaybackTick(), demo->GetDemoPlaybackTotalTicks(),
					demo->GetDemoPlaybackTimeScale(), g_VEngineClient->IsPaused() ? 1 : 0);
			}
		}
	} else {
		advancedfx::Message("  state:      no engine client interface\n");
	}
}

void PrintUsage() {
	advancedfx::Message(
		"mirv_filmmaker health - hook build, game, interfaces, command port and pump counts.\n");
}

} // namespace

void Init() {
	Netcon_Init();
}

bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	(void)wParam;
	(void)lParam;
	g_windowMessages.fetch_add(1);
	return Netcon_OnWindowMessage(hwnd, msg);
}

void OnFrameRenderStart() {
	g_frameRenderStarts.fetch_add(1);
}

} // namespace SourceFilmmaker

CON_COMMAND(mirv_filmmaker, "SOURCE:MVM filmmaker. mirv_filmmaker health for a status report.")
{
	const int argc = args->ArgC();
	if (argc >= 2 && 0 == _stricmp(args->ArgV(1), "health")) {
		SourceFilmmaker::PrintHealth();
		return;
	}
	SourceFilmmaker::PrintUsage();
}
