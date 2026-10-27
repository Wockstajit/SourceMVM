#include "stdafx.h"

#include "SourceFilmmaker.h"
#include "SourceCameraPath.h"
#include "SourceCapture.h"
#include "SourceConsole.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceDirector.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceMainThread.h"
#include "SourceNetcon.h"
#include "SourceOldDemos.h"
#include "SourceSmoothView.h"
#include "RenderView.h"
#include "WrpGlobals.h"
#include "SourceUi.h"

#include "addresses.h"
#include "WrpConsole.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/NetconServer.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

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

	const EngineDiagnostics engine = Engine_Diagnostics();
	advancedfx::Message("  engine:     %s, profile \"%s\", %d client classes, %d/%d fields, %llu faults\n",
		engine.ready ? "ready" : "NOT READY", GameProfile_Current().name, engine.classCount,
		engine.fieldsResolved, engine.fieldsWanted, engine.faults);

	if (!g_VEngineClient) {
		advancedfx::Message("  state:      no engine client interface\n");
		return;
	}
	const std::string map = Engine_MapName();
	const DemoState demo = Demo_State();
	advancedfx::Message("  state:      connected=%d in-game=%d demo=%s map=%s\n",
		g_VEngineClient->IsConnected() ? 1 : 0, g_VEngineClient->IsInGame() ? 1 : 0,
		demo.playing ? "playing" : "no", map.empty() ? "-" : map.c_str());
	if (demo.playing) {
		const double seconds = demo.tick * demo.intervalPerTick;
		advancedfx::Message("  demo:       tick %d of %d (%d:%05.2f at %.1f ticks/s), timescale %.2f, paused=%d\n",
			demo.tick, demo.totalTicks, (int)(seconds / 60), seconds - 60 * (int)(seconds / 60),
			demo.intervalPerTick > 0 ? 1.0 / demo.intervalPerTick : 0.0, demo.timescale, demo.paused ? 1 : 0);
	}
	DemoIndex_PrintHealth();
	OldDemos_PrintHealth();
	if (!g_VEngineClient->IsInGame()) return;
	Director_PrintHealth();
	CameraPath_PrintHealth();
	Ui_PrintOverlayHealth();

	const ViewPose game = View_Game();
	const ViewPose rendered = View_Rendered();
	advancedfx::Message("  view:       game (%.1f %.1f %.1f) (%.1f %.1f %.1f) fov %.1f\n",
		game.origin[0], game.origin[1], game.origin[2], game.angles[0], game.angles[1], game.angles[2], game.fov);
	advancedfx::Message("              rendered (%.1f %.1f %.1f) (%.1f %.1f %.1f) fov %.1f\n",
		rendered.origin[0], rendered.origin[1], rendered.origin[2],
		rendered.angles[0], rendered.angles[1], rendered.angles[2], rendered.fov);
	double matrixH = 0, matrixV = 0;
	if (Engine_ViewMatrixFov(matrixH, matrixV))
		advancedfx::Message("              engine matrix fov %.2f x %.2f\n", matrixH, matrixV);

	std::vector<PlayerState> players;
	Players_Snapshot(players);
	int alive = 0;
	std::string observedName = "-";
	const int observed = Players_ObservedIndex();
	for (const PlayerState& player : players) {
		if (player.alive) ++alive;
		if (player.index == observed) observedName = player.name;
	}
	advancedfx::Message("  players:    %d (%d alive), local #%d, observing #%d %s (mode %d)\n",
		(int)players.size(), alive, Players_LocalIndex(), observed, observedName.c_str(), Players_ObserverMode());
}

void PrintPlayers() {
	std::vector<PlayerState> players;
	if (!Players_Snapshot(players)) {
		advancedfx::Message("players: none (not in game, or the engine adapter is not ready)\n");
		return;
	}
	const int observed = Players_ObservedIndex();
	advancedfx::Message("  #  name                 team        hp  alive dormant  origin (x y z)            eye (pitch yaw)  class\n");
	for (const PlayerState& player : players) {
		advancedfx::Message("%s%2d  %-20.20s %-10.10s %4d  %-5s %-7s (%7.1f %7.1f %7.1f)  (%5.1f %6.1f)  %s%s%s\n",
			player.index == observed ? ">" : " ", player.index, player.name.c_str(), player.teamName,
			player.health, player.alive ? "yes" : "no", player.dormant ? "yes" : "no",
			player.origin[0], player.origin[1], player.origin[2], player.eyeAngles[0], player.eyeAngles[1],
			player.className.c_str(), player.bot ? " bot" : "", player.hltv ? " hltv" : "");
	}
}

