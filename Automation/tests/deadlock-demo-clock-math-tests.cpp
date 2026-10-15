#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockDemoClockMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using Filmmaker::DemoClock;
using Filmmaker::DemoClockConfig;
using Filmmaker::DemoClockEvent;
using Filmmaker::DemoClockStep;

int g_failures = 0;

void Fail(const char* name, const char* detail) {
	std::cerr << "FAIL " << name << ": " << detail << "\n";
	++g_failures;
}

const char* EventName(DemoClockEvent e) {
	switch (e) {
	case DemoClockEvent::FirstSample: return "FirstSample";
	case DemoClockEvent::Advanced: return "Advanced";
	case DemoClockEvent::Jitter: return "Jitter";
	case DemoClockEvent::Seek: return "Seek";
	case DemoClockEvent::Clamped: return "Clamped";
	}
	return "?";
}

void ExpectEvent(const char* name, const DemoClockStep& step, DemoClockEvent expected) {
	if (step.event != expected) {
		std::cerr << "FAIL " << name << ": expected " << EventName(expected)
			<< ", got " << EventName(step.event) << "\n";
		++g_failures;
	}
}

void ExpectDelta(const char* name, const DemoClockStep& step, double expected) {
	if (std::fabs(step.delta - expected) > 1e-9) {
		std::cerr << "FAIL " << name << ": expected delta " << expected
			<< ", got " << step.delta << "\n";
		++g_failures;
	}
}

// The first sample has nothing to measure against, so it must integrate nothing. Getting this
// wrong makes the very first frame of every shot jump by however far the demo happens to be in.
void TestFirstSampleIntegratesNothing() {
	DemoClock clock;
	const DemoClockStep step = clock.Sample(120.0);
	ExpectEvent("first sample", step, DemoClockEvent::FirstSample);
	ExpectDelta("first sample", step, 0.0);
	if (step.shouldReset()) Fail("first sample", "must not ask for a reset");
}

void TestOrdinaryProgress() {
	DemoClock clock;
	clock.Sample(10.0);
	const DemoClockStep step = clock.Sample(10.015625); // one tick at 64 Hz
	ExpectEvent("ordinary progress", step, DemoClockEvent::Advanced);
	ExpectDelta("ordinary progress", step, 0.015625);
}

// THE regression this file exists for. Demo time steps backwards a few milliseconds on ordinary
// frames; if that reads as a seek, every shot-driven effect resets constantly and looks like a
// smoothing bug rather than a clock bug.
void TestSmallBackwardsStepIsJitterNotSeek() {
	DemoClock clock;
	clock.Sample(10.0);
	const DemoClockStep step = clock.Sample(10.0 - 0.004);
	ExpectEvent("4ms backwards", step, DemoClockEvent::Jitter);
	ExpectDelta("4ms backwards", step, 0.0);
	if (step.shouldReset())
		Fail("4ms backwards", "jitter must not trigger a reset -- this is the whole bug");
}

// Either side of the tolerance, kept clear of it. Asserting behaviour AT a floating-point boundary
// tests the representation of 0.050 rather than the clock: 10.0 - 0.050 does not produce a delta
// of exactly -0.050, which is how the first version of this test failed.
void TestJitterInsideToleranceAndSeekOutsideIt() {
	{
		DemoClock clock;
		clock.Sample(10.0);
		const DemoClockStep step = clock.Sample(10.0 - 0.045);
		ExpectEvent("45ms backwards", step, DemoClockEvent::Jitter);
		if (step.shouldReset()) Fail("45ms backwards", "inside the tolerance is not a seek");
	}
	{
		DemoClock clock;
		clock.Sample(10.0);
		const DemoClockStep step = clock.Sample(10.0 - 0.060);
		ExpectEvent("60ms backwards", step, DemoClockEvent::Seek);
		if (!step.shouldReset()) Fail("60ms backwards", "outside the tolerance is a seek");
	}
}

void TestLargeBackwardsJumpIsASeek() {
	DemoClock clock;
	clock.Sample(200.0);
	const DemoClockStep step = clock.Sample(50.0);
	ExpectEvent("backwards seek", step, DemoClockEvent::Seek);
	ExpectDelta("backwards seek", step, 0.0);
	if (!step.shouldReset()) Fail("backwards seek", "a real seek must ask for a reset");
}

