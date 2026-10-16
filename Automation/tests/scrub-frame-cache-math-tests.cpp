#include "../../AfxHookSource2/Filmmaker/Movie/ScrubFrameCacheMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::ScrubCache;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

FrameId Frame(double t, int take, uint64_t rev = 1, double takeStart = -1.0, bool recording = false) {
	FrameId id;
	id.valid = true;
	id.worldTime = t;
	id.cameraTick = t;
	id.played = true;
	id.take = take;
	id.takeStart = takeStart < 0.0 ? (double)take : takeStart; // later takes start later by default
	id.recording = recording;
	id.revision = rev;
	id.pose[0] = (float)t; // the camera moves with time
	return id;
}

// Plays a stretch of demo into the index the way the present path does: plan, then commit.
void Play(Index& ix, double from, double to, double step, int take, uint64_t rev = 1, double takeStart = -1.0,
	bool recording = false) {
	for (double t = from; t <= to + 1e-9; t += step) {
		const FrameId id = Frame(t, take, rev, takeStart, recording);
		const Index::Store s = ix.PlanStore(id, t);
		if (s.slot >= 0) ix.Commit(s.slot, id);
	}
}

void TestOnlyPlayedConsistentFramesAreCached() {
	FrameId id = Frame(100.0, 1);
	CHECK(Cacheable(id));
	id.played = false;                 // a restore nobody has played into: bind-pose players
	CHECK(!Cacheable(id));
	id = Frame(100.0, 1);
	id.cameraTick = 103.0;             // camera ahead of the world: not a picture of tick 100
	CHECK(!Cacheable(id));
	id.cameraTick = -1.0;              // camera not path-driven: nothing to validate a pose against
	CHECK(!Cacheable(id));
	id = Frame(100.0, 0);
	CHECK(!Cacheable(id));             // no take yet
	Index ix; ix.Reset(8);
	id = Frame(100.0, 1); id.played = false;
	CHECK(ix.PlanStore(id, 100.0).slot < 0);
}

void TestTheRecordedTakeStays() {
	Index ix; ix.Reset(128);
	// Take 1 records the shot from its first key (5756).
	Play(ix, 5756.0, 5826.0, 1.0, 1, 1, 5748.0, true);
	CHECK(ix.Count() == 71);
	// A click at 5790 restores to 5782 and plays in: take 2 started later, so it does not overwrite.
	Play(ix, 5784.0, 5790.0, 1.0, 2, 1, 5782.0, false);
	CHECK(ix.At(ix.Find(5788.0, 1)).take == 1);
	CHECK(ix.Find(5820.0, 1) >= 0);                    // nothing after the click was dropped
	// ...but it fills a tick nothing was cached for.
	Index jx; jx.Reset(16);
	Play(jx, 5756.0, 5760.0, 1.0, 1, 1, 5748.0, true);
	Play(jx, 5790.0, 5792.0, 1.0, 2, 1, 5782.0, false);
	CHECK(jx.At(jx.Find(5791.0, 1)).take == 2);
	// Playing from the first key again re-records the shot, whatever tick its take started on.
	Play(ix, 5756.0, 5826.0, 1.0, 3, 1, 5756.0, true);
	CHECK(ix.At(ix.Find(5788.0, 1)).take == 3);
	CHECK(ix.At(ix.Find(5800.0, 1)).take == 3);
	// An older take never comes back over a newer one of the same start.
	const Index::Store s = ix.PlanStore(Frame(5800.0, 2, 1, 5782.0, false), 5800.0);
	CHECK(s.slot < 0);
}

void TestWithinATakeTheFrameNearestTheTickStays() {
	Index ix; ix.Reset(8);
	Play(ix, 99.7, 100.4, 0.1, 1); // a slow glide: several frames round to tick 100
	CHECK(ix.Count() == 1);
	CHECK(std::fabs(ix.At(ix.Find(100.0, 1)).worldTime - 100.0) < 1e-6);
}

