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
	int packetGap = 0; // > 1: a seek stops on the next packet, one every packetGap ticks (CS:S)
	double maxBackwardJump = 0.0; // how far one frame ever moved the world backward
	double dt = 1.0 / 144.0;

	void Apply(const ScrubDecision& d) {
		switch (d.action) {
		case ScrubAction::Play: timescale = d.timescale; paused = false; ++plays; break;
		case ScrubAction::Pause: pausePending = true; ++pauses; break;
		case ScrubAction::Step: now = std::floor(now + 1e-9) + d.ticks; paused = true; ++steps; break;
		case ScrubAction::Seek:
			now = packetGap > 1 ? std::ceil((double)d.seekTick / packetGap) * packetGap : d.seekTick + (seekLandsPast ? 1 : 0);
			paused = true;
			++seeks;
			break;
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
			in.seekLandsPast = packetGap > 1 ? packetGap - 1 : 1;
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

void TestBackwardDragShowsOnlyPlayedFrames() {
	// A slow backward drag, a quarter tick a frame, with seeks landing exactly or one past. The
	// preview holds from every seek until the controller is idle again (DemoScrub_HoldPreview), so
	// the frames it SHOWS are the ones where the decision is None. Each of those must have been
	// played into since the last restore (a raw restore has no animation: bind-pose players), must
	// never be further forward than the one before it (no saw-tooth), and must keep up with the drag.
	// Slow (a quarter tick a frame) and fast (a tick and a half: the playhead outruns the restores,
	// which is where the play-in used to be skipped and bind-pose frames reached the screen).
	const double speeds[2] = { 0.25, 1.5 };
	for (double speed : speeds)
	for (int past = 0; past < 2; ++past) {
		Engine e; e.now = 1000.0; e.seekLandsPast = past != 0;
		double target = 1000.0, lastShown = 1e9, worstAhead = 0.0, worstBehind = 0.0;
		bool restored = false; // the driver's g_needsPlayIn
		int shown = 0;
		for (int f = 0; f < 240; ++f) {
			target -= speed;
			ScrubInput in;
			in.target = target; in.now = e.now; in.paused = e.paused && !e.pausePending; in.busy = e.pausePending;
			in.frameSeconds = e.dt; in.timescale = e.timescale; in.draggingBack = true; in.restoredNotPlayed = restored;
			const ScrubDecision d = DecideScrub(in);
			if (d.action == ScrubAction::None && !e.pausePending) {
				++shown;
				CHECK(!restored);                  // every shown frame was played into after its restore
				CHECK(e.now <= lastShown + 1e-9);  // and the shown frames only ever go back
				lastShown = e.now;
				if (e.now - target > worstAhead) worstAhead = e.now - target;
				if (target - e.now > worstBehind) worstBehind = target - e.now;
			}
			if (d.action == ScrubAction::Seek) restored = true;
			if (d.action == ScrubAction::Step) restored = false;
			e.Apply(d);
			e.Frame();
		}
		CHECK(e.plays == 0);                    // no glides: only quick steps after restores
		CHECK(e.steps > 0 && e.seeks > 0);
		CHECK(e.steps >= e.seeks - 1);          // every restore is played into
		if (speed < 1.0) {
			CHECK(shown > 20);
			CHECK(worstAhead <= 1.0 + speed + 1e-9); // a shown frame is at most a tick ahead of the playhead
			CHECK(worstBehind < 1.0);                // and never a whole tick behind it
		}
	}
	// Turning round resumes the forward glide.
	ScrubInput in; in.target = 1000.6; in.now = 1000.0; in.paused = true; in.draggingBack = false;
	CHECK(DecideScrub(in).action == ScrubAction::Play);
	in.draggingBack = true;
	CHECK(DecideScrub(in).action == ScrubAction::None);
	// Behind by whole ticks while dragging back: one quick step plays them in.
	in.now = 998.0; in.target = 1000.3;
	const ScrubDecision step = DecideScrub(in);
	CHECK(step.action == ScrubAction::Step && step.ticks == 2 && step.timescale == ScrubBackStepTimescale(in.frameSeconds));
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
	CHECK(JudgeSeek(74133, 73688, 73688, true, 2, 0, 2, true) == SeekWait::Landed);
	CHECK(JudgeSeek(74133, 73688, 73689, true, 2, 0, 2, true) == SeekWait::Landed);
	CHECK(JudgeSeek(74133, 73688, 73688, false, 2, 2, 2, true) == SeekWait::Waiting);
	// Mid-replay a backward seek still reads the old tick: wait.
	CHECK(JudgeSeek(30541, 30007, 30541, true, 1, 0, 1, true) == SeekWait::Waiting);
	// Measured: our seek to 30007 landed, then the bar's own seek took the world back to 30541 and
	// on to 32938 -- outside the range, so lost at once.
	CHECK(JudgeSeek(30541, 30007, 32938, true, 3, 0, 3, true) == SeekWait::Lost);
	CHECK(JudgeSeek(9765, 34446, 23173, false, 3, 3, 3, true) == SeekWait::Waiting); // inside, playing briefly
	CHECK(JudgeSeek(9765, 34446, 5000, true, 3, 0, 3, true) == SeekWait::Lost);
	// Playing away from the target after the pause blip window: something resumed it.
	CHECK(JudgeSeek(9765, 34446, 23173, false, kSeekPlayingLostFrames, kSeekPlayingLostFrames, kSeekPlayingLostFrames, true) == SeekWait::Lost);
	// Measured: seek 73688 from 74133 never ran (swallowed); the world sat on 74133, paused.
	CHECK(JudgeSeek(74133, 73688, 74133, true, kSeekStallFramesKeyframed - 1, 0, kSeekStallFramesKeyframed - 1, true) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 73688, 74133, true, kSeekStallFramesKeyframed, 0, kSeekStallFramesKeyframed, true) == SeekWait::Lost);
	// Without keyframes a long backward replay holds the old tick ~160 frames: keep waiting.
	CHECK(JudgeSeek(74133, 71000, 74133, true, 160, 0, 160, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 71000, 74133, true, kSeekStallFramesPlain, 0, kSeekStallFramesPlain, false) == SeekWait::Lost);
	// A forward replay into demo not yet played moves through the range: progress, not a stall.
	CHECK(JudgeSeek(1000, 40000, 12000, true, 600, 0, 600, true) == SeekWait::Waiting);
}

// 2026-10-19, a plain match demo: seek 64094 from 64103 restored Valve's full packet 61441 and read
// 63940 (the end of its first 2500-message chunk, playing) for a frame. Judged lost, every re-aim
// restored and read 63940 again; the scrub gave up 162 ticks short and resumed the demo.
void TestSeekReplayBelowItsStart() {
	CHECK(JudgeSeek(64103, 64094, 63940, false, 1, 1, 1, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(64103, 64094, 61441, true, 1, 0, 1, false) == SeekWait::Waiting); // just restored
	// The replay then lands on the target like any seek.
	CHECK(JudgeSeek(64103, 64094, 64094, true, 2, 0, 2, false) == SeekWait::Landed);
	// No full packet is farther back than Valve's 3840 ticks: below that, another seek moved it.
	CHECK(JudgeSeek(64103, 64094, 64094 - kSeekReplayTicksPlain - 1, true, 1, 0, 1, false) == SeekWait::Lost);
	// A keyframed copy restores at most 32 ticks back.
	CHECK(JudgeSeek(5000, 4900, 4880, false, 1, 1, 1, true) == SeekWait::Waiting);
	CHECK(JudgeSeek(64103, 64094, 63940, false, 1, 1, 1, true) == SeekWait::Lost);
	// A replay crosses a chunk a frame; one parked below the target is someone else's.
	CHECK(JudgeSeek(64103, 64094, 63940, true, kSeekReplayStallFrames - 1, 0, kSeekReplayStallFrames - 1, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(64103, 64094, 63940, true, kSeekReplayStallFrames, 0, kSeekReplayStallFrames, false) == SeekWait::Lost);
	// Above the range is still another seek's world.
	CHECK(JudgeSeek(64103, 64094, 64200, true, 1, 0, 1, false) == SeekWait::Lost);
	// Review of #64: a plain demo held the old tick ~160 frames before its replay showed. Playing and
	// parked are runs of frames, so the first replay tick after that hold is still ours.
	CHECK(JudgeSeek(74133, 71000, 70500, false, 170, 1, 0, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 71000, 72500, false, 170, 1, 0, false) == SeekWait::Waiting);
	CHECK(JudgeSeek(74133, 71000, 70500, true, 200, 0, kSeekReplayStallFrames, false) == SeekWait::Lost);
	CHECK(JudgeSeek(74133, 71000, 72500, false, 200, kSeekPlayingLostFrames, 0, false) == SeekWait::Lost);
}

} // namespace

// CS:S stops a seek on the demo's next packet, 4 to 10 ticks apart in a SourceTV demo. Told how late a
// seek may land, the controller aims that far short and still lands exactly.
void TestSeeksThatLandAPacketLate() {
	for (int gap : { 4, 7, 10 }) {
		Engine back; back.now = 1000.0; back.packetGap = gap;
		CHECK(back.Run(512.0, true) == ScrubAction::Done);
		CHECK(back.now == 512.0);
		Engine ahead; ahead.now = 1000.0; ahead.packetGap = gap;
		CHECK(ahead.Run(1500.0, true) == ScrubAction::Done);
		CHECK(ahead.now == 1500.0);
		// Dragged far ahead: the seek lands at or short of the playhead, and the world glides in.
		Engine drag; drag.now = 1000.0; drag.packetGap = gap;
		drag.Run(1500.3, false);
		CHECK(std::fabs(drag.now - 1500.3) <= kScrubArriveTicks + 0.01);
	}
	// A seek landing inside the window has landed; past it, another seek moved the world.
	CHECK(JudgeSeek(600, 2500, 2506, true, 2, 0, 2, true, 6) == SeekWait::Landed);
	CHECK(JudgeSeek(600, 2500, 2507, true, 2, 0, 2, true, 6) == SeekWait::Lost);
	CHECK(JudgeSeek(600, 2500, 2502, true, 2, 0, 2, true) == SeekWait::Lost); // CS2: one past at most
}

void TestBackStepNeverRunsPastAFrame() {
	// A slow frame must not carry the play-in step past its goal: at ~60 ms frames (heavy
	// Environment effects) 4x played 15 ticks a frame and a 2-tick step ended 6 ticks on.
	for (double dt : { 1.0 / 240.0, 1.0 / 144.0, 1.0 / 60.0, 0.06, 0.2 }) {
		const double s = ScrubBackStepTimescale(dt);
		CHECK(s <= kScrubBackStepTimescale + 1e-9);
		CHECK(s >= kScrubMinTimescale - 1e-9);
		CHECK(s * kScrubTickRate * dt <= kScrubBackStepMaxTicksPerFrame + 1e-9);
	}
	CHECK(ScrubBackStepTimescale(1.0 / 1000.0) == kScrubBackStepTimescale); // fast frames keep the quick step
	CHECK(ScrubBackStepTimescale(0.0) == kScrubBackStepTimescale);           // unknown frame time
	// The decision uses it.
	ScrubInput in; in.target = 1000.3; in.now = 998.0; in.paused = true; in.draggingBack = true; in.frameSeconds = 0.06;
	const ScrubDecision d = DecideScrub(in);
	CHECK(d.action == ScrubAction::Step && d.timescale * kScrubTickRate * 0.06 <= kScrubBackStepMaxTicksPerFrame + 1e-9);
}

void TestCacheServesTheRecordedTake() {
	ScrubCacheInput in; in.enabled = true; in.cached = true; in.now = 1000.0;
	in.target = 990.0;
	CHECK(ScrubServeFromCache(in));              // behind the world, cached: no restore
	in.target = 1004.0; in.recording = true;
	CHECK(!ScrubServeFromCache(in));             // forward while recording from the first key: play and record
	in.recording = false;
	CHECK(ScrubServeFromCache(in));              // forward inside a clicked-into shot: the take is the picture
	in.target = 999.5;
	CHECK(ScrubServeFromCache(in));
	in.released = true; in.recording = true;
	CHECK(ScrubServeFromCache(in));              // a click on a cached tick shows the cached frame
	in.cached = false;
	CHECK(!ScrubServeFromCache(in));             // off the cached span: the real world
	in.cached = true; in.enabled = false;
	CHECK(!ScrubServeFromCache(in));             // Cache off: today's behaviour
}

int main() {
	TestSeekJudgedEveryFrame();
	TestSeekReplayBelowItsStart();
	TestSlowDragGlidesToAFraction();
	TestDragFollowsWithoutOvershoot();
	TestSpeedFollowsTheGap();
	TestBackwardRestoresThenGlides();
	TestBackwardDragShowsOnlyPlayedFrames();
	TestSmallOvershootHolds();
	TestReleaseLandsOnAWholeTick();
	TestReleaseAcceptsOnePastAfterSettle();
	TestFarForwardSkipsThenGlides();
	TestMediumForwardDragSeeks();
	TestBusyWaits();
	TestNeverPausesAPausedDemo();
	TestBackStepNeverRunsPastAFrame();
	TestSeeksThatLandAPacketLate();
	TestCacheServesTheRecordedTake();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "demo scrub math tests passed\n";
	return 0;
}