void TestForwardHitchIsClampedNotIntegrated() {
	DemoClock clock;
	clock.Sample(10.0);
	const DemoClockStep step = clock.Sample(30.0); // a load, or a resume from a long pause
	ExpectEvent("forward hitch", step, DemoClockEvent::Clamped);
	ExpectDelta("forward hitch", step, 0.5);
	if (step.shouldReset())
		Fail("forward hitch", "a forward hitch is not a seek; clamping is enough");
}

// A jitter step must leave the baseline alone, so the frame after it is measured from the
// pre-jitter high-water mark. Measuring from the dipped value instead re-covers the dip and
// over-counts elapsed time, which is what the integration test below catches.
void TestJitterHoldsTheBaseline() {
	DemoClock clock;
	clock.Sample(10.000);
	clock.Sample(9.996);                              // jitter; the baseline stays at 10.000
	const DemoClockStep step = clock.Sample(10.012);
	ExpectEvent("after jitter", step, DemoClockEvent::Advanced);
	ExpectDelta("after jitter", step, 0.012);
}

void TestResetMakesTheNextSampleFirstAgain() {
	DemoClock clock;
	clock.Sample(10.0);
	clock.Reset();
	if (clock.HaveSample()) Fail("reset", "reset must forget the previous sample");
	const DemoClockStep step = clock.Sample(500.0);
	ExpectEvent("after reset", step, DemoClockEvent::FirstSample);
	ExpectDelta("after reset", step, 0.0);
}

// A paused demo hands the same timestamp over and over. Zero is forward progress of zero, not
// jitter and not a seek.
void TestPausedDemoYieldsZeroAndNoReset() {
	DemoClock clock;
	clock.Sample(42.0);
	for (int i = 0; i < 5; ++i) {
		const DemoClockStep step = clock.Sample(42.0);
		ExpectEvent("paused", step, DemoClockEvent::Advanced);
		ExpectDelta("paused", step, 0.0);
		if (step.shouldReset()) Fail("paused", "a paused demo must not read as a seek");
	}
}

void TestConfigurableTolerances() {
	DemoClockConfig config;
	config.backwardsJitterTolerance = 0.001;
	config.maxForwardStep = 0.100;
	DemoClock clock(config);
	clock.Sample(10.0);

	// 4 ms backwards now exceeds a 1 ms tolerance, so it IS a seek under this config.
	const DemoClockStep seek = clock.Sample(9.996);
	ExpectEvent("tight tolerance", seek, DemoClockEvent::Seek);

	clock.Reset();
	clock.Sample(0.0);
	const DemoClockStep clamped = clock.Sample(0.4);
	ExpectEvent("tight clamp", clamped, DemoClockEvent::Clamped);
	ExpectDelta("tight clamp", clamped, 0.100);
}

// Integrating a realistic frame sequence, jitter included, must land on the true elapsed time
// rather than drifting. This is what a camera move's duration actually depends on.
void TestIntegratedElapsedTimeTracksTheDemo() {
	DemoClock clock;
	const double samples[] = {
		100.000, 100.016, 100.032, 100.030, 100.048, 100.064, 100.061, 100.080, 100.096,
	};
	double accumulated = 0.0;
	for (double t : samples) accumulated += clock.Sample(t).delta;

	// Because jitter holds the baseline instead of advancing it, the integrated total comes out
	// EXACTLY equal to the demo's true span -- no double-counting of the backwards excursions,
	// and no lost time either. This is the assertion that pins the design.
	const double trueSpan = samples[8] - samples[0];
	if (std::fabs(accumulated - trueSpan) > 1e-9) {
		std::cerr << "FAIL integration: span " << trueSpan
			<< ", accumulated " << accumulated << "\n";
		++g_failures;
	}
}

} // namespace

int main() {
	TestFirstSampleIntegratesNothing();
	TestOrdinaryProgress();
	TestSmallBackwardsStepIsJitterNotSeek();
	TestJitterInsideToleranceAndSeekOutsideIt();
	TestLargeBackwardsJumpIsASeek();
	TestForwardHitchIsClampedNotIntegrated();
	TestJitterHoldsTheBaseline();
	TestResetMakesTheNextSampleFirstAgain();
	TestPausedDemoYieldsZeroAndNoReset();
	TestConfigurableTolerances();
	TestIntegratedElapsedTimeTracksTheDemo();

	if (g_failures) {
		std::cerr << g_failures << " failure(s)\n";
		return 1;
	}
	std::cout << "DeadlockDemoClockMath: all checks passed\n";
	return 0;
}
