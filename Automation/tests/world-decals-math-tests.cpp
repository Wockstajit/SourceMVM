#include "../../AfxHookSource2/Filmmaker/WorldDecals/WorldDecalsMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::WorldDecals;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

void TestObbProjection() {
	Decal d;
	d.position[0] = 100.0f; d.position[1] = 0.0f; d.position[2] = 50.0f;
	d.angles[0] = 0.0f; d.angles[1] = 0.0f; d.angles[2] = 0.0f; // forward = +X
	d.width = 100.0f; d.height = 50.0f; d.depth = 40.0f;

	float u = 0, v = 0, z = 0;
	// Exact center maps to (0.5, 0.5, 0)
	CHECK(ProjectWorldToDecal({ 100.0f, 0.0f, 50.0f }, d, u, v, z));
	CHECK(std::fabs(u - 0.5f) < 1e-4f && std::fabs(v - 0.5f) < 1e-4f && std::fabs(z) < 1e-4f);

	// Outside depth range -> rejected
	CHECK(!ProjectWorldToDecal({ 130.0f, 0.0f, 50.0f }, d, u, v, z));
	// Outside height -> rejected
	CHECK(!ProjectWorldToDecal({ 100.0f, 0.0f, 90.0f }, d, u, v, z));
}

void TestAnimatedRouteDrawing() {
	Decal r;
	r.kind = DecalKind::Route;
	r.routeAnimate = true;
	r.routeStartTime = 10.0f;
	r.routeDuration = 4.0f;
	r.routePoints = { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 100, 0 } }; // total length = 200

	CHECK(std::fabs(EvaluateRouteProgress(r, 10.0) - 0.0f) < 1e-5f);
	CHECK(std::fabs(EvaluateRouteProgress(r, 12.0) - 0.5f) < 1e-5f);
	CHECK(std::fabs(EvaluateRouteProgress(r, 15.0) - 1.0f) < 1e-5f);

	// At 50% progress (drawn length 100), point (50, 0, 0) is on the drawn route, while (100, 80, 0) is not yet drawn!
	float arc = 0, drawn = 0;
	const float dOn = DistanceToAnimatedRoute({ 50.0f, 0.0f, 0.0f }, r.routePoints, 0.5f, arc, drawn);
	CHECK(dOn < 1e-3f && std::fabs(arc - 50.0f) < 1e-2f && std::fabs(drawn - 100.0f) < 1e-2f);

	const float dFuture = DistanceToAnimatedRoute({ 100.0f, 80.0f, 0.0f }, r.routePoints, 0.5f, arc, drawn);
	CHECK(dFuture > 75.0f);

	// At 100% progress, (100, 80, 0) is right on the route!
	const float dFull = DistanceToAnimatedRoute({ 100.0f, 80.0f, 0.0f }, r.routePoints, 1.0f, arc, drawn);
	CHECK(dFull < 1e-3f && std::fabs(arc - 180.0f) < 1e-2f);
}

void TestSceneObjectAttachment() {
	float parentPos[3] = { 100.0f, 200.0f, 50.0f };
	float parentAng[3] = { 0.0f, 90.0f, 0.0f }; // facing +Y
	float localOff[3] = { 20.0f, 0.0f, 10.0f }; // 20 forward (+Y), 10 up (+Z)
	float localAng[3] = { 0.0f, 15.0f, 0.0f };
	float outPos[3] = {}, outAng[3] = {};
	ComposeAttachedPose(parentPos, parentAng, localOff, localAng, outPos, outAng);

	CHECK(std::fabs(outPos[0] - 100.0f) < 1e-2f);
	CHECK(std::fabs(outPos[1] - 220.0f) < 1e-2f);
	CHECK(std::fabs(outPos[2] - 60.0f) < 1e-2f);
	CHECK(std::fabs(outAng[1] - 105.0f) < 1e-2f);
}

} // namespace

int main() {
	TestObbProjection();
	TestAnimatedRouteDrawing();
	TestSceneObjectAttachment();
	if (g_failed) {
		std::cerr << g_failed << " WorldDecalsMathTests failed\n";
		return 1;
	}
	return 0;
}
