#pragma once
// Lite lighting's bounce grid (RelightLiteGridMath.h): the worker that bakes it and the volume the
// lit programs read. Nothing here runs per frame once a bake is done.
#include "RelightProbes.h"
#include <string>

namespace Filmmaker::Relight {
// Render thread, every Lite frame. Starts or refreshes the bake when the map, its lamps, the sun
// or the colours change, and returns the grid's binding. Empty until the first bake is done (the
// caller keeps the map-wide fill until then); after that the last finished grid stays bound while
// a newer one is worked out.
ProbeBinding PrepareLiteGrid(ID3D11Device* device,const std::shared_ptr<const Environment::OcclusionMesh>& world,
    const Scene& scene);
// One line for `mirv_relight`: what the worker is doing and how long the last bake took.
std::string LiteGridStatus();
// Stops the worker and frees the grid.
void EndLiteGrid();
}
