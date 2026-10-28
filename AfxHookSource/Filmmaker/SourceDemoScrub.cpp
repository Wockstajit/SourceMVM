// CS:S's answers to the live demo scrub (AfxHookSource2/Filmmaker/Movie/DemoScrub.cpp, the rules shared
// with CS2): the demo state from the engine's demo player, the fractional clock from the client's
// curtime, and the demo commands through the console. CS:S has no demo_step_tick: a step plays at the
// step's speed with demo_pauseatservertick on its last tick, which counts server ticks (the demo tick
// plus an offset fixed per demo, learned from the clock; measured 2026-10-27: 5912 paused the SourceTV
// test demo on tick 760). A seek, back too (rewound in place, SourceDemoRewind.h), lands in its frame
// on the demo's next packet, up to DemoRewind_SeekLandsPast ticks after its tick.

#include "stdafx.h"

#include "SourceDemoRewind.h"
#include "SourceEngine.h"

#include "MirvTime.h"
#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Movie/DemoScrubHost.h>

#include <cmath>
#include <cstdio>
#include <string>

extern WrpVEngineClient* g_VEngineClient;

namespace Filmmaker {

namespace {

void Cmd(const char* command) {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command);
}

// Offset between the client clock (curtime / interval) and the demo tick, learned whenever the demo is
// paused on a whole tick, as CS2's host does.
bool g_haveClockOffset = false;
double g_clockOffset = 0.0;
double g_userTimescale = 1.0; // the demo speed when the scrub began

} // namespace

bool ScrubHost_DemoState(int& tick, double& now, bool& paused) {
	tick = 0; now = 0.0; paused = true;
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	if (!demo.playing || !globals || !(demo.intervalPerTick > 0.0)) return false;
	tick = demo.tick;
	paused = demo.paused;
	const double rel = (double)globals->curtime_get() / demo.intervalPerTick - tick;
	if (paused && std::fabs(rel - std::floor(rel + 0.5)) < 0.002) {
		g_clockOffset = std::floor(rel + 0.5);
		g_haveClockOffset = true;
	}
	const double offset = g_haveClockOffset ? g_clockOffset : std::floor(rel + 0.02);
	double frac = rel - offset;
	if (!(frac >= 0.0)) frac = 0.0;
	if (frac > 0.999) frac = 0.999;
	now = tick + frac;
	return true;
}

void ScrubHost_BeginScrub() {
	g_haveClockOffset = false;
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	g_userTimescale = demo.timescale > 0.0 ? demo.timescale : 1.0;
}

// Rewound in place, a seek back lands in the frame it was asked in (SourceDemoRewind.h).
bool ScrubHost_FastRestores() { return SourceFilmmaker::DemoRewind_Installed(); }

int ScrubHost_SeekLandsPast() { return SourceFilmmaker::DemoRewind_SeekLandsPast(); }

bool ScrubHost_RenderActive() { return false; } // no render job yet (port step 14)

void ScrubHost_RestoreUserTimescale() {
	char command[48];
	std::snprintf(command, sizeof(command), "demo_timescale %.4f", g_userTimescale);
	Cmd(command);
}

double ScrubHost_UserTimescale() { return g_userTimescale; }

void ScrubHost_Pause() { Cmd("demo_pause"); }

void ScrubHost_Resume() { Cmd("demo_resume"); }

void ScrubHost_Play(double timescale, bool resume) {
	char command[64];
	std::snprintf(command, sizeof(command), resume ? "demo_timescale %.4f; demo_resume" : "demo_timescale %.4f", timescale);
	Cmd(command);
}

void ScrubHost_Step(int ticks, double timescale, int fromTick) {
	// The server tick is the demo tick plus the clock's offset (the demo player's start tick is not it:
	// it read 382 and then 210 while the offset was 5152).
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	double offset = g_clockOffset;
	if (!g_haveClockOffset && globals && demo.intervalPerTick > 0.0)
		offset = std::floor((double)globals->curtime_get() / demo.intervalPerTick - demo.tick + 0.002);
	char command[96];
	std::snprintf(command, sizeof(command), "demo_timescale %.3f; demo_pauseatservertick %d; demo_resume", timescale,
		fromTick + ticks + (int)offset);
	Cmd(command);
}

void ScrubHost_Seek(int tick) {
	// Both ways the engine's own seek, landing paused; back, it rewinds in place (SourceDemoRewind.h).
	char command[64];
	std::snprintf(command, sizeof(command), "demo_gototick %d 0 1", tick < 0 ? 0 : tick);
	Cmd(command);
}

double ScrubHost_FrameSeconds() { return (double)g_MirvTime.GetAbsoluteFrameTime(); }

std::string ScrubHost_ClockText() {
	const SourceFilmmaker::DemoState demo = SourceFilmmaker::Demo_State();
	WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals();
	char text[200];
	std::snprintf(text, sizeof(text), "tick=%d start=%d interval=%.5f curtime=%.5f interp=%.4f offset=%s%.1f", demo.tick, demo.startTick,
		demo.intervalPerTick, globals ? globals->curtime_get() : 0.0f, globals ? globals->interpolation_amount_get() : 0.0f,
		g_haveClockOffset ? "" : "~", g_clockOffset);
	return text;
}

} // namespace Filmmaker
