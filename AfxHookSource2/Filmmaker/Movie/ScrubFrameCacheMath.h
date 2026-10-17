#pragma once

// Scrub frame cache: which preview frames are kept, which one a drag or a click shows, and which
// ones are replaced. Engine-free and header-only so it is unit-tested
// (Automation/tests/scrub-frame-cache-math-tests.cpp). The GPU copies live in the viewport scaler
// (RenderSystemDX11Hooks.cpp, AfxViewportScaler), the main-thread side in ScrubFrameCache.cpp, and
// the serve-or-restore decision in DemoScrubMath.h. Contract: Docs/features/filmmaker_demo_scrubbing.md
// ("Scrub frame cache").
//
// WHY TAKES. A demo replays deterministically, but the client does not: ragdolls, death animations
// and particles are simulated on the client and come out differently every time the world is
// restored and played again. So a cached frame is only true for the TAKE it was played in -- one
// stretch of real forward play since the last restore. The cache holds THE RECORDED TAKE of a shot:
//   * only played frames are kept: a restore that has not been played into yet (no animation,
//     bind-pose players) is never cached;
//   * a take replaces another's frame of the same tick only if it started no later. Playing (or
//     dragging) forward from the shot's first key re-records the whole shot; a click in the middle
//     restores the world there, and that take only fills ticks nothing is cached for. So dragging
//     back and forth and clicking anywhere in the shot all show the same take, as the user asked
//     (2026-10-16), and it only changes when the shot is played through again from its start;
//   * restores drop nothing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker {
namespace ScrubCache {

constexpr int kPoseValues = 7;                // x y z pitch yaw roll fov
constexpr double kPosePositionTolerance = 0.05; // world units
constexpr double kPoseAngleTolerance = 0.02;    // degrees
constexpr double kPoseFovTolerance = 0.02;      // degrees
// The camera must have been evaluated at the world's own time for a frame to be a true picture of
// that tick (camera timeline / graph scrubs fly the path; see ScrubFrameCache_CameraTick).
constexpr double kCameraWorldToleranceTicks = 0.02;
// A frame is served for a playhead within this of its own time...
constexpr double kServeNearTicks = 1.0;
// ...or anywhere between two frames of the same take that are at most this far apart. A heavy scene
// at 16 fps plays 2-4 ticks per frame, and a gap that small must not fall back to a restore.
constexpr double kServeBridgeTicks = 8.0;
// A restore counts as played into once the world has advanced this far from where it landed by
// playing (DemoScrubMath.h kScrubBackLeadTicks: the backward play-in is 2 ticks).
constexpr double kPlayedInTicks = 2.0;
// A one-frame move by more than this is a seek, not playback (a 16x glide moves ~17 ticks a frame
// at 60 fps only in theory; DemoScrubMath.h caps glides at 16 ticks ahead).
constexpr double kJumpTicks = 24.0;
constexpr int kDefaultBudgetMb = 384;          // about 74 frames at 1564x826 RGBA8 (5.2 MB each)
constexpr int kMaxFrames = 512;

inline bool PoseNear(const float a[kPoseValues], const float b[kPoseValues]) {
	for (int i = 0; i < 3; ++i)
		if (std::fabs(a[i] - b[i]) > kPosePositionTolerance) return false;
	for (int i = 3; i < 6; ++i) {
		double d = std::fmod(std::fabs((double)a[i] - (double)b[i]), 360.0);
		if (d > 180.0) d = 360.0 - d;
		if (d > kPoseAngleTolerance) return false;
	}
	return std::fabs(a[6] - b[6]) <= kPoseFovTolerance;
}

// What one rendered frame shows, carried from the main thread to the present that shows it.
struct FrameId {
	bool valid = false;      // a demo is open and every field below was filled for this frame
	double worldTime = 0.0;  // fractional demo time of the world (tick + interpolation)
	double cameraTick = -1.0;// time the camera path was evaluated at; < 0 = camera not path-driven
	bool played = false;     // the world has been played into since its last restore
	int take = 0;            // TakeTracker::take
	double takeStart = 0.0;  // the tick that take started at (TakeTracker::start)
	bool recording = false;  // that take began at or before the shot's first key (Recording)
	uint64_t revision = 0;   // everything other than time that changes the picture
	float pose[kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };     // camera the frame renders with
	// The camera path's own pose at cameraTick. Equal to `pose` for a scrub; path playback shows a
	// low-passed pose, so a cached frame is validated against the path, not against what it showed.
	float pathPose[kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };
};

// True when the frame is a picture of its own tick: played, and the camera at the world's time.
inline bool Cacheable(const FrameId& id) {
	return id.valid && id.played && id.take > 0 && id.cameraTick >= 0.0
		&& std::fabs(id.worldTime - id.cameraTick) <= kCameraWorldToleranceTicks;
}

inline int SlotTick(double worldTime) { return (int)std::floor(worldTime + 0.5); }

// Follows the world from frame to frame and numbers its takes. Observe() once per main-thread
// frame; NoteSeek() when a seek goes out (the world may still read the old tick mid-replay, and
// nothing from then until the landing has been played into).
class TakeTracker {
public:
	struct Event {
		bool newTake = false;
		double startTick = 0.0; // where the new take begins
	};

