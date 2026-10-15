#pragma once

// The geometry CS2 lights from its baked lightmap, in the lightmap atlas's own coordinates
// (MVMLMAP2), for `mirv_relight lightmap <file>`. World shaders read baked irradiance through a
// per-vertex vLightmapUV; this records which world point every atlas texel belongs to, straight
// from the compiled map (mesh streams plus the world's lightmap UV scale). Nothing is guessed.

#include "MapDataIo.h"

#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct LightmapStats {
    size_t triangles = 0, vertices = 0, points = 0;
    int atlasWidth = 0, atlasHeight = 0, atlasPages = 0;
};

std::vector<uint8_t> BuildLightmapLayout(const Vpk &package, const PackageSet &resources, const std::string &map, LightmapStats &stats);

} // namespace Filmmaker::MapData
