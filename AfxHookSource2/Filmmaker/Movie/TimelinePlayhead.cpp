#include "TimelinePlayhead.h"

#include "DemoScrub.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"

#include <cmath>

namespace Filmmaker {

namespace {

// How far the landed demo tick may sit from the released tick and still count as "there". A
// release lands on the nearest whole tick, and the tick read right after a skip can be one past
// it (Docs/features/filmmaker_demo_scrubbing.md); DemoScrub gives up re-aiming a tick or two off.
constexpr double kLandTolerance = 2.0;

struct State {
	bool scrubbing = false;
	double tick = 0.0;
	// Released and not yet handed back to the demo clock (see TimelinePlayhead.h, "Release").
	bool held = false;
	double heldTick = 0.0;
	int landedTick = -1; // the demo tick once DemoScrub stopped moving the world, or -1 before
};

State& S() {
	static State s;
	return s;
}

void Drop(State& s, const char* why, int demoTick) {
	if (!s.held) return;
	s.held = false;
	s.landedTick = -1;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("playhead.hold", "drop why=%s held=%.0f demoTick=%d", why, s.heldTick, demoTick);
}

} // namespace

void TimelinePlayhead_BeginScrub(double tick) {
	State& s = S();
	s.scrubbing = true;
	s.tick = tick;
	s.held = false;
	s.landedTick = -1;
}

void TimelinePlayhead_EndScrub() {
	State& s = S();
	// Only a scrub that was in progress is RELEASED. EndScrub is also the "follow the demo"
	// reset on graph enter/exit/clear; with no drag open that must not hold a stale tick.
	if (!s.scrubbing) return;
	s.scrubbing = false;
	s.held = true;
	// Hold the tick the world was actually sent to. Every caller releases the demo scrub first
	// (DemoScrub_Release), and that tick can differ from the last dragged one by the final mouse
	// move; without a world seek in flight, the last dragged tick is the answer.
	double target = s.tick;
	DemoScrub_Target(target);
	s.heldTick = std::floor(target + 0.5); // a release lands on the nearest whole tick
	s.landedTick = -1;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("playhead.hold", "hold tick=%.0f", s.heldTick);
}

bool TimelinePlayhead_Scrubbing() {
	return S().scrubbing;
}

double TimelinePlayhead_Tick() {
	State& s = S();
	if (s.scrubbing)
		return s.tick;
	int curTick = 0;
	g_MirvTime.GetCurrentDemoTick(curTick);
	if (s.held) {
		// The world is still travelling to the released tick: the demo tick is the OLD position
		// until the seek lands, and reading it made the playhead jump back for the whole seek.
		// A scrub toward somewhere else (CS2's own demo bar) is not ours to hold.
		double target = 0.0;
		if (DemoScrub_Target(target)) {
			if (std::fabs(target - s.heldTick) <= 0.5) return s.heldTick;
			Drop(s, "rescrubbed", curTick);
			return (double)curTick;
		}
		if (s.landedTick < 0) {
			s.landedTick = curTick;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("playhead.hold", "landed held=%.0f demoTick=%d", s.heldTick, curTick);
		}
		// Landed within a tick or two of the request and not moved since: the playhead stays on
		// the tick that was asked for, so a key jumped to reads as "at key" and an edit re-keys it
		// instead of dropping a second key one tick away. Playback, a step or any other seek moves
		// the demo tick and hands the playhead back.
		if (curTick == s.landedTick && std::fabs(curTick - s.heldTick) <= kLandTolerance) return s.heldTick;
		Drop(s, curTick == s.landedTick ? "missed" : "moved", curTick);
	}
	return (double)curTick;
}

void TimelinePlayhead_Reset() {
	State& s = S();
	s.scrubbing = false;
	s.tick = 0.0;
	s.held = false;
	s.landedTick = -1;
}

} // namespace Filmmaker
