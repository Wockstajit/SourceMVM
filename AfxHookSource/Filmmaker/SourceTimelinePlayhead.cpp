#include "stdafx.h"

#include "SourceTimelinePlayhead.h"
#include "SourceDemoBar.h"
#include "SourceEngine.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>

#include <cmath>

namespace SourceFilmmaker {

namespace {

// How far the landed demo tick may sit from the released tick and still count as there: a seek can
// land one past it.
constexpr double kLandTolerance = 2.0;

struct State {
	bool scrubbing = false;
	double tick = 0.0;
	bool held = false;     // released, not yet handed back to the demo tick
	double heldTick = 0.0;
	int landedTick = -1;   // the demo tick once the seek stopped moving it, -1 before
};
State g;

void Drop(const char* why, int demoTick) {
	if (!g.held) return;
	g.held = false;
	g.landedTick = -1;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("playhead.hold", "drop why=%s held=%.0f demoTick=%d", why, g.heldTick, demoTick);
}

} // namespace

void TimelinePlayhead_BeginScrub(double tick) {
	g.scrubbing = true;
	g.tick = tick;
	g.held = false;
	g.landedTick = -1;
}

void TimelinePlayhead_EndScrub() {
	if (!g.scrubbing) return; // only a drag in progress is released
	g.scrubbing = false;
	g.held = true;
	int target = 0;
	g.heldTick = DemoBar_SeekTarget(target) ? (double)target : std::floor(g.tick + 0.5);
	g.landedTick = -1;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("playhead.hold", "hold tick=%.0f", g.heldTick);
}

bool TimelinePlayhead_Scrubbing() { return g.scrubbing; }

double TimelinePlayhead_Tick() {
	if (g.scrubbing) return g.tick;
	const int demoTick = Demo_State().tick;
	if (g.held) {
		int target = 0;
		if (DemoBar_SeekTarget(target)) { // still travelling: the demo tick is the old place
			if (std::fabs(target - g.heldTick) <= 0.5) return g.heldTick;
			Drop("rescrubbed", demoTick);
			return (double)demoTick;
		}
		if (g.landedTick < 0) {
			g.landedTick = demoTick;
			if (Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_Linef("playhead.hold", "landed held=%.0f demoTick=%d", g.heldTick, demoTick);
		}
		if (demoTick == g.landedTick && std::fabs(demoTick - g.heldTick) <= kLandTolerance) return g.heldTick;
		Drop(demoTick == g.landedTick ? "missed" : "moved", demoTick);
	}
	return (double)demoTick;
}

void TimelinePlayhead_Reset() { g = State(); }

} // namespace SourceFilmmaker
