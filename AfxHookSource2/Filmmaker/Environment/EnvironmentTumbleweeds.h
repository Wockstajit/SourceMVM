#pragma once
// TUMBLEWEEDS RUNTIME (EnvironmentTumbleweedMath.h): asynchronously loads the bundled Russian
// thistle GLB (resources/environment/tumbleweed/tumbleweed.glb), steps the rolling physics against
// the map's collision grid on demo time, and publishes the live poses for EnvironmentRender.cpp.
// Render thread only (except TumbleweedsStatus).
#include "EnvironmentScene.h"
#include "EnvironmentTumbleweedMath.h"
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker::Environment {

struct TumbleweedInstance {
    Vec3 position{};
    float radius = 16.f;
    Matrix model{};
};

struct TumbleweedFrame {
    std::shared_ptr<const Mesh> mesh;
    std::vector<TumbleweedInstance> items;
};

// Render thread, once per frame. Returns empty items when CL_tumbleweeds == 0 or while the GLB is
// still loading asynchronously. Steps once per demo timestamp so HDR + display passes match.
TumbleweedFrame PrepareTumbleweeds(const Frame &frame, const Climate &climate, const Matrix &worldToClip);
// Whether one of the filmmaker's own cameras (a path, free cam, attach, scene camera, ...) owns the
// view this frame. Set by the view setup every frame; the tumbleweeds read it to tell first person
// (the game's own view from a player's eyes) from a cinematic camera. Any thread.
void Tumbleweeds_SetFilmmakerCamera(bool owned);
std::string TumbleweedsStatus();
void ReleaseTumbleweeds();

} // namespace Filmmaker::Environment
