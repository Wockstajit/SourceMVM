#include "DemoEndHold.h"

#include "../Filmmaker.h"               // PlayingDemoPath()
#include "../Demo/DemoHeaderReader.h"   // playback_ticks from the .dem header
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../MirvTime.h"
#include "../../ClientEntitySystem.h"   // cs_gamerules lookup
#include "../../SchemaSystem.h"         // C_CSGameRules::m_gamePhase offset

#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// How many ticks BEFORE the header's playback_ticks we freeze. The check runs once per rendered
// frame, so at high demo timescale the tick can advance a chunk between checks (64 tps * 16x /
// 60 fps ~= 17 ticks/frame); one second of headroom means we still catch it before the engine
// processes the DEM_Stop record and disconnects to the main menu.
constexpr int kEndMarginTicks = 64;
// Demos shorter than this (a couple of seconds) aren't worth guarding -- and a tiny/garbage
// playback_ticks would put the hold point at/near tick 0 and freeze playback instantly.
constexpr int kMinTotalTicks = 4 * kEndMarginTicks;

bool s_enabled = true;
std::wstring s_path;        // demo the cached tick count belongs to
int s_totalTicks = -1;      // header playback_ticks; -1 = unusable for this demo -> feature inert
bool s_holding = false;     // paused at the end-hold; next resume = restart from the beginning
bool s_restarting = false;  // seek back to the start was issued; suppress re-pausing until it lands
unsigned long long s_restartDeadlineMs = 0;

// ---- postgame phase ---------------------------------------------------------------------
// CCSGameRules::m_gamePhase. The values are NOT guessed: the probe below logs every transition
// to mvm_debug (`effects`-style, one line per change, never per frame) and this default was read
// off a finished Premier demo -- 3 through the second half, then 5 the instant the win panel
// appeared and the podium sequence started. It agrees with the note DeathMsg.cpp already carried
// (2 = first half, 3 = second, 4 = halftime, 5 = postgame).
//
// Treated as ">= postgame" rather than "== postgame" so a future phase added above it still
// counts as over, and overridable at runtime (`endhold phase <n>`) so a CS2 update that
// renumbers the enum can be re-fitted from the console instead of from a rebuild.
constexpr int kDefaultPostGamePhase = 5;
int s_postGamePhase = kDefaultPostGamePhase;
bool s_postGameEnabled = true;
int s_lastPhase = -1;       // last value seen, so only CHANGES are logged
int s_postGameTick = -1;    // demo tick the postgame phase was first seen at; -1 = not seen

void Reset() {
	s_path.clear();
	s_totalTicks = -1;
	s_holding = false;
	s_restarting = false;
	s_lastPhase = -1;
	s_postGameTick = -1;
}

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

void DemoEndHold_SetEnabled(bool enabled) {
	s_enabled = enabled;
	if (!enabled) Reset();
}

bool DemoEndHold_Enabled() { return s_enabled; }

