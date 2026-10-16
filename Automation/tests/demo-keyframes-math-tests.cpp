// DemoKeyframesMath.h: which ticks of a keyframed copy get the dense grid.
#include "../../AfxHookSource2/Filmmaker/Demo/DemoKeyframesMath.h"

#include <cstdio>

using namespace Filmmaker::KeyframeWindows;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

void TestOneShot() {
	// The reported shot: 5 markers within 3.2 s make ONE window, padded 4 s either side.
	const Plan p = PlanFor({ 5756, 5827, 5857, 5931, 5961 });
	CHECK(p.windows.size() == 1);
	CHECK(p.windows[0].first == 5756 - kPadTicks && p.windows[0].last == 5961 + kPadTicks);
	CHECK(p.step == 2);
	CHECK(Covers(p.windows, { 5756, 5961 }));
	CHECK(!Covers(p.windows, { 9000 }));
}

void TestClusters() {
	// 8 s apart still merge; further apart makes a second window, so the gap stays on the base grid.
	CHECK(PlanFor({ 1000, 1000 + kMergeGapTicks }).windows.size() == 1);
	const Plan two = PlanFor({ 1000, 1000 + kMergeGapTicks + 1 });
	CHECK(two.windows.size() == 2);
	CHECK(two.windows[1].first == 1000 + kMergeGapTicks + 1 - kPadTicks);
	// Unsorted and duplicated input plans the same.
	const Plan a = PlanFor({ 9000, 100, 9000, 150 });
	CHECK(a.windows.size() == 2 && a.windows[0].first == 0 && a.windows[1].last == 9000 + kPadTicks);
	CHECK(PlanFor({}).windows.empty());
	CHECK(Covers({}, {})); // no markers: nothing to cover
}

void TestCap() {
	// Many spread-out shots: the step widens so the dense keyframes stay within budget.
	std::vector<int> ticks;
	for (int i = 0; i < 40; ++i) ticks.push_back(i * 2000);
	const Plan p = PlanFor(ticks);
	long long total = 0;
	for (const Window& w : p.windows) total += w.last - w.first + 1;
	CHECK(p.step > 2 && p.step <= kMaxStep);
	CHECK(p.step == kMaxStep || total / p.step <= kMaxDenseTicks / kFirstStep);
	CHECK(32 % p.step == 0); // stays on the base grid's ticks
}

void TestFormat() {
	const std::vector<Window> w = { { 0, 600 }, { 5500, 6217 } };
	CHECK(Format(w) == "0-600,5500-6217");
	const std::vector<Window> back = Parse(Format(w));
	CHECK(back.size() == 2 && back[1].first == 5500 && back[1].last == 6217);
	CHECK(Parse("").empty());
	CHECK(Parse("garbage").empty());
	CHECK(Parse("10-5").empty()); // reversed range is refused
}
} // namespace

int main() {
	TestOneShot();
	TestClusters();
	TestCap();
	TestFormat();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("demo-keyframes-math-tests: all passed\n");
	return 0;
}