void TestFindBridgesSmallGapsOfOneTake() {
	Index ix; ix.Reset(64);
	Play(ix, 100.0, 130.0, 3.0, 1); // a heavy scene: a frame every 3 ticks
	const int a = ix.Find(101.4, 1);
	CHECK(a >= 0 && ix.At(a).tick == 100);
	const int b = ix.Find(101.6, 1);
	CHECK(b >= 0 && ix.At(b).tick == 103);
	CHECK(ix.Find(131.5, 1) < 0);         // past the last frame by more than a tick
	CHECK(ix.Find(130.8, 1) >= 0);        // within a tick of it
	// A gap between two different takes is not bridged.
	Index jx; jx.Reset(16);
	Play(jx, 100.0, 100.0, 1.0, 1);
	Play(jx, 104.0, 104.0, 1.0, 2);
	CHECK(jx.Find(102.0, 1) < 0);
	// Nor a gap wider than kServeBridgeTicks.
	Index kx; kx.Reset(16);
	Play(kx, 100.0, 100.0, 1.0, 1);
	Play(kx, 100.0 + kServeBridgeTicks + 1.0, 100.0 + kServeBridgeTicks + 1.0, 1.0, 1);
	CHECK(kx.Find(100.0 + kServeBridgeTicks / 2 + 0.5, 1) < 0);
}

void TestRevisionInvalidates() {
	Index ix; ix.Reset(16);
	Play(ix, 100.0, 105.0, 1.0, 1, 7);
	CHECK(ix.Find(102.0, 7) >= 0);
	CHECK(ix.Find(102.0, 8) < 0);        // a config / scene / path edit: not shown
	CHECK(ix.DropOtherRevisions(8) == 6);
	CHECK(ix.Count() == 0);
	// A frame of the new revision replaces a stale one of the same tick, whatever the take.
	Play(ix, 100.0, 100.0, 1.0, 3, 7);
	const Index::Store s = ix.PlanStore(Frame(100.0, 3, 8), 100.0);
	CHECK(s.slot >= 0 && s.replaced);
}

void TestFullCacheThinsBeforeEvicting() {
	Index ix; ix.Reset(10);
	Play(ix, 100.0, 109.0, 1.0, 1);
	CHECK(ix.Count() == 10);
	// Full: an inner frame goes, not the start of the shot, and the whole span stays covered.
	Index::Store s = ix.PlanStore(Frame(110.0, 1), 110.0);
	CHECK(s.slot >= 0 && s.evicted && s.evictedTick != 100 && s.evictedTick != 109);
	ix.Commit(s.slot, Frame(110.0, 1));
	for (double t = 100.0; t <= 110.0; t += 0.25) CHECK(ix.Find(t, 1) >= 0);
	// A 70-tick pass into 62 slots (the measured case) is covered end to end.
	Index jx; jx.Reset(62);
	Play(jx, 5756.0, 5826.0, 1.0, 1);
	CHECK(jx.Count() == 62);
	for (double t = 5756.0; t <= 5826.0; t += 0.5) CHECK(jx.Find(t, 1) >= 0);
	// Far longer than the budget can bridge: the frames farthest from the user go.
	Index kx; kx.Reset(10);
	Play(kx, 100.0, 200.0, 1.0, 1);
	CHECK(kx.Count() == 10);
	CHECK(kx.Find(199.0, 1) >= 0);
	CHECK(kx.Find(100.0, 1) < 0);
	// A frame farther from the user than everything cached is not worth an eviction.
	s = ix.PlanStore(Frame(50.0, 1), 105.0);
	CHECK(s.slot < 0);
	CHECK(ix.Count() == 10);
}

TakeTracker::Event Observe(TakeTracker& tk, double now, bool seekPending = false) {
	return tk.Observe((int)std::floor(now), now, seekPending);
}

