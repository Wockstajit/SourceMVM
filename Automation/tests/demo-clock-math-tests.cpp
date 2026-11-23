#include "../../AfxHookSource2/Filmmaker/Movie/DemoClockMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// A CS2 demo as the clock sees it: the interpolated demo position `now` (ticks, fractional), the
// client clock = now + the demo's server start tick, and the engine's whole demo tick, which may run
// `lead` ticks ahead of the interpolated position (measured: up to a frame's worth at full speed).
struct Demo {
	double now = 1000.0;
	double startTick = 3173.0;
	double lead = 0.0;
	int Tick() const { return int(std::floor(now + lead)); }
	double ClientTicks() const { return now + startTick; }
};

// demo_timescale 0.08 at 60 fps advances 0.085 ticks a frame: the slow motion of issue 135.
constexpr double kSlowStep = 0.08 * 64.0 / 60.0;

void TestSlowPlaybackIsSmooth() {
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 1000.3;
	double previous = clock.Sample(demo.Tick(), demo.ClientTicks(), false);
	CHECK(std::fabs(previous - demo.now) < 1e-9);
	int moved = 0;
	for (int i = 0; i < 600; ++i) {
		demo.now += kSlowStep;
		const double t = clock.Sample(demo.Tick(), demo.ClientTicks(), false);
		CHECK(std::fabs(t - demo.now) < 1e-9);
		if (t > previous) ++moved;
		previous = t;
	}
	CHECK(moved == 600); // every frame moves, not one in twelve as the whole tick does
}

void TestWholeTickTimeSteps() {
	// The defect: whole-tick time moves on about one frame in twelve at this speed.
	Demo demo;
	int moved = 0, last = demo.Tick();
	for (int i = 0; i < 600; ++i) {
		demo.now += kSlowStep;
		if (demo.Tick() != last) ++moved;
		last = demo.Tick();
	}
	CHECK(moved < 600 / 10);
}

void TestTickRunningAheadAtFullSpeedIsIgnored() {
	// Measured 2026-11-07 at 1x and ~85 fps: the tick sometimes moves two in one frame and leads the
	// interpolated clock by up to 0.75 of a tick. The time must follow the clock, not the tick.
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 2000.4;
	double previous = clock.Sample(demo.Tick(), demo.ClientTicks(), false);
	const double step = 64.0 / 85.0;
	for (int i = 0; i < 2000; ++i) {
		demo.now += step;
		demo.lead = (i % 3 == 0) ? 0.74 : 0.0;
		const double t = clock.Sample(demo.Tick(), demo.ClientTicks(), false);
		CHECK(t > previous);
		CHECK(std::fabs((t - previous) - step) < 1e-6); // exactly the clock's own step
		previous = t;
	}
	CHECK(clock.offset == demo.startTick);
}

void TestFastScrubDoesNotRelearn() {
	// A scrub catch-up plays at up to 16x: a 60 fps frame spans 17 ticks, and the tick can lead by that.
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 500.5;
	clock.Sample(demo.Tick(), demo.ClientTicks(), true);
	double previous = demo.now;
	for (int i = 0; i < 100; ++i) {
		demo.now += 16.0 * 64.0 / 60.0;
		demo.lead = (i % 2) ? 17.0 : 0.0;
		const double t = clock.Sample(demo.Tick(), demo.ClientTicks(), false);
		CHECK(t > previous);
		CHECK(std::fabs(t - demo.now) < 1e-9);
		previous = t;
	}
}

void TestAPausedWholeTickCorrectsAPlayingGuess() {
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 1000.5;
	demo.lead = 0.6; // the first sample lands while the tick leads: the guess is a tick off
	clock.Sample(demo.Tick(), demo.ClientTicks(), false);
	CHECK(clock.learned && std::fabs(clock.offset - demo.startTick) <= 1.0);
	demo.lead = 0.0;
	demo.now = 1200.0;
	CHECK(clock.Sample(demo.Tick(), demo.ClientTicks(), true) == 1200.0);
	CHECK(clock.offset == demo.startTick);
}

void TestPausedBetweenTicksHoldsTheFraction() {
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 1200.0;
	clock.Sample(demo.Tick(), demo.ClientTicks(), true);
	demo.now = 1200.52;
	for (int i = 0; i < 5; ++i)
		CHECK(std::fabs(clock.Sample(demo.Tick(), demo.ClientTicks(), true) - 1200.52) < 1e-9);
}

void TestAnotherDemoRelearns() {
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 1000.5;
	clock.Sample(demo.Tick(), demo.ClientTicks(), false);
	CHECK(clock.offset == 3173.0);
	demo.startTick = 912.0; // another server start tick
	demo.now = 40.25;
	CHECK(std::fabs(clock.Sample(demo.Tick(), demo.ClientTicks(), false) - 40.25) < 1e-9);
	CHECK(clock.offset == 912.0);
}

void TestALoadingSeekKeepsTheOffset() {
	FractionalDemoClock clock;
	Demo demo;
	demo.now = 1000.5;
	clock.Sample(demo.Tick(), demo.ClientTicks(), false);
	CHECK(std::fabs(clock.Sample(-1, demo.ClientTicks(), false) - 1000.5) < 1e-9);
	CHECK(clock.offset == 3173.0);
}

void TestNotFinite() {
	FractionalDemoClock clock;
	CHECK(clock.Sample(77, std::nan(""), false) == 77.0);
	CHECK(!clock.learned);
}

} // namespace

int main() {
	TestSlowPlaybackIsSmooth();
	TestWholeTickTimeSteps();
	TestTickRunningAheadAtFullSpeedIsIgnored();
	TestFastScrubDoesNotRelearn();
	TestAPausedWholeTickCorrectsAPlayingGuess();
	TestPausedBetweenTicksHoldsTheFraction();
	TestAnotherDemoRelearns();
	TestALoadingSeekKeepsTheOffset();
	TestNotFinite();
	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "DemoClockMathTests: all passed\n";
	return 0;
}