	int take = 0;
	bool played = false;
	double start = 0.0; // the tick the current take started at

	void Reset() { *this = TakeTracker(); }

	void NoteSeek() { m_seekPending = true; played = false; }

	// `tick` is the demo's whole tick, `now` its fractional time. Jumps are judged on the WHOLE
	// tick: until the scrub has learned the clock offset, the fraction of a playing demo wraps back
	// at tick boundaries (measured 2026-10-16: a take every ~15 ticks of plain 1x playback). The whole
	// tick only ever goes back on a seek.
	Event Observe(int tick, double now, bool seekPending) {
		Event ev;
		if (!m_have) {
			// Unknown history: a world that was simply paused here has been played into.
			m_have = true;
			take = 1;
			m_start = tick - (int)kPlayedInTicks;
			start = m_start;
			m_prev = tick;
			played = !seekPending;
			m_seekPending = seekPending;
			return ev;
		}
		const bool jumped = tick < m_prev || tick > m_prev + (int)kJumpTicks;
		if (jumped || (m_seekPending && !seekPending)) {
			// A restore landed (ours, or anyone's seek): a new simulation starts here.
			++take;
			m_start = tick;
			start = tick;
			played = false;
			ev.newTake = true;
			ev.startTick = now;
		}
		m_seekPending = seekPending;
		if (seekPending) played = false;
		else if (!played && tick - m_start >= (int)kPlayedInTicks) played = true;
		m_prev = tick;
		return ev;
	}

private:
	bool m_have = false;
	bool m_seekPending = false;
	int m_start = 0;
	int m_prev = 0;
};

// Bookkeeping for the GPU slots: which tick, take and camera each slot holds. The textures
// themselves live with the viewport scaler; this decides where a frame goes and which one to show.
class Index {
public:
	struct Entry {
		bool used = false;
		int tick = 0;
		double worldTime = 0.0;
		int take = 0;
		double takeStart = 0.0;
		uint64_t revision = 0;
		float pose[kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };
		float pathPose[kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };
	};
	struct Store {
		int slot = -1;         // -1: keep what is there (the cached frame is closer, or its take started earlier)
		bool replaced = false; // the tick had a frame (another take, or a frame farther from it)
		bool evicted = false;  // another tick's frame was dropped to make room
		int evictedTick = 0;
	};

	void Reset(int capacity) {
		m_entries.assign(capacity < 0 ? 0 : (capacity > kMaxFrames ? kMaxFrames : capacity), Entry());
	}
	int Capacity() const { return (int)m_entries.size(); }
	int Count() const {
		int n = 0;
		for (const Entry& e : m_entries) n += e.used ? 1 : 0;
		return n;
	}
	const Entry& At(int slot) const { return m_entries[slot]; }

	// Where a cacheable frame goes. `nearTick` is where the user is (the playhead). When the cache is
	// full it THINS first: the frame whose two same-take neighbours sit closest together goes, as long
	// as the gap it leaves can still be bridged (kServeBridgeTicks), so a shot longer than the budget
	// stays covered end to end. Measured 2026-10-16: a 70-tick pass into 62 slots evicted its first
	// ticks, the drag back fell off the bottom of the span, and the restore that followed started a
	// new take that dropped the other 60 frames. Only when no gap can be closed is the frame farthest
	// from the user evicted.
	Store PlanStore(const FrameId& id, double nearTick) const {
		Store s = PlanStoreSimple(id, nearTick);
		if (!s.evicted) return s;
		const int thin = ThinSlot(id.revision);
		if (thin >= 0) {
			s.slot = thin;
			s.evictedTick = m_entries[thin].tick;
		}
		return s;
	}

private:
	// The frame whose removal leaves the smallest bridgeable gap between two frames of one take, or -1.
	int ThinSlot(uint64_t revision) const {
		std::vector<int> order;
		order.reserve(m_entries.size());
		for (int i = 0; i < (int)m_entries.size(); ++i)
			if (m_entries[i].used && m_entries[i].revision == revision) order.push_back(i);
		std::sort(order.begin(), order.end(), [this](int a, int b) { return m_entries[a].worldTime < m_entries[b].worldTime; });
		int best = -1;
		double bestGap = kServeBridgeTicks + 1e-9;
		for (size_t k = 1; k + 1 < order.size(); ++k) {
			const Entry& p = m_entries[order[k - 1]];
			const Entry& e = m_entries[order[k]];
			const Entry& n = m_entries[order[k + 1]];
			if (p.take != e.take || n.take != e.take) continue;
			const double gap = n.worldTime - p.worldTime;
			if (gap < bestGap) { bestGap = gap; best = order[k]; }
		}
		return best;
	}

