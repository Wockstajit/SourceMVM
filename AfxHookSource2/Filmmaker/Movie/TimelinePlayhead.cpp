#include "TimelinePlayhead.h"

#include "../../MirvTime.h"

namespace Filmmaker {

namespace {

struct State {
	bool scrubbing = false;
	double tick = 0.0;
};

State& S() {
	static State s;
	return s;
}

} // namespace

void TimelinePlayhead_BeginScrub(double tick) {
	State& s = S();
	s.scrubbing = true;
	s.tick = tick;
}

void TimelinePlayhead_EndScrub() {
	S().scrubbing = false;
}

bool TimelinePlayhead_Scrubbing() {
	return S().scrubbing;
}

double TimelinePlayhead_Tick() {
	const State& s = S();
	if (s.scrubbing)
		return s.tick;
	int curTick = 0;
	g_MirvTime.GetCurrentDemoTick(curTick);
	return (double)curTick;
}

void TimelinePlayhead_Reset() {
	State& s = S();
	s.scrubbing = false;
	s.tick = 0.0;
}

} // namespace Filmmaker
