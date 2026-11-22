// Behavior tests for Lens/LensRainMath.h: drops on the lens that land, cling, merge and run with the
// camera's motion.
#include "../../AfxHookSource2/Filmmaker/Lens/LensRainMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::LensRain;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

constexpr float kAspect = 16.0f / 9.0f;

Sim Rained(const Params& p, float seconds) {
	Sim s(42);
	Motion still;
	for (float t = 0; t < seconds; t += 1.0f / 60) s.Step(1.0f / 60, kAspect, still, p);
	return s;
}

float MeanX(const Sim& s) { double x = 0; for (const Drop& d : s.Drops()) x += d.x; return s.Drops().empty() ? 0 : (float)(x / s.Drops().size()); }

void RainLandsAndClings() {
	Params p;
	const Sim s = Rained(p, 3);
	CHECK(!s.Drops().empty());
	for (const Drop& d : s.Drops()) CHECK(d.x >= -0.1f && d.x <= kAspect + 0.1f && d.y >= -0.1f && d.y <= 1.1f && d.r > 0);
	// Looking straight down at the floor, nothing runs: gravity points into the lens.
	Params grippy = p; grippy.amount = 60;
	Sim down(7);
	Motion floor; floor.gravity[0] = 0; floor.gravity[1] = 0;
	for (int i = 0; i < 180; ++i) down.Step(1.0f / 60, kAspect, floor, grippy);
	for (const Drop& d : down.Drops()) CHECK(d.vx == 0 && d.vy == 0);
	// No rain lands no drops.
	Params dry; dry.amount = 0;
	CHECK(Rained(dry, 2).Drops().empty());
}

void BigDropsRunDownSmallOnesHold() {
	Params p;
	Motion level; // looking level: gravity straight down the frame
	Drop big, small;
	big.r = kCriticalRadius * 2.0f; small.r = kCriticalRadius * 0.3f;
	float a[2];
	Sim::Push(big, kAspect, level, p, a);
	CHECK(a[1] > 0.5f && std::fabs(a[0]) < 1e-4f);
	Sim::Push(small, kAspect, level, p, a);
	CHECK(std::fabs(a[1]) < 1e-4f);
	// Slide 0 holds everything.
	Params stuck = p; stuck.slide = 0;
	Sim::Push(big, kAspect, level, stuck, a);
	CHECK(a[0] == 0 && a[1] == 0);
}

void RunningSidewaysBlowsTheDropsAcross() {
	Params p; p.amount = 60; p.trails = 0;
	Motion strafe; strafe.right = 400; // strafing right fast
	Sim s(3);
	Motion still;
	for (int i = 0; i < 120; ++i) s.Step(1.0f / 60, kAspect, still, p);
	const float before = MeanX(s);
	Params none = p; none.amount = 0;
	for (int i = 0; i < 60; ++i) s.Step(1.0f / 60, kAspect, strafe, none);
	CHECK(s.Drops().empty() || MeanX(s) < before - 0.05f);
}

void RunningForwardSendsThemToTheEdges() {
	Params p;
	Motion fwd; fwd.forward = 500;
	Drop left, right, top;
	left.x = kAspect * 0.2f; left.y = 0.5f; left.r = 0.008f;
	right.x = kAspect * 0.8f; right.y = 0.5f; right.r = 0.008f;
	top.x = kAspect * 0.5f; top.y = 0.1f; top.r = 0.008f;
	float a[2];
	Motion noGravity = fwd; noGravity.gravity[1] = 0;
	Sim::Push(left, kAspect, noGravity, p, a); CHECK(a[0] < 0);
	Sim::Push(right, kAspect, noGravity, p, a); CHECK(a[0] > 0);
	Sim::Push(top, kAspect, noGravity, p, a); CHECK(a[1] < 0);
	// More drops hit while running into the rain.
	Params impact; impact.impact = 2; impact.trails = 0; impact.slide = 0;
	Sim standing(9), running(9);
	Motion still, run; run.forward = 500;
	for (int i = 0; i < 60; ++i) { standing.Step(1.0f / 60, kAspect, still, impact); running.Step(1.0f / 60, kAspect, run, impact); }
	CHECK(running.Drops().size() > standing.Drops().size());
}

