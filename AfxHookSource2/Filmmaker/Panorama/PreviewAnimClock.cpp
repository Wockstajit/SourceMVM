// Experimental CLIENT animation-clock driver. Live evidence shows these global writes do not reach
// the paused preview's AG2 evaluation; keep this only as an explicit diagnostic. See the header.

#include "PreviewAnimClock.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (the mandatory runtime-diagnosis channel)
#include "../../MirvTime.h"                // g_MirvTime curtime/frametime get+set
#include "../../WrpConsole.h"              // CON_COMMAND (mvm_previewclock)
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message / Warning

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (paused)

#include <cstdint>
#include <cstring>
#include <cstdlib>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// Which FrameStageNotify stage writes the clock. LATE is the default and the one that can work: a
// write in the early stage is re-applied by the client afterwards (measured -- see the header).
enum Stage { kStageMain = 1, kStageLate = 2 };

bool     g_enabled = false;
int      g_stage = kStageLate;
float    g_rate = 1.0f;          // animation speed multiplier while paused
int64_t  g_lastCounterMain = 0;  // QPC of the previous advanced frame, per stage (0 == no baseline)
int64_t  g_lastCounterLate = 0;
uint64_t g_frames = 0;           // frames we advanced
double   g_advanced = 0.0;        // total seconds injected

// A single frame's dt is clamped hard: a stall (loading, alt-tab, a breakpoint) must never inject a
// multi-second jump into client time, which would make animgraphs snap or trip engine time deltas.
constexpr double kMaxFrameDt = 1.0 / 15.0;
constexpr double kMinFrameDt = 1.0 / 1000.0;

bool DemoPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo ? demo->IsDemoPaused() : false;
}

bool InDemo() {
	return g_pEngineToClient && g_pEngineToClient->GetDemoFile() != nullptr;
}

double WallSeconds(int64_t& lastCounter) {
	LARGE_INTEGER freq, now;
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
	if (!QueryPerformanceCounter(&now)) return 0.0;
	const int64_t prev = lastCounter;
	lastCounter = now.QuadPart;
	if (prev == 0) return 0.0; // first frame: establish the baseline only
	const double dt = (double)(now.QuadPart - prev) / (double)freq.QuadPart;
	if (dt < kMinFrameDt) return 0.0;
	return (dt > kMaxFrameDt) ? kMaxFrameDt : dt;
}

} // namespace

bool PreviewAnimClock_Enabled() { return g_enabled; }

void PreviewAnimClock_SetEnabled(bool enabled) {
	if (g_enabled == enabled) return;
	g_enabled = enabled;
	// Drop the stale baselines so re-enabling never injects the whole idle gap in one frame.
	g_lastCounterMain = g_lastCounterLate = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.clock", "%s rate=%.3f stage=%d", enabled ? "ENABLE" : "DISABLE",
			g_rate, g_stage);
}

namespace {

// The actual injection, shared by both stages. Each stage keeps its OWN QPC baseline so running
// both does not split one frame's dt in half.
void Advance(int64_t& lastCounter, const char* stageName) {
	const double dt = WallSeconds(lastCounter) * (double)g_rate;
	if (dt <= 0.0)
		return; // baseline frame, or a sub-millisecond frame not worth a write

	const float before = g_MirvTime.curtime_get();
	g_MirvTime.curtime_set(before + (float)dt);
	// An animgraph advances by frametime, so curtime alone is not enough -- a paused demo reports 0.
	g_MirvTime.frametime_set((float)dt);

	++g_frames;
	g_advanced += dt;

	// Throttled, never deduplicated. `before` is the diagnostic that matters: if it keeps reporting
	// the SAME value while injected keeps growing, the client is re-applying its own curtime after
	// this stage and the write is landing too early to be read by bone setup.
	if (MvmDebugLog_Active() && (g_frames % 32) == 0)
		MvmDebugLog_LinefAlways("preview.clock",
			"advance stage=%s frames=%llu dt=%.5f curtime %.3f -> %.3f injected=%.2fs rate=%.2f",
			stageName, (unsigned long long)g_frames, dt, before, g_MirvTime.curtime_get(),
			g_advanced, g_rate);
}

// Shared gate: enabled, in a demo, and actually paused. Resets both baselines when inactive so the
// next active frame starts from a fresh QPC reading.
bool ClockActive() {
	if (!g_enabled || !InDemo() || !DemoPaused()) {
		g_lastCounterMain = g_lastCounterLate = 0;
		return false;
	}
	return true;
}

} // namespace

void PreviewAnimClock_RunFrame() {
	if (!ClockActive() || !(g_stage & kStageMain))
		return;
	Advance(g_lastCounterMain, "main");
}

void PreviewAnimClock_RunLateFrame() {
	if (!ClockActive() || !(g_stage & kStageLate))
		return;
	Advance(g_lastCounterLate, "late");
}

void PreviewAnimClock_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 2) ? args->ArgV(1) : "status";

	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) {
		PreviewAnimClock_SetEnabled(true);
		advancedfx::Message("%s: ON (diagnostic only) -- writing the client animation clock while"
			" paused; this is known not to advance the preview AG2 pose. rate=%.2f\n", cmd, g_rate);
		if (!InDemo())
			advancedfx::Warning("%s: not in a demo; this only does anything during demo playback.\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) {
		PreviewAnimClock_SetEnabled(false);
		advancedfx::Message("%s: off.\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "rate")) {
		if (argc < 3) { advancedfx::Warning("usage: %s rate <multiplier>\n", cmd); return; }
		const float r = (float)atof(args->ArgV(2));
		if (!(r > 0.0f) || r > 10.0f) { advancedfx::Warning("%s: rate must be in (0,10].\n", cmd); return; }
		g_rate = r;
		advancedfx::Message("%s: rate=%.2f\n", cmd, g_rate);
		return;
	}
	if (0 == _stricmp(arg, "stage")) {
		if (argc < 3) { advancedfx::Warning("usage: %s stage main|late|both\n", cmd); return; }
		const char* s = args->ArgV(2);
		if (0 == _stricmp(s, "main")) g_stage = kStageMain;
		else if (0 == _stricmp(s, "late")) g_stage = kStageLate;
		else if (0 == _stricmp(s, "both")) g_stage = kStageMain | kStageLate;
		else { advancedfx::Warning("%s: stage must be main|late|both\n", cmd); return; }
		g_lastCounterMain = g_lastCounterLate = 0;
		advancedfx::Message("%s: stage=%s\n", cmd, s);
		return;
	}
	if (0 == _stricmp(arg, "status")) {
		advancedfx::Message("%s: %s stage=%d rate=%.2f inDemo=%d paused=%d frames=%llu injected=%.2fs"
			" curtime=%.3f frametime=%.5f\n",
			cmd, g_enabled ? "ON" : "off", g_stage, g_rate, InDemo() ? 1 : 0, DemoPaused() ? 1 : 0,
			(unsigned long long)g_frames, g_advanced,
			g_MirvTime.curtime_get(), g_MirvTime.frametime_get());
		return;
	}

	advancedfx::Message(
		"%s on|off  - diagnostic client-clock writes while PAUSED (known not to animate preview AG2).\n"
		"%s rate <m> - animation speed multiplier while paused (default 1.0).\n"
		"%s status  - current state.\n",
		cmd, cmd, cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_previewclock, "Experimental client-clock probe for the paused Customize preview (not a completed animation fix).") {
	Filmmaker::PreviewAnimClock_RunCommand(args->ArgC(), args, args->ArgV(0));
}
