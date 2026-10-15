#include "../../AfxHookSource2/Filmmaker/Movie/DemoScrubMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// A CS2 demo as the controller sees it: plays at demo_timescale, pauses ONE FRAME after the
// command (commands take effect next frame), steps to whole ticks, seeks to whole ticks (some
// seeks land one past, like the engine's).
struct Engine {
	double now = 1000.0;
	bool paused = true;
	double timescale = 1.0;
	bool pausePending = false;
	int seeks = 0, steps = 0, plays = 0, pauses = 0;
	bool seekLandsPast = false;
	double maxBackwardJump = 0.0; // how far one frame ever moved the world backward
	double dt = 1.0 / 144.0;

	void Apply(const ScrubDecision& d) {
		switch (d.action) {
		case ScrubAction::Play: timescale = d.timescale; paused = false; ++plays; break;
		case ScrubAction::Pause: pausePending = true; ++pauses; break;
		case ScrubAction::Step: now = std::floor(now + 1e-9) + d.ticks; paused = true; ++steps; break;
		case ScrubAction::Seek: now = d.seekTick + (seekLandsPast ? 1 : 0); paused = true; ++seeks; break;
		default: break;
		}
	}
	void Frame() {
		if (!paused) now += timescale * kScrubTickRate * dt;
		if (pausePending) { paused = true; pausePending = false; }
	}
	// Runs the controller for up to `frames` frames toward `target`.
	ScrubAction Run(double target, bool released, int frames = 2000) {
		bool settled = false;
		ScrubAction last = ScrubAction::None;
		for (int i = 0; i < frames; ++i) {
			ScrubInput in;
			in.target = target; in.now = now; in.paused = paused && !pausePending;
			in.busy = pausePending; in.released = released; in.settled = settled;
			in.frameSeconds = dt; in.timescale = timescale;
			const ScrubDecision d = DecideScrub(in);
			if (d.action == ScrubAction::Seek && released) settled = true;
			last = d.action;
			if (d.action == ScrubAction::Done) return last;
			const double before = now;
			Apply(d);
			Frame();
			if (before - now > maxBackwardJump && d.action != ScrubAction::Seek) maxBackwardJump = before - now;
		}
		return last;
	}
};

void TestSlowDragGlidesToAFraction() {
	// The playhead sits between two ticks: the world plays there and stops INSIDE the tick.
	Engine e; e.now = 1000.0;
	e.Run(1000.4, false);
	CHECK(e.paused);
	CHECK(std::fabs(e.now - 1000.4) <= kScrubArriveTicks + 0.01);
	CHECK(e.seeks == 0 && e.steps == 0); // played, never skipped
}

void TestDragFollowsWithoutOvershoot() {
	// A slow drag, 0.25 tick every frame: the world keeps following and never passes the playhead
	// by more than the arrive tolerance.
	Engine e; e.now = 1000.0;
	double target = 1000.0;
	double worstPast = 0.0;
	for (int f = 0; f < 200; ++f) {
		target += 0.05;
		ScrubInput in;
		in.target = target; in.now = e.now; in.paused = e.paused && !e.pausePending; in.busy = e.pausePending;
		in.frameSeconds = e.dt; in.timescale = e.timescale;
		e.Apply(DecideScrub(in));
		e.Frame();
		if (e.now - target > worstPast) worstPast = e.now - target;
	}
	CHECK(worstPast <= 0.05);
	CHECK(target - e.now < 1.0); // keeps up with the drag
	CHECK(e.seeks == 0);
}

void TestSpeedFollowsTheGap() {
	CHECK(ScrubPlaySpeed(0.001) == kScrubMinTimescale);
	CHECK(ScrubPlaySpeed(100000) == kScrubMaxTimescale);
	CHECK(ScrubPlaySpeed(10) > ScrubPlaySpeed(1)); // further away = faster
}

void TestBackwardRestoresThenGlides() {
	Engine e; e.now = 1000.0;
	e.Run(900.6, false);
	CHECK(e.seeks == 1);
	CHECK(std::fabs(e.now - 900.6) <= kScrubArriveTicks + 0.01);
	// A seek that lands one past still ends on the fraction: it restores the tick BEFORE.
	Engine p; p.now = 1000.0; p.seekLandsPast = true;
	p.Run(900.6, false);
	CHECK(std::fabs(p.now - 900.6) <= kScrubArriveTicks + 0.01);
}

void TestSmallOvershootHolds() {
	// Less than a tick past the playhead is not worth a restore while dragging...
	ScrubInput in; in.target = 1000.0; in.now = 1000.03; in.paused = true;
	CHECK(DecideScrub(in).action == ScrubAction::None);
	in.now = 1000.8;
	CHECK(DecideScrub(in).action == ScrubAction::None);
	// ...more than a tick is a real backward move.
	in.now = 1001.2;
	CHECK(DecideScrub(in).action == ScrubAction::Seek);
	// A release still lands exactly: 0.8 past a whole tick is re-seeked and played in.
	in.now = 1000.8; in.released = true;
	CHECK(DecideScrub(in).action == ScrubAction::Seek);
}