	Store PlanStoreSimple(const FrameId& id, double nearTick) const {
		Store s;
		if (!Cacheable(id) || m_entries.empty()) return s;
		const int tick = SlotTick(id.worldTime);
		int freeSlot = -1, farSlot = -1;
		double farDist = -1.0;
		for (int i = 0; i < (int)m_entries.size(); ++i) {
			const Entry& e = m_entries[i];
			if (!e.used) { if (freeSlot < 0) freeSlot = i; continue; }
			if (e.tick == tick) {
				const bool stale = e.revision != id.revision;
				const bool closer = e.take == id.take
					&& std::fabs(id.worldTime - tick) < std::fabs(e.worldTime - tick) - 1e-6;
				// Another take replaces this frame only if it records the shot (began at its first
				// key) or started no later: a pass from the shot's start re-records it, a click in
				// the middle does not.
				const bool rerecorded = e.take != id.take && (id.recording || id.takeStart <= e.takeStart + 1e-6);
				if (stale || closer || rerecorded) { s.slot = i; s.replaced = true; }
				return s;
			}
			const double d = std::fabs(e.worldTime - nearTick);
			if (d > farDist) { farDist = d; farSlot = i; }
		}
		if (freeSlot >= 0) { s.slot = freeSlot; return s; }
		// Full: never evict a frame nearer the user than the new one.
		if (farSlot >= 0 && farDist > std::fabs(id.worldTime - nearTick)) {
			s.slot = farSlot;
			s.evicted = true;
			s.evictedTick = m_entries[farSlot].tick;
		}
		return s;
	}

public:
	void Commit(int slot, const FrameId& id) {
		if (slot < 0 || slot >= (int)m_entries.size()) return;
		Entry& e = m_entries[slot];
		e.used = true;
		e.tick = SlotTick(id.worldTime);
		e.worldTime = id.worldTime;
		e.take = id.take;
		e.takeStart = id.takeStart;
		e.revision = id.revision;
		for (int i = 0; i < kPoseValues; ++i) { e.pose[i] = id.pose[i]; e.pathPose[i] = id.pathPose[i]; }
	}

	// The frame to show for the playhead, or -1. Only frames of the current revision count. The
	// nearest frame within kServeNearTicks, or the nearer of two same-take neighbours that bracket
	// the playhead no more than kServeBridgeTicks apart.
	int Find(double tick, uint64_t revision) const {
		int below = -1, above = -1;
		for (int i = 0; i < (int)m_entries.size(); ++i) {
			const Entry& e = m_entries[i];
			if (!e.used || e.revision != revision) continue;
			if (e.worldTime <= tick) { if (below < 0 || e.worldTime > m_entries[below].worldTime) below = i; }
			else if (above < 0 || e.worldTime < m_entries[above].worldTime) above = i;
		}
		const double dBelow = below >= 0 ? tick - m_entries[below].worldTime : 1e9;
		const double dAbove = above >= 0 ? m_entries[above].worldTime - tick : 1e9;
		const int nearest = dBelow <= dAbove ? below : above;
		if (nearest < 0) return -1;
		if ((nearest == below ? dBelow : dAbove) <= kServeNearTicks) return nearest;
		if (below >= 0 && above >= 0 && m_entries[below].take == m_entries[above].take
			&& dBelow + dAbove <= kServeBridgeTicks)
			return nearest;
		return -1;
	}

	// Frames of any other revision. Returns how many were dropped.
	int DropOtherRevisions(uint64_t revision) {
		int n = 0;
		for (Entry& e : m_entries)
			if (e.used && e.revision != revision) { e.used = false; ++n; }
		return n;
	}
	int DropSlot(int slot) {
		if (slot < 0 || slot >= (int)m_entries.size() || !m_entries[slot].used) return 0;
		m_entries[slot].used = false;
		return 1;
	}
	int Clear() {
		int n = Count();
		for (Entry& e : m_entries) e.used = false;
		return n;
	}

private:
	std::vector<Entry> m_entries;
};

// A take that started at or before the shot's first key (+1 for a seek landing one past) is
// RECORDING it: a forward drag plays the world and its frames re-record the shot. Any other take is
// shown the cached shot instead.
inline bool Recording(double takeStart, double shotStart) {
	return takeStart <= shotStart + 1.0 + 1e-6;
}

// How many frames fit in the budget. 0 when a frame is empty.
inline int CapacityForBudget(uint64_t budgetBytes, uint32_t width, uint32_t height, uint32_t bytesPerPixel) {
	const uint64_t frame = (uint64_t)width * height * bytesPerPixel;
	if (frame == 0) return 0;
	const uint64_t n = budgetBytes / frame;
	return n > (uint64_t)kMaxFrames ? kMaxFrames : (int)n;
}

} // namespace ScrubCache
} // namespace Filmmaker
