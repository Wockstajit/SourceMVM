#pragma once

// ALTERNATIVE ENVIRONMENT: the demo's own map entities on another map
// (Docs/features/alternative-environment.md). A demo records the networked entities its server
// sent, so the original map's doors, breakables, physics props and chickens play on in the
// replacement world: they float where the original map had them, and anything whose model lives in
// the original map's package (brush entities compile to maps/<map>/entities/*.vmdl) draws as ERROR,
// because that package is not mounted.
//
// Hidden at the draw. Every entity's scene object carries its owner entity handle at +0xC0 (world
// objects hold 0xffffffff; measured 2026-10-24 with `altenv sceneprobe`), so the scene-system draw
// detour skips a batch whose draws all belong to hidden entities, and draws only the rest of a batch
// that mixes them with shown ones. Mixed batches are common where the replacement map uses the same
// models or materials (Rainy Inferno: 4-6 a frame); drawing them whole left hidden props, ERRORs
// included, on screen. Writes to the entity itself were
// measured not to reach the renderer: CRenderComponent::m_bEnableRendering, EF_NODRAW in m_fEffects,
// the scene node's m_flScale and a mesh-group mask that selects no group all left the props drawn.

#include <cstddef>
#include <string>

namespace Filmmaker::AltEnvironment::Props {

// Once per main-thread frame. active: the demo plays on another map. showProps: keep the original
// map's model props (doors, breakables, physics props, chickens); what draws from the original map's
// own package is hidden whenever active, since it can only draw as ERROR or as the wrong geometry.
void Update(bool active, bool showProps, const std::string& demoMap);
// Everything drawn again (props shown, shutdown).
void RestoreAll();
// The level went away (a reload or a switch): its entities went with it.
void Forget();
size_t HiddenCount();
// Logs every entity class with a count and an example model, if it has one (channel altenv.props).
void Census();
// Logs every lighting, probe, cubemap, sky, fog and post-processing entity with its index and
// position (channel altenv.lights). Indices under 16384 came from the demo's server; higher ones
// the client spawned from the loaded map.
void LightCensus();

// From the scene-system draw detours (SceneSystem.cpp), on any render thread. Hiding() is one relaxed
// atomic load; HiddenInBatch counts the batch elements (`count`, `stride` bytes apart, each holding its
// scene object at `objectOffset`) whose owner entity is hidden. The detour skips the batch when all are.
bool Hiding();
int HiddenInBatch(const void* batch, int count, size_t stride, size_t objectOffset);
// For a mixed batch: saves the whole batch to `saved` (count * stride bytes), moves the elements of
// shown entities to its front in order and returns how many there are. The detour draws that many,
// then RestoreBatch puts the engine's batch back as it was. Returns `count` (nothing moved) when the
// batch cannot be read.
int CompactBatch(void* batch, int count, size_t stride, size_t objectOffset, unsigned char* saved);
void RestoreBatch(void* batch, int count, size_t stride, const unsigned char* saved);

} // namespace Filmmaker::AltEnvironment::Props