void TestReleaseLandsOnAWholeTick() {
	// From a fraction, forward: a step lands exactly on the whole tick.
	Engine e; e.now = 1000.37;
	CHECK(e.Run(1003.2, true) == ScrubAction::Done);
	CHECK(e.now == 1003.0);
	// Released while gliding: pause first, then land.
	Engine g; g.now = 1000.0; g.paused = false; g.timescale = 1.0;
	CHECK(g.Run(1010.0, true) == ScrubAction::Done);
	CHECK(g.now == 1010.0);
	// Backward: settle seek short of the target, then played in.
	Engine b; b.now = 1000.0;
	CHECK(b.Run(512.4, true) == ScrubAction::Done);
	CHECK(b.now == 512.0);
	CHECK(b.seeks == 1 && b.steps == 1);
	// A settle seek that lands one past is still played in to exactly the tick.
	Engine bp; bp.now = 1000.0; bp.seekLandsPast = true;
	CHECK(bp.Run(512.0, true) == ScrubAction::Done);
	CHECK(bp.now == 512.0);
}

void TestReleaseAcceptsOnePastAfterSettle() {
	ScrubInput in; in.target = 500; in.now = 501; in.paused = true; in.released = true;
	CHECK(DecideScrub(in).action == ScrubAction::Seek); // re-seek short and play in once...
	in.settled = true;
	CHECK(DecideScrub(in).action == ScrubAction::Done); // ...then one past is accepted
	in.now = 502;
	CHECK(DecideScrub(in).action == ScrubAction::Seek);
}

void TestFarForwardSkipsThenGlides() {
	Engine e; e.now = 100.0;
	e.Run(5000.5, false);
	CHECK(e.seeks == 1);
	CHECK(std::fabs(e.now - 5000.5) <= kScrubArriveTicks + 0.01);
}

void TestMediumForwardDragSeeks() {
	// 70 ticks ahead used to be played at ~10x and stuttered; it seeks now, then glides the rest.
	ScrubInput in; in.target = 1070.4; in.now = 1000.0; in.paused = true;
	CHECK(DecideScrub(in).action == ScrubAction::Seek);
	Engine e; e.now = 1000.0;
	e.Run(1070.4, false);
	CHECK(e.seeks == 1);
	CHECK(std::fabs(e.now - 1070.4) <= kScrubArriveTicks + 0.01);
}

void TestBusyWaits() {
	ScrubInput in; in.target = 2000; in.now = 1000; in.busy = true;
	CHECK(DecideScrub(in).action == ScrubAction::None);
	in.released = true;
	CHECK(DecideScrub(in).action == ScrubAction::None);
}

void TestNeverPausesAPausedDemo() {
	// demo_pause is a toggle: Pause is only ever decided while playing.
	for (double now = 990.0; now < 1010.0; now += 0.37) {
		for (int r = 0; r < 2; ++r) {
			ScrubInput in; in.target = 1000.0; in.now = now; in.paused = true; in.released = r == 1;
			CHECK(DecideScrub(in).action != ScrubAction::Pause);
		}
	}
}

// The 2026-10-09 freeze: CS2's own demo bar seeked while the scrub's seek was in flight, and the
// scrub waited 1200 frames for a landing that could never come.
void TestSeekJudgedEveryFrame() {
	// Landing (paused, +/-1) is the only success; playing there still waits for the re-pause.
	CHECK(JudgeSeek(74133, 73688, 73688, true, 2, true) == SeekWait::Landed);
	CHECK(JudgeSeek(74133, 73688, 73689, true, 2, true) == SeekWait::Landed);
	CHECK(JudgeSeek(74133, 73688, 73688, false, 2, true) == SeekWait::Waiting);
	// Mid-replay a backward seek still reads the old tick: wait.
	CHECK(JudgeSeek(30541, 30007, 30541, true, 1, true) == SeekWait::Waiting);
	// Measured: our seek to 30007 landed, then the bar's own seek took the world back to 30541 and
	// on to 32938 -- outside the range, so lost at once.
	CHECK(JudgeSeek(30541, 30007, 32938, true, 3, true) == SeekWait::Lost);
	CHECK(JudgeSeek(9765, 34446, 23173, false, 3, true) == SeekWait::Waiting); // inside, playing briefly
	CHECK(JudgeSeek(9765, 34446, 5000, true, 3, true) == SeekWait::Lost);
	// Playing away from the target after the pause blip window: something resumed it.
	CHECK(JudgeSeek(9765, 34446, 23173, false, kSeekPlayingLostFrames, true) == SeekWait::Lost);
	// Measured: seek 73688 from 74133 never ran (swallowed); the world sat on 74133, paused.
	CHECK(JudgeSeek(74133, 73688, 74133, true, kSeekStallFramesKeyframed - 1, true) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 73688, 74133, true, kSeekStallFramesKeyframed, true) == SeekWait::Lost);
	// Without keyframes a long backward replay holds the old tick ~160 frames: keep waiting.
	CHECK(JudgeSeek(74133, 71000, 74133, true, 160, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 71000, 74133, true, kSeekStallFramesPlain, false) == SeekWait::Lost);
	// A forward replay into demo not yet played moves through the range: progress, not a stall.
	CHECK(JudgeSeek(1000, 40000, 12000, true, 600, true) == SeekWait::Waiting);
}

} // namespace

int main() {
	TestSeekJudgedEveryFrame();
	TestSlowDragGlidesToAFraction();
	TestDragFollowsWithoutOvershoot();
	TestSpeedFollowsTheGap();
	TestBackwardRestoresThenGlides();
	TestSmallOvershootHolds();
	TestReleaseLandsOnAWholeTick();
	TestReleaseAcceptsOnePastAfterSettle();
	TestFarForwardSkipsThenGlides();
	TestMediumForwardDragSeeks();
	TestBusyWaits();
	TestNeverPausesAPausedDemo();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "demo scrub math tests passed\n";
	return 0;
}
