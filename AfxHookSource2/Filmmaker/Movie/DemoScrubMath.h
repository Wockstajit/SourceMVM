#pragma once

// Live demo scrubbing policy: how the demo WORLD follows a timeline drag. Engine-free and
// header-only so the policy is unit-tested (Automation/tests/demo-scrub-math-tests.cpp); the
// driver that issues the engine commands is DemoScrub.cpp. Contract and measurements:
// Docs/features/filmmaker_demo_scrubbing.md.
//
// WHAT THE ENGINE ALLOWS (measured 2026-09-27):
//   * a PAUSED demo can rest BETWEEN two ticks: play at a low demo_timescale, demo_pause, and the
//     whole world is interpolated to that fraction and stays there (camera 0.52 of the way from
//     tick 77177 to 77178 after 0.51 ticks of 0.02x playback). Demo time = tick +
//     interpolation_amount. So a slow drag GLIDES instead of snapping tick to tick;
//   * a backward seek restores the newest full packet at or before the target (one ~50-80 ms
//     frame on a keyframed copy, see Demo/DemoKeyframes.h) and lands on a whole tick, sometimes
//     one past it;
//   * a FORWARD demo_gototick sent while PAUSED never runs, so the driver sends
//     `demo_resume;demo_gototick N` and pauses once it sees the demo playing on N.
//     demo_pause is a toggle and must not share that string: it runs while still paused and
//     starts playback. Backward, a plain demo_gototick from a paused demo holds;
//   * demo_step_tick N plays to the N-th next whole tick and pauses exactly there, from a whole
//     or a fractional position;
//   * demo_pause is a TOGGLE, and commands take effect on the next frame.
//
// THE POLICY -- a controller, run every frame, that steers the world toward the playhead:
//   * AHEAD by up to kScrubPlayAheadTicks: PLAY toward it at a speed proportional to the gap
//     (arrives in about kScrubCatchupSeconds, slowing as it gets close) and pause the frame it
//     would reach it -- at whatever fraction of a tick that is;
//   * BEHIND, or far ahead: pause, seek to the whole tick before the playhead, then glide in;
//   * RELEASED: land on the nearest WHOLE tick. A release seeks kScrubSettleLeadTicks short and
//     plays in, so the final frame has been played into rather than freshly restored.

#include <cmath>