void DemoEndHold_RunFrame() {
	if (!s_enabled || !g_pEngineToClient) { Reset(); return; }

	auto* demo = g_pEngineToClient->GetDemoFile();
	const bool playing = demo && demo->IsPlayingDemo();
	const bool paused = demo && demo->IsDemoPaused(); // a paused demo reports IsPlayingDemo() false
	if (!playing && !paused) {
		// IsPlayingDemo() also blips false transiently DURING a demo_gototick seek -- exactly
		// the state our restart puts the engine in. Dropping state here would re-read the
		// still-past-the-end tick next frame and instantly re-pause, killing the restart.
		if (s_restarting && GetTickCount64() <= s_restartDeadlineMs)
			return;
		Reset();
		return;
	}

	// Re-resolve the demo's tick count whenever the playing demo changes. Header read is a
	// small bounded file read, done once per demo.
	std::wstring path = PlayingDemoPath();
	if (!path.empty() && path != s_path) {
		s_path = path;
		s_holding = false;
		s_restarting = false;
		DemoHeaderInfo hi = ReadDemoHeader(path);
		s_totalTicks = (hi.ok && hi.playbackTicks >= kMinTotalTicks) ? hi.playbackTicks : -1;
	}
	if (s_totalTicks <= 0)
		return;

	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick))
		return;

	int startTick = demo ? demo->GetDemoStartTick() : 0;
	if (startTick < 0) startTick = 0;
	int holdTick = startTick + s_totalTicks - kEndMarginTicks;

	// POSTGAME. Read every frame (the phase is what moves, not the tick), logged only on change.
	const int phase = ReadGamePhase();
	if (phase != s_lastPhase) {
		MvmDebugLog_LinefAlways("endhold", "gamePhase %d -> %d at tick %d (postgame >= %d, %s)",
			s_lastPhase, phase, tick, s_postGamePhase, s_postGameEnabled ? "armed" : "disabled");
		s_lastPhase = phase;
	}
	if (s_postGameEnabled && phase >= s_postGamePhase && phase > 0) {
		// Latch the FIRST tick the postgame phase was seen at and hold there. Latched, because
		// once we pause, the phase stays postgame for as long as we sit on that tick -- deriving
		// the hold point from "wherever we are now" would let a single frame of overshoot walk
		// the hold point forward every time the user nudged playback.
		//
		// Scrubbing BACK before the latch is still free: the tick < holdTick branch below clears
		// s_holding, and the latch is only re-armed by arriving at the postgame phase again.
		if (s_postGameTick < 0) {
			s_postGameTick = tick;
			MvmDebugLog_LinefAlways("endhold", "postgame starts at tick %d (last tick would be %d)",
				s_postGameTick, holdTick);
		}
		if (s_postGameTick < holdTick) holdTick = s_postGameTick;
	} else if (phase >= 0 && phase < s_postGamePhase) {
		// Back in a playing phase (scrubbed away from the end, or a new demo). Drop the latch so
		// the next arrival re-measures instead of holding at a tick from the previous match.
		s_postGameTick = -1;
	}

	if (s_restarting) {
		// The seek back to the start replays packets over several frames, during which the
		// current tick still reads at/past the hold point. Don't re-arm the pause until the
		// tick is clearly below it (or the seek evidently never landed).
		if (tick < holdTick || GetTickCount64() > s_restartDeadlineMs)
			s_restarting = false;
		else
			return;
	}

	// Releasing the latch is HYSTERETIC. demo_pause takes effect a frame or so after we ask, so we
	// routinely settle a tick or two SHORT of the hold point -- and a plain `tick < holdTick`
	// release then cleared s_holding there, which made pressing play from the hold nudge one tick
	// and re-pause instead of doing what the message promised (restart from the beginning).
	// A second of slack absorbs that; scrubbing back for real still releases.
	constexpr int kHoldReleaseSlackTicks = 64;
	if (tick < holdTick - kHoldReleaseSlackTicks) { s_holding = false; return; }
	if (tick < holdTick) return;   // inside the slack: keep the latch, do not pause again

	// At/past the hold point.
	if (paused) {
		// Any pause at the end counts as the hold (ours or the user's own), so a single
		// play press from here restarts from the beginning.
		s_holding = true;
		return;
	}
	if (s_holding) {
		// We were holding at the end and playback is running again = the user pressed play.
		// Restart from the beginning instead of letting the engine run into the stop record.
		s_holding = false;
		s_restarting = true;
		s_restartDeadlineMs = GetTickCount64() + 15000;
		char cmd[64];
		sprintf_s(cmd, "demo_gototick %d", startTick);
		DemoCmd(cmd);
		advancedfx::Message("mirv_filmmaker: end of demo -- restarting playback from the beginning.\n");
	} else {
		// First arrival at the hold point: freeze. At the postgame hold that keeps the podium
		// sequence off the screen entirely; at the last-tick hold it beats the engine to the
		// demo's stop record (which would disconnect back to the main menu).
		DemoCmd("demo_pause");
		s_holding = true;
		const bool postgame = (s_postGameTick >= 0 && s_postGameTick <= holdTick);
		advancedfx::Message(postgame
			? "mirv_filmmaker: match over at tick %d -- holding here, before CS2's end-of-match "
			  "screens (play again restarts from the beginning; `endhold postgame off` to run into them).\n"
			: "mirv_filmmaker: end of demo (tick %d) -- holding on the last tick "
			  "(play again restarts from the beginning).\n",
			holdTick);
	}
}

void DemoEndHold_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = (argc >= 3) ? args->ArgV(2) : "toggle";

	if (0 == _stricmp(op, "status")) {
		const int phase = ReadGamePhase();
		advancedfx::Message(
			"end-of-demo hold: %s\n"
			"  postgame hold: %s (phase >= %d)   live gamePhase: %d%s\n"
			"  postgame first seen at tick: %d\n"
			"  demo playback_ticks: %d   holding: %s\n",
			s_enabled ? "on" : "off",
			s_postGameEnabled ? "on" : "off", s_postGamePhase, phase,
			phase < 0 ? "  (unreadable -- last-tick guard only)" : "",
			s_postGameTick, s_totalTicks, s_holding ? "yes" : "no");
		return;
	}
	if (0 == _stricmp(op, "postgame")) {
		const char* v = (argc >= 4) ? args->ArgV(3) : "toggle";
		s_postGameEnabled = (0 == _stricmp(v, "toggle")) ? !s_postGameEnabled
			: (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1"));
		if (!s_postGameEnabled) s_postGameTick = -1;
		advancedfx::Message("mirv_filmmaker: postgame hold %s.\n", s_postGameEnabled ? "on" : "off");
		return;
	}
	if (0 == _stricmp(op, "phase")) {
		if (argc >= 4) {
			const int v = atoi(args->ArgV(3));
			if (v >= 1 && v <= 32) { s_postGamePhase = v; s_postGameTick = -1; }
			else { advancedfx::Warning("mirv_filmmaker: endhold phase must be 1..32.\n"); return; }
		}
		advancedfx::Message("mirv_filmmaker: postgame phase threshold = %d (default %d).\n",
			s_postGamePhase, kDefaultPostGamePhase);
		return;
	}
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "1")) DemoEndHold_SetEnabled(true);
	else if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "0")) DemoEndHold_SetEnabled(false);
	else DemoEndHold_SetEnabled(!DemoEndHold_Enabled());
	advancedfx::Message("mirv_filmmaker: end-of-demo hold %s.\n", DemoEndHold_Enabled() ? "on" : "off");
}

} // namespace Filmmaker
