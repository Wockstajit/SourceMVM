#define NOMINMAX
#include "EnvironmentOcclusion.h"
#include "EnvironmentCacheMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../MapData/MapCacheCodec.h"
#include "../MapData/MapExtract.h"
#include "../MapData/MapCasterLimits.h"
#include "../MapData/MapSources.h"
#include "../Platform/JsonParser.h"
#include <atomic>
#include <future>
#include <fstream>
#include <mutex>
#include <Windows.h>
namespace Filmmaker::Environment {
namespace {
struct Result {
    std::string map, error;
    uint64_t generation=0;
    std::shared_ptr<OcclusionMesh> mesh;
};
std::future<Result> pending;
std::atomic<bool> cancel{false};
uint64_t generation=1;
std::shared_ptr<const OcclusionMesh> current;
std::string requested, attempted;
std::mutex statusLock;
std::string status = "Map data not loaded";
void SetStatus(std::string text) {
    std::lock_guard<std::mutex> lock(statusLock);
    status = std::move(text);
}
// <root>/game/bin/win64/cs2.exe -> <root>
std::filesystem::path Cs2Root() {
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return {};
    return std::filesystem::path(executable).parent_path().parent_path().parent_path().parent_path();
}
// Extraction is background work: never compete with the game's frame threads.
struct BackgroundPriority {
    HANDLE thread = GetCurrentThread();
    int previous = GetThreadPriority(thread);
    BackgroundPriority() { SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL); }
    ~BackgroundPriority() { SetThreadPriority(thread, previous); }
};
Result Load(const std::string &map, const std::filesystem::path &folder, uint64_t requestGeneration) {
    Result r;
    r.map = map;
    r.generation = requestGeneration;
    const auto manifestPath = folder / (map + ".json");
    if (!MapData::CacheCurrent(manifestPath, map)) {
        BackgroundPriority background;
        const auto root = Cs2Root();
        MvmDebugLog_LinefAlways("environment.occlusion", "extract map=%s root=%s", map.c_str(), root.u8string().c_str());
        MapData::ExtractOptions options;
        options.cancel = &cancel;
        options.progress = [](const std::string &text) { SetStatus("Preparing map data: " + text); };
        const auto start = GetTickCount64();
        const auto summary = MapData::ExtractMap(root, map, folder, options);
        MapData::PruneCache(folder);
        MvmDebugLog_LinefAlways("environment.occlusion",
            "extracted map=%s ms=%llu lights=%zu environment=%zu collision=%zu render=%zu batches=%zu cutout=%zu glass=%zu skybox=%d warnings=%zu",
            map.c_str(), GetTickCount64() - start, summary.lights, summary.environment, summary.collisionTriangles, summary.visual.triangles,
            summary.visual.batches, summary.visual.cutoutBatches, summary.visual.stainedGlassBatches, summary.skybox ? 1 : 0, summary.warnings.size());
        for (const auto &warning : summary.warnings) MvmDebugLog_LinefAlways("environment.occlusion", "extract warning: %s", warning.c_str());
    }
    SetStatus("Loading map data");
    std::string json;
    JsonValue metadata;
    if (!ReadCacheManifest(manifestPath, json) || !JsonParse(json, metadata)) {
        r.error = "map data manifest missing or invalid";
        return r;
    }
    auto value = [&](const char *key) { auto v = metadata.Find(key); return v ? v->AsString() : std::string(); };
    std::filesystem::path physicsPath, visualPath, lightPath;
    if (!CachePayloadValid(folder, value("physicsDigest"), ".mvmocclusion", 512ull * 1024 * 1024, physicsPath) ||
        !CachePayloadValid(folder, value("visualDigest"), ".mvmcasters", MapData::kVisualCasterFileLimit, visualPath) ||
        !CachePayloadValid(folder, value("lightsDigest"), ".lights.json", 32ull * 1024 * 1024, lightPath)) {
        r.error = "corrupt or interrupted map data payload";
        return r;
    }
    const auto file = MapData::OpenCachePayload(physicsPath, r.error);
    const auto visualFile = file ? MapData::OpenCachePayload(visualPath, r.error) : nullptr;
    if (!visualFile) return r;
    auto mesh = std::make_shared<OcclusionMesh>();
    auto visual = std::make_shared<VisualCasters>();
    if (!ReadOcclusionMesh(*file, *mesh, r.error) || !ReadVisualCasters(*visualFile, *visual, r.error)) return r;
    mesh->visual = visual;
    mesh->contentKey = value("mapDigest") + value("physicsDigest") + value("visualDigest");
    mesh->lightMetadataPath = lightPath.u8string();
    if (const auto skyboxes = metadata.Find("skyboxes"); skyboxes && skyboxes->type == JsonValue::Type::Array && !skyboxes->arr.empty()) {
        const auto &sky = skyboxes->arr[0];
        auto skyValue = [&](const char *key) { const auto v = sky.Find(key); return v ? v->AsString() : std::string(); };
        std::filesystem::path skyPath;
        if (!CachePayloadValid(folder, skyValue("visualDigest"), ".mvmcasters", MapData::kVisualCasterFileLimit, skyPath)) {
            r.error = "corrupt skybox map data payload";
            return r;
        }
        const auto skyFile = MapData::OpenCachePayload(skyPath, r.error);
        if (!skyFile) return r;
        auto casters = std::make_shared<VisualCasters>();
        if (!ReadVisualCasters(*skyFile, *casters, r.error)) return r;
        const auto scale = sky.Find("cameraScale");
        const auto origin = sky.Find("cameraOrigin");
        const float skyScale = scale ? float(scale->AsNumber()) : 0.f;
        if (!std::isfinite(skyScale) || skyScale < 1 || skyScale > 1024 || !origin || origin->type != JsonValue::Type::Array || origin->arr.size() != 3) {
            r.error = "invalid skybox camera";
            return r;
        }
        Vec3 p{float(origin->arr[0].AsNumber(NAN)), float(origin->arr[1].AsNumber(NAN)), float(origin->arr[2].AsNumber(NAN))};
        if (!Finite(p)) {
            r.error = "invalid skybox camera origin";
            return r;
        }
        auto world = std::make_shared<OcclusionMesh>();
        world->visual = casters;
        world->low = casters->low;
        world->high = casters->high;
        world->contentKey = skyValue("visualDigest");
        world->skyboxScale = skyScale;
        world->skyboxOrigin = p;
        mesh->skybox = world;
        mesh->contentKey += world->contentKey;
    }
    r.mesh = mesh;
    return r;
}
} // namespace
void UpdateMapOcclusion(const std::string &map, const std::filesystem::path &folder) {
    if (map != requested) {
        requested = map;
        ++generation;
        attempted.clear();
        current.reset();
        SetStatus("Map data not loaded");
        if (pending.valid()) cancel = true;
    }
    if (pending.valid()) {
        if (pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto result = pending.get();
        cancel = false;
        if (result.map == requested && result.generation == generation) {
            if (result.mesh) current = result.mesh;
            std::string text = result.mesh ? "Map data ready: geometry, foliage and authored lights" : "Map data unavailable: " + result.error;
            if (current && !result.mesh) text += "; retaining previous geometry";
            SetStatus(text);
            MvmDebugLog_LinefAlways("environment.occlusion", "map=%s triangles=%zu status=%s", map.c_str(),
                                    current ? current->indices.size() / 3 : 0, text.c_str());
        }
    }
    if (attempted == map || !MapData::ValidMapName(map) || map.rfind("ui_", 0) == 0)
        return;
    attempted = map;
    SetStatus("Loading map data");
    MvmDebugLog_LinefAlways("environment.occlusion", "request map=%s folder=%s", map.c_str(), folder.u8string().c_str());
    const auto requestGeneration = generation;
    pending = std::async(std::launch::async, [map, folder, requestGeneration]() {
        try {
            return Load(map, folder, requestGeneration);
        } catch (const std::exception &e) {
            Result r;
            r.map = map;
            r.generation = requestGeneration;
            r.error = e.what();
            return r;
        }
    });
}
void ReloadMapOcclusion() {
    ++generation;
    attempted.clear();
    // Publish only a successfully loaded replacement; keep current resources usable.
}
std::shared_ptr<const OcclusionMesh> MapOcclusion() { return current; }
std::string MapOcclusionStatus() {
    std::lock_guard<std::mutex> lock(statusLock);
    return status;
}
bool MapOcclusionPreparing() { return pending.valid(); }
} // namespace Filmmaker::Environment
