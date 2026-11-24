#include "../../AfxHookSource2/Filmmaker/WorldWarp/WorldWarpMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::WorldWarp;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

void TestLittlePlanetConformalDirections() {
	Settings s = PresetFor(Mode::LittlePlanet);
	// Center of screen must look straight down at the street / bombsite (nadir: z = -1).
	Vec3 center = LittlePlanetDirection(0.0f, 0.0f, s);
	CHECK(std::fabs(center.x) < 1e-4f && std::fabs(center.y) < 1e-4f && center.z < -0.999f);

	// Mid-radius (r == scale) must land on the horizon (buildings fanning outward: z ~ 0).
	Vec3 horizon = LittlePlanetDirection(s.planetScale, 0.0f, s);
	CHECK(std::fabs(horizon.z) < 1e-3f && horizon.x > 0.99f);

	// Far outer rim must point upward into the sky (z > 0).
	Vec3 rim = LittlePlanetDirection(s.planetScale * 2.5f, 0.0f, s);
	CHECK(rim.z > 0.5f);

	// Spin rotates azimuth cleanly.
	s.planetSpin = 90.0f;
	Vec3 spun = LittlePlanetDirection(s.planetScale, 0.0f, s);
	CHECK(std::fabs(spun.x) < 1e-3f && spun.y > 0.99f);
}

void TestRollingHorizonFlatZoneAndSign() {
	Settings s = PresetFor(Mode::RollingHorizon);
	s.horizonFlatRadius = 300.0f;
	s.horizonBend = 1.0f;
	Vec3 anchor{ 0, 0, 0 };

	// Inside flat radius -> untouched!
	Vec3 nearPt = DeformRollingHorizon({ 200.0f, 0.0f, 10.0f }, anchor, s);
	CHECK(std::fabs(nearPt.z - 10.0f) < 1e-5f);

	// Beyond flat radius -> lifts upward for positive bend.
	Vec3 farUp = DeformRollingHorizon({ 1300.0f, 0.0f, 10.0f }, anchor, s);
	CHECK(farUp.z > 400.0f);

	// Negative bend curves downward.
	s.horizonBend = -1.0f;
	Vec3 farDown = DeformRollingHorizon({ 1300.0f, 0.0f, 10.0f }, anchor, s);
	CHECK(farDown.z < -400.0f);
}

void TestWorldWaveDirectionAndPeriodicity() {
	Settings s = PresetFor(Mode::WorldWave);
	s.waveHeight = 100.0f;
	s.waveLength = 400.0f;
	s.waveDirection = 0.0f; // along +X
	s.waveFlatRadius = 0.0f;
	Vec3 origin{ 0, 0, 0 };

	// Quarter wavelength = peak (+100), three-quarters = trough (-100), full = 0.
	Vec3 q1 = DeformWorldWave({ 100.0f, 0.0f, 0.0f }, origin, origin, 0.0, s);
	Vec3 q3 = DeformWorldWave({ 300.0f, 0.0f, 0.0f }, origin, origin, 0.0, s);
	Vec3 full = DeformWorldWave({ 400.0f, 0.0f, 0.0f }, origin, origin, 0.0, s);
	CHECK(std::fabs(q1.z - 100.0f) < 1e-2f);
	CHECK(std::fabs(q3.z + 100.0f) < 1e-2f);
	CHECK(std::fabs(full.z) < 1e-2f);

	// Advancing time moves the wave crest.
	Vec3 moved = DeformWorldWave({ 100.0f, 0.0f, 0.0f }, origin, origin, 1.0, s);
	CHECK(std::fabs(moved.z - q1.z) > 10.0f);
}

void TestWorldFoldRigidLift() {
	Settings s = PresetFor(Mode::WorldFold);
	s.foldAxisYaw = 0.0f;  // hinge runs along +X, normal is +Y
	s.foldOffset = 100.0f; // hinge line at y = 100
	s.foldSoftness = 0.0f; // sharp rigid hinge for exact angle test
	s.foldAngle = 90.0f;
	Vec3 anchor{ 0, 0, 0 };

	// Unlifted side (y < 100) stays completely flat.
	Vec3 flat = DeformWorldFold({ 50.0f, 40.0f, 0.0f }, anchor, s);
	CHECK(std::fabs(flat.y - 40.0f) < 1e-4f && std::fabs(flat.z) < 1e-4f);

	// Lifted side (y = 300, i.e. u = 200 past hinge) folds 90 deg straight up to (y=100, z=200)!
	Vec3 lifted = DeformWorldFold({ 50.0f, 300.0f, 0.0f }, anchor, s);
	CHECK(std::fabs(lifted.x - 50.0f) < 1e-3f);
	CHECK(std::fabs(lifted.y - 100.0f) < 1e-2f);
	CHECK(std::fabs(lifted.z - 200.0f) < 1e-2f);
}

} // namespace

void TestCubemapSetNeedsOneDemoTime() {
	using Step = CubemapSetTracker::Step;
	CubemapSetTracker t;
	// Six faces at one time publish on face 5.
	for (int f = 0; f < 5; ++f) { CHECK(t.OnFace(f, 10.0, true) == Step::Capture); CHECK(t.MidSet()); }
	CHECK(t.OnFace(5, 10.0, true) == Step::CaptureAndPublish);
	CHECK(!t.MidSet() && t.next == 0);
	// The demo moved after face 2: the set is dropped and starts again at face 0.
	CHECK(t.OnFace(0, 11.0, true) == Step::Capture);
	CHECK(t.OnFace(1, 11.0, true) == Step::Capture);
	CHECK(t.OnFace(2, 11.0 + 1.0 / 64.0, true) == Step::Discard);
	CHECK(t.next == 0 && !t.MidSet());
	// A face out of order (a frame that rendered a stale face) also restarts the set.
	CHECK(t.OnFace(0, 12.0, true) == Step::Capture);
	CHECK(t.OnFace(3, 12.0, true) == Step::Discard);
	// Without a demo clock to hold, a complete set publishes as it is.
	for (int f = 0; f < 5; ++f) CHECK(t.OnFace(f, 20.0 + f, false) == Step::Capture);
	CHECK(t.OnFace(5, 30.0, false) == Step::CaptureAndPublish);
}

int main() {
	TestLittlePlanetConformalDirections();
	TestRollingHorizonFlatZoneAndSign();
	TestWorldWaveDirectionAndPeriodicity();
	TestWorldFoldRigidLift();
	TestCubemapSetNeedsOneDemoTime();
	if (g_failed) {
		std::cerr << g_failed << " WorldWarpMathTests failed\n";
		return 1;
	}
	return 0;
}
