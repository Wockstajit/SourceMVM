#pragma once
#include "EnvironmentShadowMath.h"
#include <memory>
#include <filesystem>
namespace Filmmaker::Environment {
// Called under the scene mutex. When the map has no current cache, the native extractor
// (Filmmaker/MapData) builds it on an owned background worker first; loading also happens there,
// never in a render callback.
void UpdateMapOcclusion(const std::string &map, const std::filesystem::path &folder);
void ReloadMapOcclusion();
std::shared_ptr<const OcclusionMesh> MapOcclusion();
std::string MapOcclusionStatus();
bool MapOcclusionPreparing();
} // namespace Filmmaker::Environment
