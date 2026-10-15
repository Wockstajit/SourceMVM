#pragma once

// World / prop / sun-cloud colour modulation -- tints the actual geometry as it is drawn.
//
// WHY THIS EXISTS RATHER THAN MORE LIGHT CONTROLS: CS2 competitive maps are almost entirely BAKED
// lighting. Tinting the dynamic scene lights (World/LightFx.cpp) is correct, works, and is very
// nearly invisible on Mirage, because those lights contribute almost nothing at a typical camera.
// Measured: 0.000 mean pixel difference at 8x intensity. If you want the world to actually look
// different, you have to colour the world, not the lights.
//
// Static world, model/foliage, and sun/sky materials are tinted through their named g_vColorTint
// shader parameter when present. Streamed submaterials without that parameter and transient
// effects use the guarded per-draw colour in CBaseSceneData.
//
// THE HOOK ALREADY EXISTS. `new_DrawBaseSceneObject` (CBaseSceneObjectDesc::DrawArray) is the
// scene-draw detour in SceneSystem.cpp, and it is genuinely the home for scene-draw work; a second
// detour on the same vtable slot would be the coupling mistake, not the fix. SceneSystem calls
// INTO this module and this module owns all the policy -- the arrangement Docs/effects/README.md
// records as the one deliberate exception to the "own your own hook" rule.
//
// CATEGORIES are decided by material name, the same split the reference research uses:
//   sun / clouds  -> the sky's sun and cloud layers
//   effects       -> particle and effect surfaces
//   everything else -> world geometry and props
//
// THREADING: the draw hook runs on the render thread and reads atomics only. Material enumeration
// and writes run on the main thread from WorldModulation_RunFrame.
//
// See Docs/effects/world-modulation.md.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {
void WorldModulation_OnLevelInit();

enum class ModCategory { World = 0, Props = 1, SunClouds = 2, Effects = 3, Count = 4 };

// True when anything at all is overridden, so the draw hook can early-out for free.
bool WorldModulation_Active();

// Called from new_DrawBaseSceneObject on the render thread. Returns false to leave the batch
// alone; otherwise writes the packed RGBA to apply.
bool WorldModulation_ColorFor(const char* materialName, unsigned char outRgba[4]);
// Debug: true when `world modulate hide <bits> <value>` is armed and this material's name hashes into the
// chosen bucket (FNV-1a, low <bits> bits == <value>). Cheap when off: one relaxed atomic load.
bool WorldModulation_HideDraw(const char* materialName);

void WorldModulation_SetTint(ModCategory c, int r, int g, int b);
void WorldModulation_SetEnabled(ModCategory c, bool on);
void WorldModulation_Reset();
void WorldModulation_GetTint(ModCategory c,int* r,int* g,int* b,bool* enabled);

// Main-thread material refresh. Detects newly streamed map materials immediately and periodically
// reasserts the tint while active. Cheap no-op when no material-backed category is enabled.
void WorldModulation_RunFrame();

// Bounded stride probe -- see the note in the .cpp. Called from the draw hook.
void WorldModulation_ProbeStride(int batches);
void WorldModulation_ProbeBatch(const void* sceneData, int count, const char* materialName);

// ---- AGGREGATE (batched static world geometry) ----------------------------------------------
// Most of a CS2 map is not drawn through CBaseSceneObjectDesc at all -- it is drawn through
// CAggregateSceneObjectDesc, and an aggregate IGNORES the per-draw colour. Its colour lives in the
// aggregate object's own per-instance data array. Tinting only the per-draw field is exactly why
// the first working build coloured a wall patch and a doormat and left the rest of Mirage alone.
//
// Called from SceneSystem's aggregate detour on the render thread, around the original call:
//   const int n = WorldModulation_AggregateApply(sceneData, count);
//   original(...);
//   if (n) WorldModulation_AggregateRestore();
//
// Returns how many instance colours were newly written this call.
//
// STICKY: there is no restore around the original call, because aggregate objects are shared across
// render threads and a straddling save/restore tears (flickering geometry, colours that survive a
// reset). Originals are remembered and put back by WorldModulation_AggregateRestoreAll on reset.
int  WorldModulation_AggregateApply(void* batchArray, int count);
void WorldModulation_AggregateRestoreAll();

// 0 = untinted, 1 = full tint.
void  WorldModulation_SetStrength(float s);
float WorldModulation_Strength();

// Bounded layout probe for the aggregate object -- reports candidate (count, data*) field pairs
// so the offsets are MEASURED after a CS2 update instead of trusted from a 2024 struct dump.
void WorldModulation_ProbeAggregate(int batches);

void WorldModulation_AppendStateJson(std::ostringstream& o);
void WorldModulation_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

namespace Filmmaker { void WorldModulation_RefreshTransient(); }
