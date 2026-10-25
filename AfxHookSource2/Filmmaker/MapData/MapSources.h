#pragma once

// Locating a map's packages: the CS2 install, official map packages and Workshop addons
// (which nest the map as a `maps/<name>.vpk` entry), plus the shared game packages that
// models, materials and textures resolve from.

#include "MapDataIo.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Filmmaker::MapData {

struct MapSources {
    std::filesystem::path cs2Root;
    std::string map;
    std::shared_ptr<Vpk> package;  // the map package itself
    std::shared_ptr<Vpk> addon;    // the Workshop addon that nests it, or null
    PackageSet resources;          // map, Workshop/community assets, then gameinfo.gi Game packages
    // Every physical file the extraction depends on; the cache is valid while all of them keep
    // their size and modification time.
    std::vector<std::filesystem::path> files;
    std::string mapDigest; // SHA-256 of the map package bytes
};

// The CS2 install root (the folder holding game/), from Steam's registry entry and
// libraryfolders.vdf. Empty when not found.
std::filesystem::path ResolveCs2Root();

// Official game/csgo/maps/<map>.vpk first, then the newest installed Workshop addon that
// carries maps/<map>.vpk. With `addon` (a Workshop item id) only that item is read: two items, or
// an item and a loose package, can hold a map of the same name. Throws MapDataError when none does.
MapSources OpenMapSources(const std::filesystem::path &cs2Root, const std::string &map, const std::string &addon = {});

// A package referenced by the map (`maps/....vpk`, e.g. a 3D skybox prefab): nested in the
// mounted resource packages first, then a file under game/csgo. Null when missing. Adds read
// files to `sources`.
std::shared_ptr<Vpk> OpenReferencedPackage(MapSources &sources, const std::string &package);

bool ValidMapName(const std::string &map);

} // namespace Filmmaker::MapData
