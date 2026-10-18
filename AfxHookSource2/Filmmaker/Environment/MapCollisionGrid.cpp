#define NOMINMAX
#include "MapCollisionGrid.h"
#include "EnvironmentScene.h"
#include "SurfaceRayGridMath.h"
#include "../Debug/MvmDebugLog.h"
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace Filmmaker::Environment {
namespace {
std::future<std::shared_ptr<SurfaceRayGrid>> g_pending;
std::shared_ptr<const OcclusionMesh> g_building, g_geometry; // g_geometry: the mesh g_grid was built for
std::shared_ptr<const SurfaceRayGrid> g_grid;
std::string g_status = "waiting for map data";
} // namespace

void MapCollisionGrid_Pump() {
	if (g_pending.valid()) {
		if (g_pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
		auto grid = g_pending.get();
		if (g_building == CollisionGeometrySnapshot()) {
			g_geometry = g_building;
			g_grid = grid;
			g_status = grid ? "ready (static map collision)" : "grid build failed: full sweep";
			MvmDebugLog_LinefAlways("map.collision.grid", "triangles=%zu cells=%zu buckets=%zu status=%s",
				g_geometry->indices.size() / 3, g_grid ? g_grid->CellCount() : 0,
				g_grid ? g_grid->triangles.size() : 0, g_status.c_str());
		}
		g_building.reset();
	}
	const auto mesh = CollisionGeometrySnapshot();
	if (mesh == g_geometry) return;
	g_geometry.reset();
	g_grid.reset();
	if (!mesh) { g_status = "waiting for map data"; return; }
	g_status = "building map collision grid";
	g_building = mesh;
	g_pending = std::async(std::launch::async, [mesh]() {
		auto grid = std::make_shared<SurfaceRayGrid>();
		return BuildSurfaceRayGrid(*mesh, *grid) ? grid : nullptr;
	});
}

const char* MapCollisionGrid_Status() { return g_status.c_str(); }
bool MapCollisionGrid_Ready() { return g_geometry && g_grid; }

bool MapCollisionRaycast(Vec3 origin, Vec3 direction, float range, SurfaceHit& hit) {
	if (!(range > 0.f)) return false;
	if (g_geometry && g_grid) return SurfaceRaycastGrid(*g_geometry, *g_grid, origin, direction, range, hit);
	// The grid is still building (or failed): the full sweep, as before.
	const auto mesh = CollisionGeometrySnapshot();
	return mesh && SurfaceRaycast(*mesh, origin, direction, range, hit);
}

} // namespace Filmmaker::Environment