void TestTakeTracker() {
	TakeTracker tk;
	TakeTracker::Event ev = Observe(tk, 500.0);
	CHECK(tk.take == 1 && tk.played && !ev.newTake); // paused here: it was played to
	// Playing forward stays in the take.
	for (double t = 500.0; t < 520.0; t += 1.1) { ev = Observe(tk, t); CHECK(!ev.newTake); }
	CHECK(tk.take == 1 && tk.played);
	// Our seek goes out: nothing is played until it lands and is played into.
	tk.NoteSeek();
	CHECK(!tk.played);
	ev = Observe(tk, 519.8, true);                   // mid-replay the tick still reads the old value
	CHECK(!ev.newTake && !tk.played);
	ev = Observe(tk, 510.0);                         // landed: a new take starts at 510
	CHECK(ev.newTake && std::fabs(ev.startTick - 510.0) < 1e-9 && tk.take == 2 && !tk.played);
	CHECK(tk.start == 510.0);
	CHECK(Recording(tk.start, 510.0) && Recording(tk.start, 515.0)); // began at or before the first key
	CHECK(!Recording(tk.start, 505.0));                             // began inside the shot
	ev = Observe(tk, 511.0);
	CHECK(!tk.played);                               // one tick in: still the restore's look
	ev = Observe(tk, 512.0);
	CHECK(tk.played && tk.take == 2);                // the 2-tick play-in is done
	// Someone else's seek (CS2's own demo bar): detected from the jump.
	ev = Observe(tk, 400.0);
	CHECK(ev.newTake && tk.take == 3 && !tk.played);
	ev = Observe(tk, 400.0 + kJumpTicks + 50.0);     // a far forward seek also restores
	CHECK(ev.newTake && tk.take == 4);
	// A seek that lands on the very tick it left still starts a take.
	tk.NoteSeek();
	Observe(tk, 474.0 + kJumpTicks, true);
	ev = Observe(tk, 474.0 + kJumpTicks);
	CHECK(ev.newTake && tk.take == 5);
}

void TestPlaybackClockWrapIsNotATake() {
	// Before the clock offset is learned, the fraction of a playing demo wraps back at a tick
	// boundary while the whole tick holds (measured: a new take every ~15 ticks of 1x playback).
	TakeTracker tk;
	tk.Observe(684, 684.0, false);
	TakeTracker::Event ev = tk.Observe(684, 684.97, false);
	CHECK(!ev.newTake);
	ev = tk.Observe(684, 684.01, false); // fraction wrapped, tick did not move
	CHECK(!ev.newTake && tk.take == 1 && tk.played);
	ev = tk.Observe(685, 685.40, false);
	CHECK(!ev.newTake && tk.take == 1);
}

void TestPoseTolerance() {
	float a[kPoseValues] = { 100, 200, 300, 10, 179.99f, 0, 90 };
	float b[kPoseValues] = { 100.01f, 200, 300, 10, -179.995f, 0, 90 };
	CHECK(PoseNear(a, b));              // yaw wraps at 180
	b[0] = 100.5f;
	CHECK(!PoseNear(a, b));
	b[0] = 100; b[6] = 91;
	CHECK(!PoseNear(a, b));             // FOV is part of the picture
}

void TestBudget() {
	// 1564x826 RGBA8 is about 5.2 MB; the default budget holds about 74 frames.
	const int n = CapacityForBudget((uint64_t)kDefaultBudgetMb << 20, 1564, 826, 4);
	CHECK(n >= 70 && n <= 80);
	CHECK(CapacityForBudget(1ull << 40, 16, 16, 4) == kMaxFrames);
	CHECK(CapacityForBudget(1ull << 30, 0, 826, 4) == 0);
}

} // namespace

int main() {
	TestOnlyPlayedConsistentFramesAreCached();
	TestTheRecordedTakeStays();
	TestWithinATakeTheFrameNearestTheTickStays();
	TestFindBridgesSmallGapsOfOneTake();
	TestRevisionInvalidates();
	TestFullCacheThinsBeforeEvicting();
	TestTakeTracker();
	TestPlaybackClockWrapIsNotATake();
	TestPoseTolerance();
	TestBudget();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "scrub frame cache math tests passed\n";
	return 0;
}
