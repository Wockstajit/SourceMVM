#include "../../AfxHookSource2/Filmmaker/SceneEditor/SceneEditorMath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace Filmmaker;
using namespace Filmmaker::SceneEditor;

static int g_fails = 0;
#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_fails;                                                  \
		}                                                               \
	} while (0)

static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

static void TestRecordedPathAndSpatialOffset() {
	Takes::Track t;
	t.name = "Entry";
	t.delayTicks = 10;
	t.ticks.resize(5);
	for (int i = 0; i < 5; ++i) {
		t.ticks[i].pos[0] = float(i * 10);
		t.ticks[i].pos[1] = 0.f;
		t.ticks[i].pos[2] = float(i * 5); // climbs 20u
		t.ticks[i].vel[0] = 64.f;
		t.ticks[i].yaw = 0.f;
	}
	t.offsetPos[0] = 50.f;
	t.offsetPos[1] = -20.f;
	t.offsetPos[2] = 10.f;
	t.offsetYaw = 90.f;

	ActorAnim::Cue cue;
	cue.startTick = 1;
	cue.clip = "hla_test_jump";
	cue.duration = 0.05f;
	cue.blendIn = 0.01f;
	cue.blendOut = 0.01f;
	t.animCues.push_back(cue);

	const TrackVisualPath path = BuildTrackVisualPath(t, 64.f, nullptr, 12);
	CHECK(path.valid);
	CHECK(path.points.size() == 5);
	CHECK(Near(path.startPos[0], 50.f) && Near(path.startPos[1], -20.f) && Near(path.startPos[2], 10.f));
	CHECK(Near(path.startYaw, 90.f));
	// +40 along X rotated +90 deg -> +40 along Y => (50, 20, 30)
	CHECK(Near(path.endPos[0], 50.f) && Near(path.endPos[1], 20.f) && Near(path.endPos[2], 30.f));
	CHECK(Near(path.scrubPos[0], 50.f) && Near(path.scrubPos[1], 0.f) && Near(path.scrubPos[2], 20.f));
	CHECK(path.hasAirborneArc);
	CHECK(path.groundPoints.size() == 5);
	CHECK(Near(path.groundPoints.back().pos[2], 10.f));
	CHECK(path.cueMarkers.size() == 1);
	CHECK(Near(path.cueMarkers[0][1], -10.f));

	const GraphCurve zCurve = BuildTrackGraphCurve(t, 64.f, nullptr, GraphChannel::HeightZ);
	CHECK(zCurve.samples.size() == 5);
	CHECK(Near(zCurve.minVal, 10.f) && Near(zCurve.maxVal, 30.f));

	const GraphCurve spdCurve = BuildTrackGraphCurve(t, 64.f, nullptr, GraphChannel::Speed);
	CHECK(Near(spdCurve.samples[0].value, 64.f));

	const GraphCurve yawCurve = BuildTrackGraphCurve(t, 64.f, nullptr, GraphChannel::Yaw);
	CHECK(Near(yawCurve.samples[0].value, 90.f));

	const GraphCurve animCurve = BuildTrackGraphCurve(t, 64.f, nullptr, GraphChannel::AnimWeight);
	CHECK(Near(animCurve.samples[0].value, 0.f));
	CHECK(animCurve.samples[2].value > 0.5f);
}

static void TestPlacedActorJumpArcAndPick() {
	ActorAnim::Library lib;
	ActorAnim::Clip jump;
	jump.id = "hla_jump_down";
	jump.category = uint8_t(ActorAnim::Category::Cinematic);
	jump.fps = 10.f;
	jump.hasRootMotion = true;
	jump.rootSpeed = 50.f;
	jump.frames.resize(11);
	for (int f = 0; f <= 10; ++f) {
		const float u = float(f) / 10.f;
		jump.frames[f].pelvisDelta[0] = 50.f * u;
		jump.frames[f].groundLift = 20.f * (1.f - u * u); // drops 20u arc
	}
	lib.clips.push_back(jump);

	Takes::Track t;
	t.name = "Actor 1";
	t.placedActor = true;
	t.ticks.resize(65);
	for (auto &s : t.ticks) { s.pos[0] = 10.f; s.pos[1] = 20.f; s.pos[2] = 100.f; s.yaw = 0.f; }
	ActorAnim::Cue cue;
	cue.clip = "hla_jump_down";
	cue.loop = false;
	t.animCues.push_back(cue);

	const TrackVisualPath path = BuildTrackVisualPath(t, 64.f, &lib, 32);
	CHECK(path.valid && path.hasAirborneArc);
	CHECK(Near(path.startPos[0], 10.f) && Near(path.startPos[2], 120.f, 0.2f));
	CHECK(Near(path.endPos[0], 60.f, 0.5f) && Near(path.endPos[2], 100.f, 0.2f));
	CHECK(path.groundPoints.size() == path.points.size());

	const float uv[3][3] = {{0.2f, 0.2f, 1.f}, {0.5f, 0.5f, 1.f}, {0.8f, 0.8f, 0.f}};
	CHECK(PickNearestProjected(0.49f, 0.51f, uv, 3, 0.04f) == 1);
	CHECK(PickNearestProjected(0.80f, 0.80f, uv, 3, 0.04f) == -1); // invisible point ignored
}

int main() {
	TestRecordedPathAndSpatialOffset();
	TestPlacedActorJumpArcAndPick();
	if (g_fails) {
		std::printf("scene-editor-math-tests: %d failure(s)\n", g_fails);
		return EXIT_FAILURE;
	}
	std::printf("scene-editor-math-tests: all passed\n");
	return EXIT_SUCCESS;
}
