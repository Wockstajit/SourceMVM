// Unit tests for the Method Acting facial performance maths (see
// AfxHookSource2/Filmmaker/Movie/MethodActingMath.h).
//
// The hook itself cannot be tested -- it needs CS2 running -- so the two properties that make
// this feature usable in a movie tool are pinned here instead:
//
//   * the blink is a PURE FUNCTION of demo time, so a seek back to a tick reproduces the face;
//   * the squint envelope cannot overshoot, stick, or emit a non-finite weight into the engine.

#include "../../AfxHookSource2/Filmmaker/Movie/MethodActingMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using Filmmaker::MethodActingMath::BlinkParams;
using Filmmaker::MethodActingMath::SquintParams;
using Filmmaker::MethodActingMath::BlinkWeight;
using Filmmaker::MethodActingMath::SquintTarget;
using Filmmaker::MethodActingMath::SquintStep;
using Filmmaker::MethodActingMath::Combine;

int g_failures = 0;

void Fail(const char* name, const char* detail) {
	std::cerr << name << ": " << detail << "\n";
	++g_failures;
}

void Expect(const char* name, bool condition, const char* detail) {
	if (!condition) Fail(name, detail);
}

void ExpectNear(const char* name, double got, double want, double tol) {
	if (!(std::fabs(got - want) <= tol)) {
		std::cerr << name << ": expected " << want << " +/- " << tol << ", got " << got << "\n";
		++g_failures;
	}
}

void ExpectInRange(const char* name, double got, double lo, double hi) {
	if (!(got >= lo && got <= hi)) {
		std::cerr << name << ": expected " << lo << ".." << hi << ", got " << got << "\n";
		++g_failures;
	}
}

// --- blink ---------------------------------------------------------------------------------

// The whole reason the blink is hashed rather than random: scrubbing to a tick twice must give
// the same face both times, otherwise a re-render of the same frame does not match.
void TestBlinkIsDeterministic() {
	const BlinkParams p;
	for (double t = 0.0; t < 40.0; t += 0.013) {
		const double a = BlinkWeight(t, 7u, p);
		const double b = BlinkWeight(t, 7u, p);
		if (a != b) { Fail("blink deterministic", "same input gave two answers"); return; }
	}
}

// Counts complete blinks inside one window: a lid that reaches shut and comes back is one blink.
int BlinksInWindow(double windowStart, double windowLength, uint32_t seed, const BlinkParams& p) {
	int blinks = 0;
	bool open = true;
	for (double t = windowStart; t < windowStart + windowLength; t += 0.001) {
		const double w = BlinkWeight(t, seed, p);
		if (open && w > 0.9) { ++blinks; open = false; }
		else if (!open && w < 0.05) { open = true; }
	}
	return blinks;
}

// Every window must contain a complete blink: the lid reaches fully shut and comes back.
void TestBlinkCompletesEveryWindow() {
	BlinkParams p;
	p.intervalSec = 4.0;
	for (int window = 0; window < 12; ++window) {
		double peak = 0.0;
		const double start = window * p.intervalSec;
		for (double t = start; t < start + p.intervalSec; t += 0.002) {
			const double w = BlinkWeight(t, 1234u, p);
			ExpectInRange("blink range", w, 0.0, 1.0);
			if (w > peak) peak = w;
		}
		if (peak < 0.98) {
			Fail("blink completes", "a window contained no full blink");
			return;
		}
	}
}

// --- double blink ------------------------------------------------------------------------------

// The two ends of the slider have to be exact, not approximate: 0 must never produce a pair and
// 1 must always produce one, or the setting does not mean what the panel says it means.
void TestDoubleBlinkExtremes() {
	BlinkParams never;
	never.intervalSec = 4.0;
	never.doubleChance = 0.0;

	BlinkParams always = never;
	always.doubleChance = 1.0;

	for (int window = 0; window < 20; ++window) {
		const double start = window * never.intervalSec;
		const int singles = BlinksInWindow(start, never.intervalSec, 4242u, never);
		const int pairs = BlinksInWindow(start, always.intervalSec, 4242u, always);
		if (singles != 1) { Fail("double blink off", "a window was not a single blink"); return; }
		if (pairs != 2) { Fail("double blink on", "a window was not a pair"); return; }
	}
}

