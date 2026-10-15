#pragma once
#include "../Environment/EnvironmentMath.h"

// CLIENT-SIDE ENTITY SPAWN: the engine's own CEntitySpawner<T>::Spawn sequence, run from the client,
// so a model can be placed in a DEMO (no server to ask). Located by signature; see the .cpp.
namespace Filmmaker::Props {
using Environment::Vec3;
bool ClientEntitySpawnAvailable();
// Blocking load of a model resource (CResourceSystem::PreCache). SpawnClientEntity calls it itself;
// call it ahead of time (on placement) to keep the first spawn of a heavy model off the frame.
bool PrecacheModel(const char *model);
// Spawns `classname` (prop_dynamic) with `model` (a .vmdl path, or "" for a class without one) at a
// world pose. Returns the entity (CEntityInstance*), or nullptr. Main thread only. `scale` is the
// model scale (1 = authored size). `extra` is further keyvalues as name, value pairs ending in a
// nullptr ({"minwind", "10", nullptr}), parsed by the entity's own spawn as if from a map.
unsigned char *SpawnClientEntity(const char *classname, const char *name, const char *model,
                                 Vec3 position, Vec3 angles, float scale, const char *const *extra = nullptr);
// UTIL_Remove on a client-only entity we spawned. Main thread only.
bool RemoveClientEntity(unsigned char *entity);
// The entity-list index of a CEntityInstance (-1 if unreadable), to tell whether it still exists.
int ClientEntityIndex(unsigned char *entity);
// Model-space collision bounds of a spawned entity (before its scale). False when unreadable.
bool ClientEntityBounds(unsigned char *entity, Vec3 &mins, Vec3 &maxs);
// Resolves the bounds offsets. Called from SchemaSystem.cpp before the schema name map is cleared.
void ClientEntitySpawn_InitSchema();
} // namespace Filmmaker::Props