void TouchingDropsMergeKeepingTheirWater() {
	Params p; p.amount = 100; p.size = 3; p.slide = 0; p.trails = 0;
	Sim s(11);
	Motion still;
	for (int i = 0; i < 600; ++i) s.Step(1.0f / 60, kAspect, still, p);
	const auto& d = s.Drops();
	for (size_t i = 0; i < d.size(); ++i)
		for (size_t j = i + 1; j < d.size(); ++j) {
			if ((d[i].kind == Kind::Splash) != (d[j].kind == Kind::Splash)) continue;
			const float dx = d[i].x - d[j].x, dy = d[i].y - d[j].y, reach = (d[i].r + d[j].r) * 0.85f;
			CHECK(dx * dx + dy * dy >= reach * reach);
		}
	CHECK((int)d.size() <= kMaxDrops);
}

void RunoffFollowsTheGlass() {
	Params storm;
	storm.amount = 85;
	storm.size = 1.5f;
	storm.trails = 0.8f;
	Sim level(42), rolled(42), dryTrails(42);
	Motion down, left;
	left.gravity[0] = -1;
	left.gravity[1] = 0;
	Params noTrails = storm;
	noTrails.trails = 0;
	for (int i = 0; i < 180; ++i) {
		level.Step(1.0f / 60, kAspect, down, storm);
		rolled.Step(1.0f / 60, kAspect, left, storm);
		dryTrails.Step(1.0f / 60, kAspect, down, noTrails);
	}
	int splashes = 0, strips = 0, sideways = 0;
	for (const Drop& d : level.Drops()) {
		if (d.kind == Kind::Splash) {
			++splashes;
			CHECK(VisualRadius(d) > d.r * 5);
		}
		if (d.trailY > 0.018f) ++strips;
		CHECK(d.trailX == 0 && d.trailY >= 0 && d.trailY <= 0.180001f);
		CHECK(d.kind == Kind::Rivulet || d.trailY == 0);
	}
	for (const Drop& d : rolled.Drops()) {
		if (d.trailX < -0.018f) ++sideways;
		CHECK(d.trailY == 0 && d.trailX <= 0);
	}
	for (const Drop& d : dryTrails.Drops()) CHECK(d.trailX == 0 && d.trailY == 0);
	CHECK(splashes > 0 && splashes < static_cast<int>(level.Drops().size() / 4));
	CHECK(strips > 0 && strips < static_cast<int>(level.Drops().size() / 4));
	CHECK(sideways > 0);
}

void SameStepsSameDrops() {
	Params p;
	Motion m; m.right = 120; m.forward = 200; m.yawRate = 40;
	Sim a(5), b(5);
	for (int i = 0; i < 240; ++i) { a.Step(1.0f / 60, kAspect, m, p); b.Step(1.0f / 60, kAspect, m, p); }
	CHECK(a.Drops().size() == b.Drops().size());
	for (size_t i = 0; i < a.Drops().size() && i < b.Drops().size(); ++i)
		CHECK(a.Drops()[i].x == b.Drops()[i].x && a.Drops()[i].y == b.Drops()[i].y && a.Drops()[i].r == b.Drops()[i].r);
	// A paused demo (dt 0) moves nothing.
	const auto before = a.Drops();
	a.Step(0, kAspect, m, p);
	CHECK(a.Drops().size() == before.size());
}