// A half-set slider should give roughly half pairs. Sampled across many windows and players, so
// this is about the draw being unbiased rather than about any one window.
void TestDoubleBlinkRateIsAboutHalf() {
	BlinkParams p;
	p.intervalSec = 4.0;
	p.doubleChance = 0.5;

	int windows = 0, pairs = 0;
	for (uint32_t seed = 1; seed <= 12; ++seed) {
		for (int window = 0; window < 25; ++window) {
			const int n = BlinksInWindow(window * p.intervalSec, p.intervalSec, seed, p);
			++windows;
			if (n == 2) ++pairs;
		}
	}
	const double rate = (double)pairs / (double)windows;
	ExpectInRange("double blink rate", rate, 0.40, 0.60);
}

// A pair must fit inside its own window like a single does, or a single-window lookup clips the
// second blink and it reads as a stutter.
void TestDoubleBlinkStaysInsideWindow() {
	BlinkParams p;
	p.intervalSec = 4.0;
	p.doubleChance = 1.0;
	for (int window = 0; window < 12; ++window) {
		const double edge = (window + 1) * p.intervalSec;
		if (BlinkWeight(edge - 1e-6, 77u, p) > 1e-6) {
			Fail("double blink boundary", "a pair was still open at the window edge");
			return;
		}
	}
}

// When the window is too short to hold a pair, fall back to a single rather than emit a clipped
// second blink. A fast blink rate must not start stuttering.
void TestDoubleBlinkDegradesToSingleInAShortWindow() {
	BlinkParams p;
	p.intervalSec = 0.25;   // shorter than close+open twice plus the gap
	p.doubleChance = 1.0;
	for (int window = 0; window < 12; ++window) {
		const double start = window * p.intervalSec;
		const int n = BlinksInWindow(start, p.intervalSec, 21u, p);
		if (n > 1) { Fail("double blink short window", "emitted a pair that could not fit"); return; }
		for (double t = start; t < start + p.intervalSec; t += 0.001)
			ExpectInRange("double blink short range", BlinkWeight(t, 21u, p), 0.0, 1.0);
	}
}

// The pair decision has to survive a seek like everything else here.
void TestDoubleBlinkIsDeterministic() {
	BlinkParams p;
	p.doubleChance = 0.5;
	for (double t = 0.0; t < 60.0; t += 0.017) {
		if (BlinkWeight(t, 9u, p) != BlinkWeight(t, 9u, p)) {
			Fail("double blink deterministic", "same input gave two answers");
			return;
		}
	}
}

// A blink must never straddle a window boundary, or a single-window lookup would clip it in half.
void TestBlinkClosesBeforeWindowEnd() {
	BlinkParams p;
	p.intervalSec = 4.0;
	p.doubleChance = 0.0;   // pairs get their own boundary test below
	for (int window = 0; window < 12; ++window) {
		const double edge = (window + 1) * p.intervalSec;
		// Sample right up to (but not past) the boundary.
		const double w = BlinkWeight(edge - 1e-6, 99u, p);
		if (w > 1e-6) { Fail("blink boundary", "a blink was still open at the window edge"); return; }
	}
}

// Two players standing together must not blink in lockstep.
void TestBlinkSeedsDiffer() {
	const BlinkParams p;
	int differing = 0;
	for (double t = 0.0; t < 60.0; t += 0.01) {
		if (std::fabs(BlinkWeight(t, 1u, p) - BlinkWeight(t, 2u, p)) > 0.25) ++differing;
	}
	Expect("blink seeds", differing > 0, "two seeds produced identical blink timing");
}

// The blink rate slider has to actually change the rate.
void TestBlinkRateRespondsToInterval() {
	auto countBlinks = [](double interval) {
		BlinkParams p;
		p.intervalSec = interval;
		int blinks = 0;
		bool open = true;
		for (double t = 0.0; t < 120.0; t += 0.005) {
			const double w = BlinkWeight(t, 5u, p);
			if (open && w > 0.5) { ++blinks; open = false; }
			else if (!open && w < 0.05) { open = true; }
		}
		return blinks;
	};
	const int fast = countBlinks(2.0);
	const int slow = countBlinks(8.0);
	Expect("blink rate", fast > slow * 2, "halving the interval did not roughly double the blinks");
}

