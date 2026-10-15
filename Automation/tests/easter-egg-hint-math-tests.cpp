#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggHintMath.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

using namespace Filmmaker::EasterEggHint;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// Plays `days` days of `hoursPerDay` play in one evening session per day (18:00 onwards), the
// way the hook does: a 1 s active tick, Evaluate every tick, Show -> RecordShown. Returns hints
// shown per day in `perDay`.
void Simulate(int days, double hoursPerDay, uint64_t seed, int* perDay) {
	std::mt19937_64 rng(seed);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	Params p;
	State s;
	for (int d = 0; d < days; ++d) {
		perDay[d] = 0;
		RollDay(s, 20260101 + d);
		const double dayStart = 1.7e9 + d * 86400.0 + 18 * 3600.0;
		const int ticks = (int)(hoursPerDay * 3600.0);
		for (int t = 0; t < ticks; t += 10) {
			s.activeSeconds += 10.0;
			const double now = dayStart + t;
			switch (Evaluate(s, now, p)) {
			case Verdict::NotScheduled: Schedule(s, u(rng), p); break;
			case Verdict::Show: RecordShown(s, PickHint(s, 12, u(rng)), now, u(rng), p); perDay[d]++; break;
			default: break;
			}
		}
	}
}

void TestGapDraw() {
	Params p;
	CHECK(SampleGapSeconds(0.0, p) == p.minGapSeconds);
	CHECK(SampleGapSeconds(0.999999, p) == p.maxGapSeconds);
	// The median of an exponential is mean*ln2; inside the clamps it must land there.
	const double median = SampleGapSeconds(0.5, p);
	CHECK(std::fabs(median - p.meanGapSeconds * std::log(2.0)) < 1.0);
	CHECK(SampleGapSeconds(std::nan(""), p) == p.minGapSeconds);
}

void TestFirstRunWaitsInsteadOfFiring() {
	Params p;
	State s;
	CHECK(Evaluate(s, 1.7e9, p) == Verdict::NotScheduled);
	Schedule(s, 0.0, p);
	CHECK(Evaluate(s, 1.7e9, p) == Verdict::NotYet);
	s.activeSeconds += p.minGapSeconds;
	CHECK(Evaluate(s, 1.7e9, p) == Verdict::Show);
}

void TestCapsHold() {
	Params p;
	State s;
	RollDay(s, 20260101);
	s.nextAtActive = 0.0;
	s.shownToday = p.maxPerDay;
	CHECK(Evaluate(s, 1.7e9, p) == Verdict::DailyCap);
	RollDay(s, 20260102);
	CHECK(s.shownToday == 0);
	s.lastShownUnix = 1.7e9;
	CHECK(Evaluate(s, 1.7e9 + 3600.0, p) == Verdict::WallGap);
	CHECK(Evaluate(s, 1.7e9 + p.minWallGapSeconds, p) == Verdict::Show);
}

void TestPickAvoidsRecent() {
	State s;
	s.recent[0] = 0; s.recent[1] = 1; s.recent[2] = 2; s.recent[3] = 3;
	for (int i = 0; i < 100; ++i) {
		const int pick = PickHint(s, 6, i / 100.0);
		CHECK(pick == 4 || pick == 5);
	}
	// Fewer hints than the recent window: still returns something in range.
	const int pick = PickHint(s, 3, 0.5);
	CHECK(pick >= 0 && pick < 3);
	CHECK(PickHint(s, 0, 0.5) == -1);
}

// The shape that was asked for, measured over many simulated players.
void TestRarityShape() {
	const int days = 3000;
	static int perDay[3000];
	Simulate(days, 3.0, 7, perDay);
	int total = 0, two = 0, over = 0, dryRuns = 0, threeDayDry = 0;
	for (int d = 0; d < days; ++d) {
		total += perDay[d];
		if (perDay[d] == 2) two++;
		if (perDay[d] > 2) over++;
		dryRuns = perDay[d] == 0 ? dryRuns + 1 : 0;
		if (dryRuns == 3) threeDayDry++;
	}
	const double rate = (double)total / days;
	std::cout << "3h/day: " << rate << " hints/day, two-hint days " << (100.0 * two / days)
		<< "%, 3-day dry spells " << threeDayDry << "\n";
	CHECK(over == 0);                 // never more than two a day
	CHECK(rate > 0.5 && rate < 1.1);  // rare, but most days have a chance
	CHECK(two > 0);                   // a lucky day with two exists
	CHECK(threeDayDry > 0);           // and so does an unlucky three-day drought

	// A marathon session still cannot exceed the cap or bunch hints together.
	static int marathon[200];
	Simulate(200, 14.0, 11, marathon);
	for (int d = 0; d < 200; ++d) CHECK(marathon[d] <= 2);
}

} // namespace

int main() {
	TestGapDraw();
	TestFirstRunWaitsInsteadOfFiring();
	TestCapsHold();
	TestPickAvoidsRecent();
	TestRarityShape();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "EasterEggHintMathTests passed\n";
	return 0;
}
