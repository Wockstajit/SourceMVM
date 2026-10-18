#pragma once

// Fast raycasts against the map collision (the Environment's extracted map data), for every
// main-thread caller: ragdoll weapons and death animations, Method Acting's line of sight, the
// scene objects' paint brush and placement.
//
// SurfaceRaycast walks every triangle of the map -- about 10 ms a ray on de_ancient (measured
// 2026-10-18), which made the paint brush hitch for 30-85 ms a spray (one ground probe per copy)
// and its preview probe every state update. The grid (SurfaceRayGridMath.h) is built once per
// map on a worker; until it is ready a ray falls back to the full sweep.

#include "EnvironmentSurfaceMath.h"

namespace Filmmaker::Environment {

// Main thread, every frame (cheap while the map is unchanged): starts or collects the grid build.
void MapCollisionGrid_Pump();
const char* MapCollisionGrid_Status();
bool MapCollisionGrid_Ready();
// The nearest map-collision hit along the ray. False without map data or without a hit.
bool MapCollisionRaycast(Vec3 origin, Vec3 direction, float range, SurfaceHit& hit);

} // namespace Filmmaker::Environment