namespace Filmmaker {

// Forward gaps up to a quarter second are PLAYED; anything farther seeks. It was 128 (2 s), but
// gliding that far means playing at ~10x, where frames run slow, the "pause the frame you would
// arrive" check fires early, and the world stuttered pause/play every two frames for half a second
// (measured 2026-10-09). On a keyframed copy a seek lands in one ~80 ms frame.
constexpr double kScrubPlayAheadTicks = 16.0;
constexpr int kScrubSettleLeadTicks = 8;       // a release seeks this far short and plays in
constexpr double kScrubCatchupSeconds = 0.12;
constexpr double kScrubMinTimescale = 0.02;    // slow enough to stop inside a tick
constexpr double kScrubMaxTimescale = 16.0;
constexpr double kScrubTickRate = 64.0;
constexpr double kScrubArriveTicks = 0.02;     // this close to a dragged playhead is "there"
// While dragging, a world AHEAD of the playhead by less than this holds instead of restoring a
// keyframe. A restore costs a 50-500 ms frame and lands on a whole tick (sometimes one past), while
// a fraction of a tick ahead is invisible -- measured: the drag's first event sat 0.17 behind a
// world paused at 1400.27, and a tighter tolerance spent a restore on it.
constexpr double kScrubBehindToleranceTicks = 1.0;
// A released world one whole tick past its goal after the settle seek is accepted (seeks land one
// past now and then); a second restore would land the same way.
constexpr double kScrubReleaseOvershootTicks = 1.0;
// A backward drag restores this far short of the playhead and plays it in with one quick step,
// while the preview holds its last settled frame (DemoScrub_HoldPreview): the frame shown next has
// been simulated, not just restored.
constexpr int kScrubBackLeadTicks = 2;
constexpr double kScrubBackStepTimescale = 4.0;

enum class ScrubAction {
	None,   // wait: a command is in flight, or the world is where it should be
	Play,   // demo_timescale <timescale> (+ demo_resume when paused)
	Pause,  // demo_pause (only ever decided while playing)
	Step,   // demo_timescale <timescale>; demo_step_tick <ticks>
	Seek,   // demo_gototick <seekTick>, from a paused demo
	Done    // released and on the final tick: restore speed / play state
};

struct ScrubInput {
	double target = 0.0;       // demo time (ticks) the playhead points at
	double now = 0.0;          // demo time (ticks) of the world, tick + interpolation
	bool paused = true;
	bool busy = false;         // a step, seek or pause has not taken effect yet
	bool released = false;     // the drag ended: land exactly on a whole tick
	bool settled = false;      // released, and the settle seek went out
	bool draggingBack = false; // the drag's last move went backward
	bool restoredNotPlayed = false; // a backward restore landed and nothing has been played since
	double frameSeconds = 1.0 / 60.0;
	double timescale = 1.0;    // demo_timescale while playing
};

struct ScrubDecision {
	ScrubAction action = ScrubAction::None;
	double timescale = 1.0; // Play, Step
	int ticks = 0;          // Step
	int seekTick = 0;       // Seek
};

// Where the world should end up: the playhead itself while dragging, the nearest whole tick on
// release.
inline double ScrubGoal(double target, bool released) {
	if (target < 0.0) target = 0.0;
	return released ? std::floor(target + 0.5) : target;
}

// Playback speed that closes `gapTicks` in about kScrubCatchupSeconds.
inline double ScrubPlaySpeed(double gapTicks) {
	double s = gapTicks / kScrubTickRate / kScrubCatchupSeconds;
	if (s < kScrubMinTimescale) s = kScrubMinTimescale;
	if (s > kScrubMaxTimescale) s = kScrubMaxTimescale;
	return s;
}

inline ScrubDecision ScrubSeekTo(int tick) {
	ScrubDecision d;
	d.action = ScrubAction::Seek;
	d.seekTick = tick < 0 ? 0 : tick;
	return d;
}

// WAITING FOR A SEEK. The scrub's own seeks are not the only ones: CS2's demo bar seeks on its own
// when it is clicked or released (GotoTick in huddemocontroller.js), and so do other tools. If one
// of those runs while ours is in flight, ours never "lands", and waiting for it froze every drag
// until a 1200-frame safety cap (measured 2026-10-09: 20 s of ignored drags, twice in a minute).
// So a seek is judged every frame and given up the moment it is plainly not going to land; the
// controller then aims again from wherever the world actually is.
//   * Mid-replay the tick reads the OLD value (or values on the way, forward), so anything inside
//     [from, expect] (+/-1) may still be ours. Outside it, another seek moved the world: Lost.
//   * Still playing away from the target after a few frames (a pause can blip false mid-seek):
//     another command resumed it: Lost.
//   * Not moved at all: a seek swallowed by a later one to the same tick. A keyframed copy restores
//     in one or two frames; without keyframes a long backward replay holds the old tick for up to
//     ~160 frames (ragdolls.dem, 3000 ticks back), so the wait depends on which file is playing.
enum class SeekWait { Waiting, Landed, Lost };

constexpr int kSeekPlayingLostFrames = 12;
constexpr int kSeekStallFramesKeyframed = 20;
constexpr int kSeekStallFramesPlain = 240;

inline SeekWait JudgeSeek(int fromTick, int expectTick, int tick, bool paused, int busyFrames, bool keyframed) {
	const bool there = tick >= expectTick - 1 && tick <= expectTick + 1;
	if (there) return paused ? SeekWait::Landed : SeekWait::Waiting; // the driver re-pauses once
	const int lo = (fromTick < expectTick ? fromTick : expectTick) - 1;
	const int hi = (fromTick > expectTick ? fromTick : expectTick) + 1;
	if (tick < lo || tick > hi) return SeekWait::Lost;
	if (!paused && busyFrames >= kSeekPlayingLostFrames) return SeekWait::Lost;
	// Moving through the range is progress (a forward replay into demo not yet played); only a
	// world still on the tick it started from counts as stalled.
	const bool unmoved = tick >= fromTick - 1 && tick <= fromTick + 1;
	if (unmoved && busyFrames >= (keyframed ? kSeekStallFramesKeyframed : kSeekStallFramesPlain)) return SeekWait::Lost;
	return SeekWait::Waiting;
}

inline ScrubDecision DecideScrub(const ScrubInput& in) {
	ScrubDecision d;
	if (in.busy) return d;
	const double goal = ScrubGoal(in.target, in.released);
	const double gap = goal - in.now;

	if (in.released) {
		const double past = in.settled ? kScrubReleaseOvershootTicks : 0.0;
		if (gap <= 1e-3 && gap >= -past - 1e-3) {
			if (!in.paused) { d.action = ScrubAction::Pause; return d; }
			d.action = ScrubAction::Done;
			return d;
		}
		if (!in.paused) { d.action = ScrubAction::Pause; return d; }
		if (gap > 0.0 && gap <= kScrubPlayAheadTicks) {
			// Step to the goal's whole tick: from a fractional position the step still lands on
			// the next whole ticks, so ceil(gap) of them end exactly on the goal.
			d.action = ScrubAction::Step;
			d.ticks = (int)std::ceil(gap - 1e-6);
			d.timescale = ScrubPlaySpeed(gap);
			return d;
		}
		return ScrubSeekTo((int)goal - kScrubSettleLeadTicks);
	}

	// Dragging.
	if (in.draggingBack && in.restoredNotPlayed) {
		// A backward restore has landed and nothing has been played since. Play the lead in FIRST,
		// whatever the playhead did meanwhile: the next frame the preview shows must be simulated.
		// A fast drag outruns the restores (~10 a second), so by the time one lands the playhead is
		// already further back and the play-in below never came up -- measured 2026-10-16 at 40
		// ticks a second: 25 restores, 0 steps, and un-animated players on screen.
		if (!in.paused) { d.action = ScrubAction::Pause; return d; }
		d.action = ScrubAction::Step;
		d.ticks = kScrubBackLeadTicks;
		d.timescale = kScrubBackStepTimescale;
		return d;
	}
	if (gap < -kScrubBehindToleranceTicks || gap > kScrubPlayAheadTicks) {
		if (!in.paused) { d.action = ScrubAction::Pause; return d; }
		// Dragging BACKWARD: restore kScrubBackLeadTicks short of the playhead and play them in
		// with one quick step (below), while the preview holds its last settled frame
		// (DemoScrub_HoldPreview). Two things this avoids, both measured 2026-10-16:
		//   * gliding into the playhead after every restore showed as a saw-tooth -- jump back,
		//     play a tick FORWARD -- on a slow backward drag (seek 5812 target 5813, seek 5806 ...);
		//   * showing the restore itself showed an un-simulated world: a restore runs no
		//     animation, so players stood in bind pose or were missing until a tick had played.
		// A seek landing one past still leaves at least a tick to play in.
		if (in.draggingBack && gap < 0.0)
			return ScrubSeekTo((int)std::floor(goal) - kScrubBackLeadTicks);
		// Otherwise restore the whole tick before the playhead (a seek may land one past), then glide in.
		return ScrubSeekTo((int)std::floor(goal) - 1);
	}
	if (gap <= kScrubArriveTicks || (in.draggingBack && gap < 1.0)) {
		// There, a hair past, or -- dragging backward -- within the tick: hold. Never glide forward
		// while the playhead moves back.
		if (!in.paused) d.action = ScrubAction::Pause;
		return d;
	}
	if (in.draggingBack) {
		// Behind the playhead after a backward restore: play the lead in, whole ticks, in one step,
		// so the frame the preview shows next has been simulated.
		if (!in.paused) { d.action = ScrubAction::Pause; return d; }
		d.action = ScrubAction::Step;
		d.ticks = (int)std::floor(gap + 1e-9);
		d.timescale = kScrubBackStepTimescale;
		return d;
	}
	const double speed = ScrubPlaySpeed(gap);
	if (in.paused) {
		d.action = ScrubAction::Play;
		d.timescale = speed;
		return d;
	}
	// Pause the frame the world would reach the playhead: the pause lands a frame later.
	const double advance = in.timescale * kScrubTickRate * in.frameSeconds;
	if (gap <= 1.5 * advance) {
		d.action = ScrubAction::Pause;
		return d;
	}
	if (std::fabs(speed - in.timescale) > 0.15 * in.timescale) {
		d.action = ScrubAction::Play;
		d.timescale = speed;
	}
	return d;
}

} // namespace Filmmaker
