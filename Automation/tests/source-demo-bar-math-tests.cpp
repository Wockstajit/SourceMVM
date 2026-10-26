// DemoBarMath, the CS:S demo bar's rules: CS2's speed dial mapping and zones, the scrubber's tick
// mapping, -5s/+5s, how seeks are sent, LOOP, and stepping between events.

#include "../../AfxHookSource/Filmmaker/Ui/DemoBarMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SourceFilmmaker::Ui::DemoBarMath;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void TestSpeedDial() {
	CHECK(Near(SpeedFor(0), kMinSpeed));
	CHECK(Near(SpeedFor(1), kMaxSpeed));
	CHECK(SpeedFor(0.5) == 1.0);
	CHECK(SpeedFor(0.515) == 1.0);           // inside the snap
	CHECK(SpeedFor(0.53) > 1.0);             // outside it
	CHECK(Near(SpeedFor(-1), kMinSpeed) && Near(SpeedFor(2), kMaxSpeed));
	for (double v = 0.0; v <= 1.0; v += 0.05)
		if (std::fabs(v - 0.5) >= kSnap) CHECK(Near(SliderFor(SpeedFor(v)), v, 1e-9));
	CHECK(SliderFor(0) == 0.5 && SliderFor(-3) == 0.5);
	// Each zone's speed puts the handle close to the zone's centre, so labels and handle agree.
	for (int i = 0; i < kZoneCount; ++i) {
		const double centre = (i + 0.5) / kZoneCount;
		CHECK(std::fabs(SliderFor(kZoneSpeeds[i]) - centre) < 0.04);
		CHECK(ZoneAt(SliderFor(kZoneSpeeds[i])) == i);
	}
	CHECK(ZoneAt(1.0) == kZoneCount - 1 && ZoneAt(-1) == 0);
}

static void TestSpeedText() {
	char text[16];
	FormatSpeed(1, text, sizeof(text)); CHECK(0 == std::strcmp(text, "1x"));
	FormatSpeed(4, text, sizeof(text)); CHECK(0 == std::strcmp(text, "4x"));
	FormatSpeed(1.5, text, sizeof(text)); CHECK(0 == std::strcmp(text, "1.50x"));
	FormatSpeed(12.34, text, sizeof(text)); CHECK(0 == std::strcmp(text, "12.3x"));
	FormatSpeed(0.25, text, sizeof(text)); CHECK(0 == std::strcmp(text, "0.25x"));
	FormatSpeed(0.05, text, sizeof(text)); CHECK(0 == std::strcmp(text, "0.050x"));
	CHECK(!SpeedChanged(1.005, 1.0) && SpeedChanged(1.02, 1.0) && SpeedChanged(1, 0));
}

static void TestDialWidth() {
	CHECK(DialWidth(4000, 4000) == kDialMaxW);       // capped
	CHECK(DialWidth(1000, 4000) == 260 || DialWidth(1000, 4000) == kDialMinW); // share, then floor
	CHECK(DialWidth(1000, 4000) == kDialMinW);
	CHECK(DialWidth(1500, 4000) == 390);            // 26% of the bar
	CHECK(DialWidth(4000, 400) == 352);             // the free room minus 48
	CHECK(DialWidth(4000, 100) == kDialMinW);
}

static void TestScrubber() {
	CHECK(TickAtX(100, 100, 500, 0, 4410) == 0);
	CHECK(TickAtX(500, 100, 500, 0, 4410) == 4410);
	CHECK(TickAtX(300, 100, 500, 0, 4410) == 2205);
	CHECK(TickAtX(50, 100, 500, 0, 4410) == 0 && TickAtX(900, 100, 500, 0, 4410) == 4410);
	CHECK(TickAtX(300, 100, 100, 7, 4410) == 7);    // no width
	for (int tick : { 0, 1, 490, 2205, 4409, 4410 })
		CHECK(std::abs(TickAtX(XAtTick(tick, 0, 4410, 0, 4410), 0, 4410, 0, 4410) - tick) <= 1);
	char text[24];
	FormatTime(66.15, text, sizeof(text)); CHECK(0 == std::strcmp(text, "1:06.15"));
	FormatTime(0, text, sizeof(text)); CHECK(0 == std::strcmp(text, "0:00.00"));
	FormatTime(3723.4, text, sizeof(text)); CHECK(0 == std::strcmp(text, "1:02:03.40"));
	FormatTime(-3, text, sizeof(text)); CHECK(0 == std::strcmp(text, "0:00.00"));
}

static void TestSeeking() {
	CHECK(TicksForSeconds(5, 0.015) == 335);        // CS:S 66.67 tick: 5 x 67
	CHECK(TicksForSeconds(5, 1.0 / 64) == 320);
	CHECK(TicksForSeconds(5, 0) == 0);
	Seek s = PlanSeek(1000, 1335, 4410);
	CHECK(s.tick == 1335 && !s.restart);
	s = PlanSeek(1000, 665, 4410);
	CHECK(s.tick == 665 && s.restart);              // backward: restart, then forward
	s = PlanSeek(100, -235, 4410);
	CHECK(s.tick == 0 && s.restart);
	s = PlanSeek(4300, 4635, 4410);
	CHECK(s.tick == 4410 && !s.restart);
	CHECK(!PlanSeek(1000, 1000, 4410).restart);
}

static void TestLoop() {
	LoopRegion r = SeedLoop(1000, 4410);
	CHECK(r.in == 1000 && r.out == 1320 && r.Valid());
	r = SeedLoop(4300, 4410);
	CHECK(r.out == 4410 && r.in == 4090);           // kept inside the demo
	r = SeedLoop(-5, 100);
	CHECK(r.in == 0 && r.out == 100);
	r = SeedLoop(1000, 4410);
	CHECK(!LoopWraps(r, 1319, true) && LoopWraps(r, 1320, true) && !LoopWraps(r, 1400, false));
	CHECK(!LoopWraps(LoopRegion(), 50, true));
}

static void TestEvents() {
	CHECK(EventLeadTicks(1.0 / 64) == 40);
	CHECK(EventLeadTicks(0.015) == 42);
	const std::vector<int> kills = { 490, 4250, 4310 };
	CHECK(NextTickAfter(kills, 0, 10) == 490);
	CHECK(NextTickAfter(kills, 485, 10) == 4250);   // just before one: move on
	CHECK(NextTickAfter(kills, 4310, 10) == -1);
	CHECK(PreviousTickBefore(kills, 4300, 10) == 4250);
	CHECK(PreviousTickBefore(kills, 4255, 10) == 490);
	CHECK(PreviousTickBefore(kills, 400, 10) == -1);
}

int main() {
	TestSpeedDial();
	TestSpeedText();
	TestDialWidth();
	TestScrubber();
	TestSeeking();
	TestLoop();
	TestEvents();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDemoBarMathTests passed\n");
	return 0;
}
