#pragma once

// One map's lighting cache: authored lights/environment (.lights.json), opaque collision
// (.mvmocclusion), visual casters (.mvmcasters) and the 3D-skybox casters, written as
// digest-named payloads plus <map>.json. The binary payloads are compressed (MapCacheCodec.h);
// a payload's digest is the SHA-256 of the file as stored. The manifest lists every game file the extraction
// read with its size and write time; the cache stays valid while all of them are unchanged.

#include "MapCasters.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

// 4: binary payloads compressed (MapCacheCodec.h).
constexpr int kManifestFormat = 4;
// Bump whenever extraction output changes so existing caches are rebuilt.
constexpr int kExtractorVersion = 2;

struct ExtractOptions {
    std::function<void(const std::string &)> progress;
    const std::atomic<bool> *cancel = nullptr;
};

struct ExtractSummary {
    std::filesystem::path manifest;
    size_t lights = 0, environment = 0, collisionTriangles = 0;
    CasterStats visual;
    bool skybox = false;
    std::vector<std::string> warnings;
};

// Extracts `map` from the CS2 install into `outDir`. Throws MapDataError on failure; nothing is
// published unless every payload was produced.
ExtractSummary ExtractMap(const std::filesystem::path &cs2Root, const std::string &map, const std::filesystem::path &outDir,
                          const ExtractOptions &options = {});

// True when `<outDir>/<map>.json` is a current-format manifest whose recorded source files
// all still have their recorded size and write time.
bool CacheCurrent(const std::filesystem::path &manifest, const std::string &map);

// Removes digest-named payloads in `outDir` that no manifest references, and manifests of an
// older format or extractor version.
void PruneCache(const std::filesystem::path &outDir);

} // namespace Filmmaker::MapData
