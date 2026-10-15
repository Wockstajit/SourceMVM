#pragma once

#include <cmath>

namespace Filmmaker {

// Deciding whether a camera sample is CONTINUOUS with the previous one, or a cut the shake
// modules must restart from.
//
// Why this is not a one-line speed test. The view-setup demo clock and the camera position are
// interpolated separately, so their per-frame deltas disagree: measured 2026-09-19, 4.6% of
// frames carried a nonzero demo dt under 4 ms, some as short as 0.4 ms, while the camera moved
// the ordinary distance a running player covers in a frame. Dividing one by the other
// manufactures a speed no player can reach -- 3.9 units over 0.9 ms reads as 4622 u/s -- and the
// old speed-threshold test classified those frames as seeks. Every one of them hard-reset the
// tracker, which drops Weapon Bob's phase and pose: the gun stopped and snapped back mid-stride,
// twice in 30 seconds of ordinary playback.
//
// So the test is on DISTANCE, against the distance a player could have covered in the sample's
// own interval, floored at one demo tick. The floor is the whole point: a sample can claim to
// span 0.4 ms, but the camera it reports cannot have moved less than one tick's worth of world.
enum class TrackerBreak {
	None,      // continuous: integrate normally
	Gap,       // backwards past the jitter tolerance, or a long stall (loading, alt-tab)
	Teleport,  // moved further than any player could: a seek or a spectator switch
};

// The demo clock steps BACKWARDS by a few milliseconds routinely (sub-tick interpolation;
// measured 2026-08-28: 68 negative steps of 0.8-7.9 ms in ~20 s). Treating those as seeks reset
// the tracker several times a second and cut every shot effect short, so only a step past this
// restarts it.
inline constexpr double kTrackerBackwardsTolerance = -0.05; // seconds, ~3 ticks at 64 Hz
inline constexpr double kTrackerMaxSampleGap = 0.5;         // seconds
// Above any real player speed, including boosts and surfs, with room to spare.
inline constexpr double kTrackerTeleportSpeed = 4000.0;     // units/sec
// The shortest interval a sample is believed to span. One tick at 64 Hz: the sub-millisecond
// steps the clock reports are interpolation noise, not evidence that time barely passed.
inline constexpr double kTrackerMinSampleInterval = 1.0 / 64;

// The distance beyond which a move cannot be a player running, for a sample spanning `dt`.
inline double TrackerTeleportDistance(double dt) {
	const double interval = dt > kTrackerMinSampleInterval ? dt : kTrackerMinSampleInterval;
	return kTrackerTeleportSpeed * interval;
}

// `dt` is demo seconds since the previous accepted sample; `dx`/`dy` the planar move since it.
inline TrackerBreak ClassifyTrackerSample(double dt, double dx, double dy) {
	if (dt < kTrackerBackwardsTolerance || dt > kTrackerMaxSampleGap) return TrackerBreak::Gap;
	const double distance = std::sqrt(dx * dx + dy * dy);
	if (distance > TrackerTeleportDistance(dt)) return TrackerBreak::Teleport;
	return TrackerBreak::None;
}

} // namespace Filmmaker