// Degenerate parameters must produce a relaxed face, never NaN fed into a flex weight.
void TestBlinkRejectsDegenerateParams() {
	BlinkParams p;
	p.intervalSec = 0.0;
	ExpectNear("blink zero interval", BlinkWeight(3.0, 1u, p), 0.0, 0.0);
	p = BlinkParams();
	p.closeSec = 0.0;
	ExpectNear("blink zero close", BlinkWeight(3.0, 1u, p), 0.0, 0.0);
	p = BlinkParams();
	// A blink longer than its own window cannot fit; it must degrade, not wrap.
	p.intervalSec = 0.05;
	for (double t = 0.0; t < 2.0; t += 0.001)
		ExpectInRange("blink oversized", BlinkWeight(t, 1u, p), 0.0, 1.0);
	// Negative demo time (a clock that has not started) must not index a negative window.
	ExpectInRange("blink negative time", BlinkWeight(-5.0, 1u, BlinkParams()), 0.0, 1.0);
}

// --- squint ---------------------------------------------------------------------------------

void TestSquintTargetWindow() {
	SquintParams p;
	p.level = 0.5;
	p.holdSec = 0.25;
	ExpectNear("squint never fired", SquintTarget(-1.0, p), 0.0, 0.0);
	ExpectNear("squint at the shot", SquintTarget(0.0, p), 0.5, 1e-9);
	ExpectNear("squint inside hold", SquintTarget(0.24, p), 0.5, 1e-9);
	ExpectNear("squint past hold", SquintTarget(0.26, p), 0.0, 0.0);
}

// Attack and release are stated as full-travel times, so the level slider must not also change
// the speed -- a 90% squint and a 20% squint have to arrive at the same moment.
void TestSquintTimingIsLevelIndependent() {
	auto timeToReach = [](double level) {
		SquintParams p;
		p.level = level;
		p.attackSec = 0.06;
		double w = 0.0;
		double t = 0.0;
		for (int i = 0; i < 10000; ++i) {
			if (w >= level - 1e-6) break;
			w = SquintStep(w, level, 1.0 / 1000.0, p);
			t += 1.0 / 1000.0;
		}
		return t;
	};
	ExpectNear("squint attack timing", timeToReach(0.9), timeToReach(0.2), 0.005);
	ExpectNear("squint attack duration", timeToReach(0.5), 0.06, 0.005);
}

void TestSquintReleaseReturnsToZero() {
	SquintParams p;
	p.level = 0.5;
	p.releaseSec = 0.35;
	double w = 0.5;
	double t = 0.0;
	for (int i = 0; i < 10000 && w > 0.0; ++i) {
		w = SquintStep(w, 0.0, 1.0 / 1000.0, p);
		t += 1.0 / 1000.0;
	}
	ExpectNear("squint release", w, 0.0, 1e-9);
	ExpectNear("squint release duration", t, 0.35, 0.005);
}

// A step must never sail past the target, however large the frame time. A hitch used to be the
// classic way to make an envelope overshoot and then oscillate.
void TestSquintNeverOvershoots() {
	SquintParams p;
	p.level = 0.5;
	double w = 0.0;
	w = SquintStep(w, 0.5, 10.0, p);
	ExpectNear("squint big dt rising", w, 0.5, 1e-9);
	w = SquintStep(w, 0.0, 10.0, p);
	ExpectNear("squint big dt falling", w, 0.0, 1e-9);
}

void TestSquintDegenerateParams() {
	SquintParams p;
	p.level = 0.0;
	ExpectNear("squint zero level", SquintStep(0.4, 0.4, 0.016, p), 0.0, 0.0);
	p = SquintParams();
	ExpectNear("squint zero dt", SquintStep(0.3, 0.0, 0.0, p), 0.3, 1e-9);
	ExpectNear("squint negative dt", SquintStep(0.3, 0.0, -0.5, p), 0.3, 1e-9);
	p.level = 5.0;  // out of range from a bad config file
	ExpectInRange("squint clamped level", SquintStep(0.0, 5.0, 1.0, p), 0.0, 1.0);
}

// --- combine ---------------------------------------------------------------------------------

// The user-visible contract: a player can still blink mid-burst, and the blink is a FULL blink
// rather than a flicker clipped to the squint level.
void TestCombineLetsBlinkWinMidBurst() {
	ExpectNear("combine blink over squint", Combine(0.5, 1.0), 1.0, 1e-9);
	ExpectNear("combine squint over open eye", Combine(0.5, 0.0), 0.5, 1e-9);
	ExpectNear("combine both idle", Combine(0.0, 0.0), 0.0, 1e-9);
	ExpectInRange("combine clamps", Combine(3.0, 4.0), 0.0, 1.0);
}