void PrintUsage() {
	advancedfx::Message(
		"mirv_filmmaker health  - hook build, game, interfaces, command port, engine, view, demo and players.\n"
		"mirv_filmmaker players - one line per player: team, health, origin, eye angles, class.\n"
		"mirv_filmmaker engine  - the networked fields the engine adapter resolved, with offsets.\n"
		"mirv_filmmaker editor  - on|off|toggle|state: the Camera Editor workspace (ESC closes it).\n"
		"mirv_filmmaker loop    - on|off|toggle|state: LOOP playback between the demo bar's In and Out.\n"
		"mirv_filmmaker seek    - <tick>: go to a demo tick as the demo bar does (back = restart, then forward).\n"
		"mirv_filmmaker hud     - on|off|toggle|state: the movie director's cards during a demo.\n"
		"mirv_filmmaker blackout - on|off|toggle|status: F8's blackout (no HUD, no bar, no cards).\n"
		"mirv_filmmaker director - state: the movie director's mouse, cards, and where each bar control is.\n"
		"mirv_filmmaker smoothview - on|off|toggle|state|reset|<setting> <value>: CS2's Smooth View.\n"
		"mirv_filmmaker marker  - place|delete|select|arm|play|pause|stop|timing|interp|speedmode|...: camera paths.\n"
		"mirv_filmmaker capture - <file.bmp>: the next frame as the game shows it, the filmmaker's UI included.\n");
}

} // namespace

void Init() {
	Netcon_Init();
	Engine_Init();
	DemoIndex_Init();
	OldDemos_Init();
}

bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	g_windowMessages.fetch_add(1);
	if (Netcon_OnWindowMessage(hwnd, msg)) return true;
	return Ui_OnWindowMessage(hwnd, msg, wParam, lParam);
}

bool SuspendMirvInput() { return Ui_SuspendMirvInput(); }

void OnViewOverride(float tx, float ty, float tz, float& rx, float& ry, float& rz, float& fov, bool cameraOverridden) {
	if (!cameraOverridden) { // else HLAE's free camera or a campath has the view
		WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
		Director_ApplyRoll(rz, globals ? globals->absoluteframetime_get() : 0.0);
		SmoothView_OnView(tx, ty, tz, rx, ry, rz, fov, cameraOverridden);
	}
	const double origin[3] = { tx, ty, tz }, angles[3] = { rx, ry, rz };
	Ui_ViewRendered(origin, angles, fov); // the view as rendered: the path's markers drawn over it
}

void OnFrameRenderStart() {
	g_frameRenderStarts.fetch_add(1);
	MainThread_Frame();
	DemoIndex_Frame();
	OldDemos_Frame();
	DemoBar_Frame();
	CameraPath_Frame();
	Ui_MainThreadFrame();
}

} // namespace SourceFilmmaker

CON_COMMAND(mirv_filmmaker, "SOURCE:MVM filmmaker. mirv_filmmaker health for a status report.")
{
	const int argc = args->ArgC();
	const char* sub = argc >= 2 ? args->ArgV(1) : "";
	if (0 == _stricmp(sub, "health")) {
		SourceFilmmaker::PrintHealth();
	} else if (0 == _stricmp(sub, "players")) {
		SourceFilmmaker::PrintPlayers();
	} else if (0 == _stricmp(sub, "engine")) {
		SourceFilmmaker::Engine_PrintFields();
	} else if (0 == _stricmp(sub, "editor")) {
		SourceFilmmaker::Ui_EditorCommand(argc, args);
	} else if (0 == _stricmp(sub, "loop")) {
		std::string action = argc >= 3 ? args->ArgV(2) : "state";
		for (int i = 3; i < argc; ++i) action += std::string(" ") + args->ArgV(i); // range <in> <out>
		SourceFilmmaker::DemoBar_LoopCommand(action.c_str());
	} else if (0 == _stricmp(sub, "seek") && argc >= 3) {
		SourceFilmmaker::DemoBar_SeekTo(atoi(args->ArgV(2)));
	} else if (0 == _stricmp(sub, "hud")) {
		SourceFilmmaker::Director_HudCommand(argc >= 3 ? args->ArgV(2) : "state");
	} else if (0 == _stricmp(sub, "blackout")) {
		SourceFilmmaker::Director_BlackoutCommand(argc >= 3 ? args->ArgV(2) : "status");
	} else if (0 == _stricmp(sub, "director")) {
		SourceFilmmaker::Ui_DirectorState();
	} else if (0 == _stricmp(sub, "smoothview")) {
		SourceFilmmaker::SmoothView_Command(argc, args);
	} else if (0 == _stricmp(sub, "marker")) {
		SourceFilmmaker::CameraPath_Command(argc, args);
	} else if (0 == _stricmp(sub, "capture")) {
		SourceFilmmaker::Capture_Request(argc >= 3 ? args->ArgV(2) : "");
	} else {
		SourceFilmmaker::PrintUsage();
	}
}
