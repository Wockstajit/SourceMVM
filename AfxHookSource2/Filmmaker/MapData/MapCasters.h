#pragma once

// Placed props, world fragments and foliage of a map's world nodes -> the runtime's visual
// shadow casters (MVMCAST6): opaque render geometry with surface albedo, alpha-tested
// cutouts with their masks and native foliage-sway inputs, and stained-glass transmitters.
// Scene traversal, LOD choice, material/tint keying and texture selection follow
// ValveResourceFormat 20.0 (MIT) world-node glTF export; the caster rules are the ones the
// map-lighting cache has always applied.

#include "MapDataIo.h"

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct CasterStats {
    size_t batches = 0, cutoutBatches = 0, stainedGlassBatches = 0, rigidVertices = 0, rigidTriangles = 0, triangles = 0;
    uint64_t bytes = 0;
    std::array<float, 3> low{}, high{};
};

struct CasterProgress {
    std::function<void(const std::string &)> report;
    const std::atomic<bool> *cancel = nullptr;
};

// Every .vwnod_c of `package` (ordinal path order), resolving models/materials/textures through
// `resources`. Per-asset problems become `warnings` entries; the asset is left out or kept static.
std::vector<uint8_t> BuildVisualCasters(const Vpk &package, const PackageSet &resources, CasterStats &stats,
                                        std::vector<std::string> &warnings, const CasterProgress &progress = {});

} // namespace Filmmaker::MapData
