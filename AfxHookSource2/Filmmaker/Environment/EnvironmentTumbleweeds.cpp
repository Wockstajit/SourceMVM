#define NOMINMAX
#include "EnvironmentTumbleweeds.h"
#include "MapCollisionGrid.h"
#include "SurfaceRayGridMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../../hlaeFolder.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <atomic>
#include <mutex>

namespace Filmmaker::Environment {
namespace {

struct MeshLoadResult {
    std::shared_ptr<const Mesh> mesh;
    std::string status;
};

MeshLoadResult LoadTumbleweedAsset() {
    MeshLoadResult out;
    const std::filesystem::path path =
        std::filesystem::path(GetHlaeFolderW()) / L"resources" / L"environment" / L"tumbleweed" / L"tumbleweed.glb";
    auto mesh = std::make_shared<Mesh>();
    std::string error;
    if (!LoadGltfMesh(path, *mesh, error)) {
        out.status = "tumbleweed: " + path.filename().string() + " " + error;
        return out;
    }
    out.mesh = mesh;
    out.status = "tumbleweed " + std::to_string(mesh->vertices.size() / 3) + " tris";
    return out;
}

struct State {
    bool loadStarted = false, loadDone = false;
    std::future<MeshLoadResult> pending;
    std::shared_ptr<const Mesh> mesh;
    TumbleweedSim sim;
    const OcclusionMesh *world = nullptr;
    std::string meshStatus = "tumbleweed not loaded";
    int activeCount = 0;
} g_state;

std::atomic<bool> g_filmmakerCamera{false};

std::mutex g_statusLock;
std::string g_status = "tumbleweed not loaded";

void SetStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    g_status = g_state.meshStatus + " (active=" + std::to_string(g_state.activeCount) + ")";
}

} // namespace

TumbleweedFrame PrepareTumbleweeds(const Frame &frame, const Climate &climate, const Matrix &worldToClip) {
    TumbleweedFrame out;
    auto &s = g_state;
    const int target = climate[CL_enabled] > 0 ? int(std::lround(climate[CL_tumbleweeds])) : 0;
    if (target <= 0) {
        if (s.activeCount > 0 || s.sim.initialized) {
            TumbleweedStepSim(s.sim, frame.time, 0, {}, {}, {}, {}, {}, nullptr, {});
            s.activeCount = 0;
            SetStatus();
        }
        return out;
    }

    // Start one background load of the GLB the first time tumbleweeds are switched on.
    if (!s.loadStarted) {
        s.loadStarted = true;
        s.meshStatus = "tumbleweed loading";
        SetStatus();
        s.pending = std::async(std::launch::async, LoadTumbleweedAsset);
    }
    if (!s.loadDone && s.pending.valid() && s.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        MeshLoadResult loaded = s.pending.get();
        s.loadDone = true;
        s.mesh = std::move(loaded.mesh);
        s.meshStatus = loaded.status;
        SetStatus();
        MvmDebugLog_LinefAlways("environment.tumbleweeds", "%s", s.meshStatus.c_str());
    }
    if (!s.mesh) return out;

    // Reset simulation when changing maps so tumbleweeds never carry across levels.
    if (s.world != frame.occlusion.get()) {
        s.world = frame.occlusion.get();
        s.sim = TumbleweedSim{};
    }

    // Use the fast uniform-grid raycaster once ready; skip stepping until the grid matches this map
    // so we never brute-force hundreds of thousands of triangles on the render thread.
    const MapCollisionSnapshot snap = MapCollisionGrid_Snapshot();
    const bool hasGrid = snap.geometry && snap.grid && (!frame.occlusion || snap.geometry == frame.occlusion);
    TumbleweedRaycast raycast;
    if (hasGrid) {
        raycast = [&snap](Vec3 origin, Vec3 direction, float range, TumbleweedRayHit &hit) {
            SurfaceHit sh;
            if (!SurfaceRaycastGrid(*snap.geometry, *snap.grid, origin, direction, range, sh)) return false;
            hit = {sh.position, sh.normal, sh.distance};
            return true;
        };
    }

    TumbleweedCamera cam;
    cam.position = frame.camera;
    cam.worldToClip = worldToClip;
    cam.valid = true;
    // FIRST PERSON: none of our cameras owns the view and the eye is a live player's (standing or
    // crouched, within a few units of the head over its feet). Then new tumbleweeds come out in
    // front of the player; for any other camera they may come from any side.
    if (!g_filmmakerCamera.load(std::memory_order_relaxed))
        for (const auto &a : frame.actors) {
            const float dx = frame.camera.x - a.origin.x, dy = frame.camera.y - a.origin.y, up = frame.camera.z - a.origin.z;
            if (dx * dx + dy * dy < 20.f * 20.f && up > 30.f && up < 76.f) { cam.pov = true; break; }
        }

    TumbleweedWind wind{frame.scene.weather.windX, frame.scene.weather.windY, climate[CL_gusts]};

    std::vector<TumbleweedActor> actors;
    actors.reserve(frame.actors.size());
    for (const auto &a : frame.actors) actors.push_back({a.origin, (std::max)(a.radius * .75f, 16.f)});

    static const std::vector<Vec3> kEmptyNav;
    const std::vector<Vec3> &nav = frame.occlusion ? frame.occlusion->navPositions : kEmptyNav;
    const Vec3 mapLow = frame.occlusion ? frame.occlusion->low : Vec3{-2048, -2048, -256};
    const Vec3 mapHigh = frame.occlusion ? frame.occlusion->high : Vec3{2048, 2048, 512};

    // Wait for collision grid on a prepared map so tumbleweeds spawn cleanly on real ground & interact
    // with walls from their very first frame.
    if (frame.occlusion && !hasGrid) return out;

    TumbleweedStepSim(s.sim, frame.time, target, wind, cam, nav, mapLow, mapHigh, raycast, actors);

    out.mesh = s.mesh;
    out.items.reserve(kTumbleweedMax);
    for (const auto &b : s.sim.bodies) {
        if (!b.active) continue;
        TumbleweedInstance inst;
        inst.position = b.pos;
        inst.radius = b.radius;
        inst.model = TumbleweedModelMatrix(b.pos, b.orientation, b.radius);
        out.items.push_back(inst);
    }
    if (int(out.items.size()) != s.activeCount) {
        s.activeCount = int(out.items.size());
        SetStatus();
    }
    if (MvmDebugLog_Active()) {
        static double lastLog = -1e9;
        if (std::abs(frame.time - lastLog) > 2.5) {
            lastLog = frame.time;
            const Vec3 firstPos = out.items.empty() ? Vec3{} : out.items[0].position;
            MvmDebugLog_Linef("environment.tumbleweeds",
                              "pov=%d target=%d active=%zu grid=%d nav=%zu wind=(%.0f %.0f) gusts=%.2f first=(%.0f %.0f %.0f)",
                              int(cam.pov), target, out.items.size(), int(hasGrid), nav.size(), wind.windX, wind.windY, wind.gusts,
                              firstPos.x, firstPos.y, firstPos.z);
        }
    }
    return out;
}

void Tumbleweeds_SetFilmmakerCamera(bool owned) { g_filmmakerCamera.store(owned, std::memory_order_relaxed); }

std::string TumbleweedsStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    return g_status;
}

void ReleaseTumbleweeds() {
    auto &s = g_state;
    if (s.pending.valid()) s.pending.wait();
    s = State{};
    SetStatus();
}

} // namespace Filmmaker::Environment