void DropsSplatClingThenDrain() {
	// Lands as a splat: about a fifth of its size, nine tenths within 50 ms, all of it soon after.
	Drop d; d.r = kCriticalRadius * 2; d.life = 4;
	float size, alpha;
	d.age = 0; Lifecycle(d, size, alpha); CHECK(size < 0.3f);
	d.age = 0.05f; Lifecycle(d, size, alpha); CHECK(size > 0.85f && size < 0.95f && alpha > 0.99f);
	d.age = 1.0f; Lifecycle(d, size, alpha); CHECK(size > 0.99f && alpha > 0.99f);
	// Drains over the last third: fainter and smaller near the end.
	d.age = 3.9f; Lifecycle(d, size, alpha); CHECK(size < 0.55f && alpha < 0.2f);
	// Clings first: gravity barely pulls a big drop that just landed, fully two thirds in.
	Params p; Motion level; float a0[2], a1[2];
	d.age = 0; Sim::Push(d, kAspect, level, p, a0);
	d.age = 3; Sim::Push(d, kAspect, level, p, a1);
	CHECK(a0[1] < a1[1] * 0.05f && a1[1] > 0.5f);
	// Drained drops are gone: after a shower stops, the lens dries.
	Params rain; rain.slide = 0;
	Sim s(13); Motion still;
	for (int i = 0; i < 120; ++i) s.Step(1.0f / 60, kAspect, still, rain);
	CHECK(!s.Drops().empty());
	Params dry = rain; dry.amount = 0;
	for (int i = 0; i < 60 * 8; ++i) s.Step(1.0f / 60, kAspect, still, dry);
	for (const Drop& e : s.Drops()) CHECK(e.life == 0); // only drops with no set life could remain
}

void MotionFromTwoPoses() {
	const double o0[3] = { 0, 0, 0 }, a0[3] = { 0, 0, 0 };
	// Moving +x while looking along +x is forward; +y (Source's left) is moving left.
	const double o1[3] = { 10, 0, 0 };
	Motion m = MotionFrom(o0, a0, o1, a0, 0.05);
	CHECK(std::fabs(m.forward - 200) < 1e-3 && std::fabs(m.right) < 1e-3);
	const double o2[3] = { 0, 10, 0 };
	m = MotionFrom(o0, a0, o2, a0, 0.05);
	CHECK(std::fabs(m.right + 200) < 1e-3);
	// Level: gravity is straight down the frame. Looking at the floor (pitch 90): none on the glass.
	CHECK(std::fabs(m.gravity[1] - 1) < 1e-5 && std::fabs(m.gravity[0]) < 1e-5);
	const double down[3] = { 89.9, 0, 0 };
	m = MotionFrom(o0, down, o0, down, 0.05);
	CHECK(std::fabs(m.gravity[1]) < 0.01);
	// Rolled 90 degrees right, down on the glass points to the frame's left... or right: it is sideways.
	const double rolled[3] = { 0, 0, 90 };
	m = MotionFrom(o0, rolled, o0, rolled, 0.05);
	CHECK(std::fabs(std::fabs(m.gravity[0]) - 1) < 1e-5 && std::fabs(m.gravity[1]) < 1e-5);
	// A cut (a jump across the map) is no motion.
	const double far[3] = { 4000, 0, 0 };
	m = MotionFrom(o0, a0, far, a0, 1.0 / 60);
	CHECK(m.forward == 0 && m.right == 0 && m.up == 0);
	// Turning left at 90 degrees per second.
	const double turned[3] = { 0, 4.5, 0 };
	m = MotionFrom(o0, a0, o0, turned, 0.05);
	CHECK(std::fabs(m.yawRate - 90) < 1e-3);
}

} // namespace

int main() {
	RainLandsAndClings();
	BigDropsRunDownSmallOnesHold();
	RunningSidewaysBlowsTheDropsAcross();
	RunningForwardSendsThemToTheEdges();
	TouchingDropsMergeKeepingTheirWater();
	RunoffFollowsTheGlass();
	SameStepsSameDrops();
	DropsSplatClingThenDrain();
	MotionFromTwoPoses();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "LensRainMathTests: all passed\n";
	return 0;
}
