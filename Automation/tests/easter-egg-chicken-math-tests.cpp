#include "../../AfxHookSource2/Filmmaker/EasterEggs/EasterEggChickenMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::EasterEggChicken;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) < eps; }

void TestFallBounceLandAndExpire() {
	Bird b;
	b.z = 800.0; b.groundZ = 0.0; b.spinPitch = 300.0;
	int bounces = 0;
	double lastVz = 0.0, minZ = 1e9;
	double t = 0.0;
	bool alive = true;
	while (alive && t < 60.0) {
		lastVz = b.vz;
		alive = Step(b, 1.0 / 60.0);
		t += 1.0 / 60.0;
		if (lastVz < 0.0 && b.vz > 0.0) bounces++;
		if (b.z < minZ) minZ = b.z;
	}
	CHECK(minZ >= 0.0);             // never sinks through the floor
	CHECK(bounces >= 1);            // a drop from 800 bounces at least once
	CHECK(bounces <= 4);            // ...but settles quickly
	CHECK(b.landed);
	CHECK(b.pitch == 0.0 && b.roll == 0.0); // sits upright
	CHECK(!alive);                  // and is eventually cleaned up
	CHECK(t > kSitSeconds);
	CHECK(t < 15.0);
}

void TestHitchClamp() {
	// A one-second hitch must not tunnel a bird far below the floor before the clamp catches it.
	Bird b;
	b.z = 100.0; b.groundZ = 0.0;
	Step(b, 1.0);
	CHECK(b.z >= 0.0);
	CHECK(b.z < 100.0);
}

void TestAngleQuaternion() {
	float q[4];
	AngleQuaternion(0, 0, 0, q);
	CHECK(Near(q[0], 0) && Near(q[1], 0) && Near(q[2], 0) && Near(q[3], 1));
	// Pure yaw of 90 degrees: rotation about +Z.
	AngleQuaternion(0, 90, 0, q);
	CHECK(Near(q[2], std::sqrt(0.5)) && Near(q[3], std::sqrt(0.5)) && Near(q[0], 0) && Near(q[1], 0));
	// Pure pitch: rotation about +Y; pure roll: about +X.
	AngleQuaternion(90, 0, 0, q);
	CHECK(Near(q[1], std::sqrt(0.5)) && Near(q[0], 0) && Near(q[2], 0));
	AngleQuaternion(0, 0, 90, q);
	CHECK(Near(q[0], std::sqrt(0.5)) && Near(q[1], 0) && Near(q[2], 0));
	// Always unit length.
	AngleQuaternion(37, -120, 211, q);
	CHECK(Near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0, 1e-5));
}

void TestWrap() {
	CHECK(Near(WrapDegrees(190), -170));
	CHECK(Near(WrapDegrees(-190), 170));
	CHECK(Near(WrapDegrees(720 + 45), 45));
}

} // namespace

int main() {
	TestFallBounceLandAndExpire();
	TestHitchClamp();
	TestAngleQuaternion();
	TestWrap();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "EasterEggChickenMathTests passed\n";
	return 0;
}
