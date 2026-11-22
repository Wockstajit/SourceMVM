// Behavior tests for lens drops driven by scene rain and camera motion.
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

void RainControlsDensityAndSize() {
	Params p;
	const Sim moderate = Rained(p, 3);
	CHECK(!moderate.Drops().empty() && moderate.Drops().size() < 24);
	Params light = p;
	light.intensity = 0.25f;
	CHECK(Rained(light, 3).Drops().size() < moderate.Drops().size());
	light.intensity = 0;
	CHECK(Rained(light, 3).Drops().empty());
	p.amount = 0;
	CHECK(Rained(p, 3).Drops().empty());

	Drop d;
	d.r = 0.003f;
	CHECK(std::fabs(VisualRadius(d, 0.8f) - 2 * VisualRadius(d, 0.4f)) < 1e-6f);
	d.kind = Kind::Splash;
	CHECK(VisualRadius(d, 0.8f) > 4 * VisualRadius(Drop{0, 0, d.r}, 0.8f));
}

void HeavierRainRunsFaster() {
	Params gentle;
	gentle.intensity = 0.2f;
	Params heavy = gentle;
	heavy.intensity = 1;
	Drop large, small;
	large.r = 0.005f; large.age = 0.2f;
	small.r = 0.0013f; small.age = large.age;
	Motion down;
	float light[2], hard[2], quiet[2];
	Sim::Push(large, kAspect, down, gentle, light);
	Sim::Push(large, kAspect, down, heavy, hard);
	Sim::Push(small, kAspect, down, heavy, quiet);
	CHECK(light[1] > 0.5f && hard[1] > 2 * light[1] && light[0] == 0);
	CHECK(quiet[1] == 0);
	heavy.slide = 0;
	Sim::Push(large, kAspect, down, heavy, quiet);
	CHECK(quiet[0] == 0 && quiet[1] == 0);
}

void CameraMotionMovesTheWater() {
	Params p;
	Drop d;
	d.x = 0.2f; d.y = 0.1f; d.r = 0.004f;
	Motion forward;
	forward.forward = 300;
	forward.gravity[1] = 0;
	float a[2];
	Sim::Push(d, kAspect, forward, p, a);
	CHECK(a[0] < 0 && a[1] < 0);
	d.x = 0.8f;
	Sim::Push(d, kAspect, forward, p, a);
	CHECK(a[0] > 0);
	Motion strafe; strafe.right = 400;
	Sim::Push(d, kAspect, strafe, p, a);
	CHECK(a[0] < 0);

	Params impact; impact.impact = 2; impact.slide = 0;
	Sim standing(9), running(9);
	for (int i = 0; i < 60; ++i) {
		standing.Step(1.0f / 60, kAspect, {}, impact);
		running.Step(1.0f / 60, kAspect, forward, impact);
	}
	CHECK(running.Drops().size() > standing.Drops().size());

	// Fast camera motion at the maximum Amount still respects the drop limit.
	impact.amount = 100;
	Motion sprint; sprint.forward = 1500;
	for (int i = 0; i < 240; ++i) {
		running.Step(1.0f / 60, kAspect, sprint, impact);
		CHECK(running.Drops().size() <= kMaxDrops);
	}
}

void RunoffFollowsTheGlass() {
	Params storm;
	storm.amount = 60;
	storm.size = 0.85f;
	Sim level(42), rolled(42);
	Motion down, left;
	left.gravity[0] = -1;
	left.gravity[1] = 0;
	int downward = 0, sideways = 0;
	for (int i = 0; i < 180; ++i) {
		level.Step(1.0f / 60, kAspect, down, storm);
		rolled.Step(1.0f / 60, kAspect, left, storm);
		for (const Drop& d : level.Drops()) {
			CHECK(d.trailX == 0 && d.trailY >= 0 && d.trailY <= kMaxRunoff + 1e-6f);
			CHECK(d.kind == Kind::Rivulet || d.trailY == 0);
			if (d.trailY > 0.012f) ++downward;
		}
		for (const Drop& d : rolled.Drops()) {
			CHECK(d.trailY == 0 && d.trailX <= 0);
			if (d.trailX < -0.012f) ++sideways;
		}
	}
	CHECK(downward > 0 && sideways > 0);
}

void SameStepsSameDrops() {
	Params p;
	Motion m; m.right = 120; m.forward = 200; m.yawRate = 40;
	Sim a(5), b(5);
	for (int i = 0; i < 240; ++i) { a.Step(1.0f / 60, kAspect, m, p); b.Step(1.0f / 60, kAspect, m, p); }
	b.Step(0, kAspect, m, p);
	CHECK(a.Drops().size() == b.Drops().size());
	for (size_t i = 0; i < a.Drops().size() && i < b.Drops().size(); ++i) {
		const Drop& before = a.Drops()[i];
		const Drop& after = b.Drops()[i];
		CHECK(before.x == after.x && before.y == after.y && before.age == after.age && before.trailY == after.trailY);
	}
}

void DropsFadeInAndDrain() {
	Drop d;
	d.life = 1.2f;
	CHECK(Opacity(d) == 0);
	d.age = 0.06f;
	CHECK(std::fabs(Opacity(d) - 0.5f) < 1e-6f);
	d.age = 0.5f;
	CHECK(Opacity(d) == 1);
	d.age = 1.04f;
	CHECK(std::fabs(Opacity(d) - 0.5f) < 1e-6f);
	d.age = d.life;
	CHECK(Opacity(d) == 0);
	Params p;
	Sim s = Rained(p, 2);
	CHECK(!s.Drops().empty());
	p.amount = 0;
	for (int i = 0; i < 60 * 2; ++i) s.Step(1.0f / 60, kAspect, {}, p);
	CHECK(s.Drops().empty());
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
	RainControlsDensityAndSize();
	HeavierRainRunsFaster();
	CameraMotionMovesTheWater();
	RunoffFollowsTheGlass();
	SameStepsSameDrops();
	DropsFadeInAndDrain();
	MotionFromTwoPoses();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "LensRainMathTests: all passed\n";
	return 0;
}
