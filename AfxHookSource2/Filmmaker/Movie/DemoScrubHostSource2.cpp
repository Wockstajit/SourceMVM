// CS2's answers to the live demo scrub (DemoScrub.cpp, the rules shared with CS:S): the demo clock
// from the client's curtime, the commands through the engine's client command, the user's speed from
// DemoTimescale, keyframed copies for fast restores, and the viewport scaler's preview hold.

#include "DemoScrubHost.h"

#include "DemoScrubMath.h"  // kScrubTickRate
#include "DemoTimescale.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Demo/DemoProjectStoreMath.h" // IsKeyframedCopyFileName: either slot of a keyframed copy
#include "../AltEnvironment/AltEnvironment.h" // IsKeyframedCopy
#include "../Render/RenderCommand.h" // Render_IsJobActive: a render job owns the demo clock
#include "../../ViewportScaler.h"     // AfxViewportScaler::SetHoldPreview: no raw restore on screen

#include "../../MirvTime.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cmath>
#include <cstdio>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

void Cmd(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

// Offset between the client clock (curtime * 64) and the demo tick, learned whenever the demo is
// paused on a whole tick (it is the demo's server start tick; 3173 in ragdolls.dem).
bool g_haveClockOffset = false;
double g_clockOffset = 0.0;

} // namespace

// The world's demo time in ticks, FRACTIONAL. A paused demo can rest between two ticks
// (DemoScrubMath.h) and a slow drag has to see that. The client clock is the source: measured
// 2026-09-27, curtime * 64 reads exactly tick + offset when paused on a whole tick, holds the
// fraction when paused between ticks, and advances continuously while playing. (MirvTime's
// interpolation_amount reads 0 in CS2 -- a CS:GO-era offset -- so it cannot be used.)
bool ScrubHost_DemoState(int& tick, double& now, bool& paused) {
	tick = 0; now = 0.0; paused = true;
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	if (!demo || !(demo->IsPlayingDemo() || demo->IsDemoPaused())) return false;
	paused = demo->IsDemoPaused();
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return false;
	const double rel = (double)g_MirvTime.curtime_get() * kScrubTickRate - tick;
	if (paused && std::fabs(rel - std::floor(rel + 0.5)) < 0.002) {
		g_clockOffset = std::floor(rel + 0.5);
		g_haveClockOffset = true;
	}
	// Before an offset is learned, tolerate the clock's small backward jitter at a tick boundary.
	const double offset = g_haveClockOffset ? g_clockOffset : std::floor(rel + 0.02);
	double frac = rel - offset;
	if (!(frac >= 0.0)) frac = 0.0;
	if (frac > 0.999) frac = 0.999;
	now = tick + frac;
	return true;
}

void ScrubHost_BeginScrub() {
	g_haveClockOffset = false; // per demo: relearned on the first paused whole tick
}

bool ScrubHost_FastRestores() {
	const std::wstring file = ResolvePlayingDemoPath();
	const size_t slash = file.find_last_of(L"\\/");
	return IsKeyframedCopyFileName(slash == std::wstring::npos ? file : file.substr(slash + 1))
		|| AltEnvironment::IsKeyframedCopy(file); // the other-map copy of a keyframed copy keeps its keyframes
}

int ScrubHost_SeekLandsPast() { return 1; }

bool ScrubHost_RenderActive() { return Render_IsJobActive(); }

void ScrubHost_RestoreUserTimescale() { DemoTimescale_Set(DemoTimescale_Get()); }

double ScrubHost_UserTimescale() { return DemoTimescale_Get(); }

void ScrubHost_Pause() { Cmd("demo_pause"); }

void ScrubHost_Resume() { Cmd("demo_resume"); }

void ScrubHost_Play(double timescale, bool resume) {
	char c[96];
	if (resume) _snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.4f;demo_resume", timescale);
	else _snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.4f", timescale);
	Cmd(c);
}

void ScrubHost_Step(int ticks, double timescale, int) {
	char c[96];
	_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.3f;demo_step_tick %d", timescale, ticks);
	Cmd(c);
}

void ScrubHost_Seek(int tick) {
	char c[64];
	_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_gototick %d", tick);
	Cmd(c);
}

double ScrubHost_FrameSeconds() { return (double)g_MirvTime.absoluteframetime_get(); }

void ScrubHost_HoldPreview(bool hold) { AfxViewportScaler::SetHoldPreview(hold); }

std::string ScrubHost_ClockText() {
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	char c[160];
	_snprintf_s(c, sizeof(c), _TRUNCATE, "tick=%d interp=%.4f curtime=%.5f curtime*64=%.4f", tick,
		g_MirvTime.interpolation_amount_get(), g_MirvTime.curtime_get(), g_MirvTime.curtime_get() * kScrubTickRate);
	return c;
}

} // namespace Filmmaker
