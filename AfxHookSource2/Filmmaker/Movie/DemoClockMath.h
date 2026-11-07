#pragma once

// THE FRACTIONAL DEMO CLOCK. CS2's demo tick (IDemoFile::GetDemoTick) only advances in whole ticks,
// and MirvTime's interpolation_amount is a CS:GO-era offset that reads 0 in CS2, so tick * interval
// holds still between ticks. At demo_timescale 0.08 a 64-tick demo then moves in about five steps a
// second while the players, which the engine interpolates, move smoothly: anything placed from that
// time (weather, storm flashes, studio light cues) froze and jumped next to them (issue 135).
//
// The client clock is the smooth source. Measured 2026-09-27 (Docs/features/filmmaker_demo_scrubbing.md):
// curtime * 64 reads the demo tick plus a whole-tick offset (the demo's server start tick) plus the
// interpolated fraction. It advances continuously while playing, holds the fraction while paused
// between two ticks, and reads a whole tick when paused on one.
//
// The time is that clock minus the offset, never the tick plus a fraction: measured 2026-11-07, at
// full speed the demo tick runs up to a frame ahead of the interpolated clock (it sometimes moves two
// ticks in one frame), so tick + (clock - tick - offset) jumped back and forth by up to a tick. The
// tick only teaches the offset:
//   * paused exactly on a whole tick it is exact;
//   * otherwise the first sample sets it (it can be a tick off while playing; the next pause on a
//     whole tick corrects it);
//   * a clock more than kRelearnTicks away from the learned offset is another demo, and relearns.
// Between those events the time is exactly as smooth as the client clock and never steps back while
// playing (consumers treat a backward step as a seek: Environment Vfx lights clear on one).
//
// Engine-free and header-only: Automation/tests/demo-clock-math-tests.cpp. MirvTime owns the one
// instance (CMirvTime::GetFractionalDemoTick).

#include <cmath>

namespace Filmmaker {

struct FractionalDemoClock {
	static constexpr double kWholeTickTolerance = 0.002; // paused this close to a whole tick is on it
	static constexpr double kBoundarySlack = 0.02;       // the clock may trail the tick by this much
	// A second of ticks. The tick may lead the clock by a frame's worth of ticks (17 at 16x on a
	// 60 fps frame), so only a jump this large is another demo rather than playback.
	static constexpr double kRelearnTicks = 64.0;

	bool learned = false;
	double offset = 0.0;

	// tick: the engine's whole demo tick (negative while a seek loads). clientTicks: curtime times the
	// tick rate. Returns the demo position in ticks, with its fraction.
	double Sample(int tick, double clientTicks, bool paused) {
		if (!std::isfinite(clientTicks)) return tick;
		if (tick < 0) return learned ? clientTicks - offset : 0.0;
		const double rel = clientTicks - tick;
		const double nearest = std::floor(rel + 0.5);
		if (paused && std::fabs(rel - nearest) < kWholeTickTolerance) {
			offset = nearest;
			learned = true;
		} else if (!learned || std::fabs(rel - offset) > kRelearnTicks) {
			offset = std::floor(rel + kBoundarySlack);
			learned = true;
		}
		return clientTicks - offset;
	}
	void Reset() { learned = false; offset = 0.0; }
};

} // namespace Filmmaker
