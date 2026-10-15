#pragma once

// World collision -> the runtime's opaque occlusion mesh (MVMOCC01). Physics shape decoding
// follows ValveResourceFormat 20.0 (MIT) RubikonPhysics and its physics mesh export; the
// opaque-geometry rules are the ones the map-lighting cache has always used.

#include "MapDataIo.h"

#include <array>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct OcclusionMesh {
    std::vector<std::array<float, 3>> vertices; // Source units, exact-position deduplicated
    std::vector<uint32_t> indices;
    std::array<float, 3> low{}, high{};
    std::vector<std::string> excluded; // physics groups left out (clip, sky, window, glass)
};

// maps/<map>/world_physics (.vphys_c or a .vmdl_c with embedded physics) of the map package.
OcclusionMesh BuildOcclusionMesh(const Vpk &package, const std::string &map);

// MVMOCC01: magic, vertex count, index count, bounds low/high, float3 vertices, uint32 indices.
std::vector<uint8_t> SerializeOcclusion(const OcclusionMesh &mesh);

} // namespace Filmmaker::MapData
