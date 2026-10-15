#pragma once

// When does the next easter-egg hint float across the screen?
//
// THE BRIEF. Rare, but not so rare nobody ever sees one: a lucky day can bring one or two, an
// unlucky stretch can go three days without any, and nobody gets them several times a day.
//
// THE MODEL. Hints arrive as a Poisson process over ACTIVE PLAY TIME (seconds the game is actually
// running with the hook), not wall time -- someone who opens CS2 for ten minutes a week should not
// be ambushed by a backlog the moment it starts. The gap to the next hint is drawn from an
// exponential with mean Params::meanGapSeconds, then clamped so one hint can never chain straight
// into the next. Two hard caps sit on top of the random draw:
//   * Params::maxPerDay hints per local calendar day, and
//   * Params::minWallGapSeconds of real time between two hints, so a long session cannot spend
//     both of the day's hints back to back.
// Measured by the test's simulation (3 h of play every evening): ~0.7 hints a day, two on about
// one day in eight, and three-day droughts do happen -- the shape that was asked for. A 3 h wall
// gap made two-hint days impossible for a 3 h session, which is why it is 2 h.
//
// Engine-free and header-only so Automation/tests/easter-egg-hint-math-tests.cpp can pin it.

#include <cmath>
#include <cstdint>

namespace Filmmaker {
namespace EasterEggHint {

struct Params {
	double meanGapSeconds = 4.5 * 3600.0;
	double minGapSeconds = 45.0 * 60.0;     // active time; floor on the random draw
	double maxGapSeconds = 12.0 * 3600.0;   // active time; ceiling on the random draw
	double minWallGapSeconds = 2.0 * 3600.0;
	int maxPerDay = 2;
};

// Everything that must survive a restart. Persisted verbatim.
struct State {
	double activeSeconds = 0.0;   // total active play time ever counted
	double nextAtActive = -1.0;   // activeSeconds threshold of the next hint; < 0 = not scheduled
	double lastShownUnix = 0.0;   // wall clock of the last hint, 0 = never
	int dayKey = 0;               // local yyyymmdd that shownToday belongs to
	int shownToday = 0;
	int recent[4] = { -1, -1, -1, -1 }; // last hint indices, newest first (avoid repeats)
};

enum class Verdict {
	Show,
	NotScheduled,  // first run: schedule, then wait
	NotYet,        // active time has not reached the threshold
	DailyCap,      // threshold reached but today's quota is spent; it fires tomorrow
	WallGap,       // threshold reached but the previous hint was too recent in real time
};

// Exponential draw from a uniform u in [0,1), clamped to [minGap, maxGap].
inline double SampleGapSeconds(double u01, const Params& p) {
	if (!(u01 >= 0.0)) u01 = 0.0;
	if (u01 >= 1.0) u01 = 0.999999;
	double gap = -std::log(1.0 - u01) * p.meanGapSeconds;
	if (gap < p.minGapSeconds) gap = p.minGapSeconds;
	if (gap > p.maxGapSeconds) gap = p.maxGapSeconds;
	return gap;
}

// A new calendar day resets the daily quota. Call before Evaluate with the current local day.
inline void RollDay(State& s, int todayKey) {
	if (s.dayKey != todayKey) { s.dayKey = todayKey; s.shownToday = 0; }
}

inline void Schedule(State& s, double u01, const Params& p) {
	s.nextAtActive = s.activeSeconds + SampleGapSeconds(u01, p);
}

inline Verdict Evaluate(const State& s, double nowUnix, const Params& p) {
	if (s.nextAtActive < 0.0) return Verdict::NotScheduled;
	if (s.activeSeconds < s.nextAtActive) return Verdict::NotYet;
	if (s.shownToday >= p.maxPerDay) return Verdict::DailyCap;
	if (s.lastShownUnix > 0.0 && nowUnix - s.lastShownUnix < p.minWallGapSeconds) return Verdict::WallGap;
	return Verdict::Show;
}

// Picks a hint index in [0,count) that is not among the recent ones when that is possible.
// `u01` chooses among the remaining candidates.
inline int PickHint(const State& s, int count, double u01) {
	if (count <= 0) return -1;
	int candidates[256];
	int n = 0;
	for (int i = 0; i < count && i < 256; ++i) {
		bool recent = false;
		for (int r = 0; r < 4; ++r) if (s.recent[r] == i) recent = true;
		if (!recent) candidates[n++] = i;
	}
	if (u01 < 0.0) u01 = 0.0;
	if (u01 >= 1.0) u01 = 0.999999;
	if (n == 0) return (int)(u01 * count);
	return candidates[(int)(u01 * n)];
}

inline void RecordShown(State& s, int hintIndex, double nowUnix, double u01Next, const Params& p) {
	s.shownToday++;
	s.lastShownUnix = nowUnix;
	for (int r = 3; r > 0; --r) s.recent[r] = s.recent[r - 1];
	s.recent[0] = hintIndex;
	Schedule(s, u01Next, p);
}

} // namespace EasterEggHint
} // namespace Filmmaker
