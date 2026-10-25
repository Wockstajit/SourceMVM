#pragma once

// ALTERNATIVE ENVIRONMENT research probe: what the scene objects the world and its entities draw
// through hold. Read-only. `mirv_filmmaker altenv sceneprobe [n]` dumps the next n scene objects the
// base and aggregate draw detours see into the mvm_debug log (channel altenv.sceneprobe), after a
// line with the world offset the playing copy applied.
//
// Measured with it 2026-10-24 (Inferno demo on de_eldorado, align -2488 -3152 31.2 yaw -15.5):
//   +0x30  the placement, a row-major 3x4 matrix. World aggregates and the world's plain meshes hold
//          the copy's spawn-group offset there (rows cos -sin 0 x / sin cos 0 y / 0 0 1 z);
//          aggregates carry a second copy at +0x118. No bounds sit next to it.
//   +0xC0  the owner entity handle (index in the low 15 bits); 0xffffffff for the world. The props
//          hiding reads it (AltEnvironmentProps.h).
// Rewriting the +0x30 / +0x118 matrices of every world object being drawn did NOT move the world:
// the renderer draws it from data built at load, so moving a loaded world live would need those
// load-time buffers (most likely the aggregates' per-fragment GPU transforms), not this matrix.

#include <cstddef>

namespace Filmmaker {

// Cheap enough for every draw call: one relaxed atomic load.
bool AltEnvironment_SceneProbeArmed();
// From the scene-system draw detours (SceneSystem.cpp): `count` batch elements `stride` bytes apart,
// each holding its scene object pointer at `objectOffset`. `via` names the detour.
void AltEnvironment_SceneProbeNote(const void* batch, int count, size_t stride, size_t objectOffset, const char* via);
// Arms the dump for `maxObjects` distinct scene objects; `expected` describes the world offset the
// copy applied, logged first so the dumps can be read against it.
void AltEnvironment_SceneProbeArm(int maxObjects, const char* expected);

} // namespace Filmmaker
