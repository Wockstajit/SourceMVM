#include "../../AfxHookSource2/Filmmaker/Render/RenderPlayerMatteMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::PlayerMatte;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

void TestSinglePlayerOwnership() {
	RenderSnapshot snap;
	snap.hasTarget = true;
	snap.targetBoneCount = 2;
	snap.targetBones[0][0] = 100.0f; snap.targetBones[0][1] = 200.0f; snap.targetBones[0][2] = 36.0f; snap.targetBones[0][3] = 16.0f;
	snap.targetBones[1][0] = 100.0f; snap.targetBones[1][1] = 200.0f; snap.targetBones[1][2] = 64.0f; snap.targetBones[1][3] = 14.0f;
	ComputeTargetBounds(snap);

	// Inside torso -> 1.0
	CHECK(EvaluateOwnership({ 104.0f, 202.0f, 40.0f }, snap) == 1.0f);
	// Far away -> 0.0
	CHECK(EvaluateOwnership({ 400.0f, 200.0f, 40.0f }, snap) == 0.0f);
}

void TestCrowdedShotVoronoiSeparation() {
	RenderSnapshot snap;
	snap.hasTarget = true;
	snap.targetCapsule[0] = 0.0f; snap.targetCapsule[1] = 0.0f; snap.targetCapsule[2] = 0.0f; snap.targetCapsule[3] = 74.0f;
	snap.targetBoneCount = 1;
	snap.targetBones[0][0] = 0.0f; snap.targetBones[0][1] = 0.0f; snap.targetBones[0][2] = 40.0f; snap.targetBones[0][3] = 16.0f;
	snap.otherCount = 1;
	// Second player standing right next to them (40 units away)
	snap.otherCapsules[0][0] = 40.0f; snap.otherCapsules[0][1] = 0.0f; snap.otherCapsules[0][2] = 0.0f; snap.otherCapsules[0][3] = 74.0f;
	ComputeTargetBounds(snap);

	// Point on target player's side -> owned by target
	CHECK(EvaluateOwnership({ 4.0f, 0.0f, 40.0f }, snap) == 1.0f);
	// Point on other player's side -> rejected cleanly
	CHECK(EvaluateOwnership({ 36.0f, 0.0f, 40.0f }, snap) == 0.0f);
	// Midpoint -> smooth transition
	const float mid = EvaluateOwnership({ 21.0f, 0.0f, 40.0f }, snap);
	CHECK(mid >= 0.0f && mid <= 1.0f);
}

} // namespace

int main() {
	TestSinglePlayerOwnership();
	TestCrowdedShotVoronoiSeparation();
	if (g_failed) {
		std::cerr << g_failed << " RenderPlayerMatteMathTests failed\n";
		return 1;
	}
	return 0;
}