// Nothing non-finite may ever reach a flex weight.
void TestCombineRejectsNonFinite() {
	const double nan = std::nan("");
	ExpectInRange("combine nan squint", Combine(nan, 0.4), 0.0, 1.0);
	ExpectInRange("combine nan blink", Combine(0.4, nan), 0.0, 1.0);
}

// --- integration: one burst ----------------------------------------------------------------

// Walks a realistic burst at 64 tick and asserts the shape the user asked for: eyes narrow while
// firing, stay narrowed through the buffer, then relax.
void TestFiringBurstShape() {
	SquintParams p;
	p.level = 0.5;
	p.attackSec = 0.06;
	p.holdSec = 0.25;
	p.releaseSec = 0.35;

	const double dt = 1.0 / 64.0;
	const double lastShot = 1.0;   // a burst whose final round lands at t = 1.0 s
	double w = 0.0;

	// Firing: shots keep arriving, so the target is held.
	for (double t = 0.0; t <= lastShot; t += dt)
		w = SquintStep(w, SquintTarget(0.0, p), dt, p);
	ExpectNear("burst peak", w, 0.5, 1e-6);

	// The buffer: still squinting a fifth of a second after the last round.
	w = SquintStep(w, SquintTarget(0.2, p), dt, p);
	ExpectNear("burst during buffer", w, 0.5, 1e-6);

	// Past the buffer: relaxing, and fully relaxed a release later.
	double t = lastShot + p.holdSec;
	for (int i = 0; i < 64 && w > 0.0; ++i) {
		t += dt;
		w = SquintStep(w, SquintTarget(t - lastShot, p), dt, p);
	}
	ExpectNear("burst relaxed", w, 0.0, 1e-9);
}

