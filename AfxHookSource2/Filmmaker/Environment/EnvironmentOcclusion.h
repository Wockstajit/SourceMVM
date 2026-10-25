#pragma once
#include "EnvironmentShadowMath.h"
#include <atomic>
#include <functional>
#include <memory>
#include <filesystem>
namespace Filmmaker::Environment {
// <AppData>\HLAE\source_mvm\environment\occlusion: every map's cache (EnvironmentScene.cpp).
std::filesystem::path MapDataFolder();
// Called under the scene mutex. When the map has no current cache, the native extractor
// (Filmmaker/MapData) builds it on an owned background worker first; loading also happens there,
// never in a render callback.
void UpdateMapOcclusion(const std::string &map, const std::filesystem::path &folder);
// No map data while `reason` holds (the alternative environment has moved the world off the
// coordinates its map data uses); the status line says why. UpdateMapOcclusion resumes.
void HoldMapOcclusion(const std::string &reason);
void ReloadMapOcclusion();
std::shared_ptr<const OcclusionMesh> MapOcclusion();
std::string MapOcclusionStatus();
bool MapOcclusionPreparing();
// Any map's collision, for a map that need not be the one loaded (the alternative environment's
// top-down pictures). Extracts the cache first when it is stale, on the caller's thread, one
// extraction at a time with the live loader so two never write the same cache. `addon`: the
// Workshop item to read the map from, when items or a loose package share its name.
bool ReadMapCollision(const std::string &map, const std::filesystem::path &folder, OcclusionMesh &out, std::string &error,
                      const std::atomic<bool> *stop, const std::function<void(const std::string &)> &progress,
                      const std::string &addon = {});
} // namespace Filmmaker::Environment