void TestGazeAndDeath() {
    using namespace Filmmaker::MethodActingMath;
    for(int n=0;n<2000;++n) {
        const auto a=Wander(n*.017,17), b=Wander(n*.017,17);
        ExpectNear("seek gaze yaw",a.yaw,b.yaw,0);
        if(std::abs(a.yaw)>48.001 || std::abs(a.up)>2.501) Fail("wander","outside idle bounds");
        const auto pose=WanderPose(n*.017,17);
        ExpectInRange("head turn bounds",pose.headYaw,-45,45);
        ExpectNear("same wander gaze",pose.gaze.yaw,a.yaw,0);
        float q[4];EyeRotation(a,q);
        ExpectNear("unit eye rotation",q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3],1,1e-6);
    }
    int left=0,right=0,center=0;
    for(int n=0;n<120;++n) {
        const auto glance=Wander(n*WanderPeriod(17)+.5,17);
        if(glance.yaw < -7) ++left;
        else if(glance.yaw > 7) ++right;
        else ++center;
    }
    if(left<15 || right<15 || center<3) Fail("wander variety","needs held left, right and resting glances");
    ExpectNear("held glance yaw",WanderPose(.45,17).gaze.yaw,WanderPose(.95,17).gaze.yaw,0);
    ExpectNear("held glance head",WanderPose(.45,17).headYaw,WanderPose(.95,17).headYaw,0);
    ExpectNear("rest between glances",WanderPose(1.5,17).headYaw,0,0);
    int wide=0;
    for(int seed=1;seed<=40;++seed) for(int cycle=0;cycle<30;++cycle)
        if(std::abs(WanderPose(cycle*WanderPeriod((uint32_t)seed)+.5,(uint32_t)seed).headYaw)>15) ++wide;
    if(wide<350 || wide>650) Fail("head glance frequency","wide turns should occur on roughly 45% of side glances");
    for(uint32_t seed=1;seed<=40;++seed) {
        ExpectInRange("faster glance cadence",WanderPeriod(seed),1.6,2.3);
        for(int n=0;n<500;++n) {
            const auto combat=CombatWanderPose(n*.017,seed);
            ExpectInRange("combat head stays restrained",combat.headYaw,-9,9);
            ExpectInRange("combat eyes stay restrained",combat.gaze.yaw,-10.56,10.56);
        }
    }
    ExpectNear("head follow center",HeadFollowYaw(0),0,0);
    ExpectNear("head follow small",HeadFollowYaw(10),2.8,1e-9);
    ExpectNear("head follow left cap",HeadFollowYaw(100),4,0);
    ExpectNear("head follow right cap",HeadFollowYaw(-100),-4,0);
    ExpectNear("head pause",HeadStep(7,0,0),7,0);
    const float muzzle[3]={0,0,0}, impact[3]={1000,0,0};
    const float nearFace[3]={450,95,0}, farFace[3]={450,150,0}, behindBullet[3]={-100,0,0};
    if(!NearBullet(muzzle,impact,nearFace,120) || NearBullet(muzzle,impact,farFace,120) ||
       NearBullet(muzzle,impact,behindBullet,50)) Fail("incoming fire","bullet segment proximity wrong");
    ExpectNear("incoming squint hold",ThreatSquintTarget(.9,SquintParams()),.4,1e-9);
    ExpectNear("incoming squint release",ThreatSquintTarget(1.2,SquintParams()),0,0);
    float forward[3]={0,100,0},behind[3]={0,-100,0},side[3]={0,30,90},above[3]={15,100,10};Gaze g;
    if(!NearbyGaze(forward,100,g)||!NearbyGaze(above,102,g)) Fail("nearby","front target rejected");
    if(NearbyGaze(behind,100,g)||NearbyGaze(side,95,g)||NearbyGaze(forward,221,g)||NearbyGaze(forward,2,g)) Fail("nearby","invalid target accepted");
    Gaze a{},b{},target{20,10};
    for(int n=0;n<30;++n)a=GazeStep(a,target,1./30);
    for(int n=0;n<128;++n)b=GazeStep(b,target,1./128);
    ExpectNear("gaze rate independence",a.yaw,b.yaw,1e-9);
    ExpectNear("pause gaze",GazeStep(a,target,0).yaw,a.yaw,0);
    ExpectNear("death initial lid",DeathEyePose(0).closed,0,0);
    if(DeathEyePose(.25).up<7 || DeathEyePose(.25).closed!=0) Fail("death order","lids lowered before eye roll");
    double last=-1;
    for(int n=0;n<150;++n) {
        auto pose=DeathEyePose(n*.02);
        if(pose.closed<last)Fail("death lid","reopened");
        ExpectInRange("death lid avoids pinching",pose.closed,0,.12);
        ExpectInRange("death roll avoids extreme rotation",pose.up,0,25);
        ExpectInRange("death roll sideways",pose.yaw,-7,7);
        last=pose.closed;
    }
    ExpectNear("dead relaxed lid",DeathEyePose(100).closed,.12,0);
    ExpectNear("dead upward gaze",DeathEyePose(100).up,22.0,0);
    bool bothSides=false;
    for(uint32_t seed=1;seed<64;++seed) if(DeathEyePose(100,seed).yaw!=DeathEyePose(100,1).yaw) bothSides=true;
    if(!bothSides) Fail("death side","seed must vary side");
    // An eyeball's local +X is forward; rest maps it to head +Y.
    float q[4];EyeRotation({0,0},q);
    ExpectNear("eye forward rest Y",2*(q[0]*q[1]+q[3]*q[2]),1,1e-6);
    EyeRotation({0,28},q);
    if(1-2*(q[1]*q[1]+q[2]*q[2])<.4)Fail("eye roll","not head-up");
    EyeRotation({20,0},q);
    if(2*(q[0]*q[2]-q[3]*q[1])<.3)Fail("eye yaw","not head-left");
}

} // namespace

int main() {
    TestGazeAndDeath();
	TestBlinkIsDeterministic();
	TestBlinkCompletesEveryWindow();
	TestBlinkClosesBeforeWindowEnd();
	TestDoubleBlinkExtremes();
	TestDoubleBlinkRateIsAboutHalf();
	TestDoubleBlinkStaysInsideWindow();
	TestDoubleBlinkDegradesToSingleInAShortWindow();
	TestDoubleBlinkIsDeterministic();
	TestBlinkSeedsDiffer();
	TestBlinkRateRespondsToInterval();
	TestBlinkRejectsDegenerateParams();

	TestSquintTargetWindow();
	TestSquintTimingIsLevelIndependent();
	TestSquintReleaseReturnsToZero();
	TestSquintNeverOvershoots();
	TestSquintDegenerateParams();

	TestCombineLetsBlinkWinMidBurst();
	TestCombineRejectsNonFinite();

	TestFiringBurstShape();

	if (g_failures) {
		std::cerr << g_failures << " failure(s)\n";
		return EXIT_FAILURE;
	}
	std::cout << "MethodActingMathTests: all checks passed\n";
	return EXIT_SUCCESS;
}
