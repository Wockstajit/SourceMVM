#define NOMINMAX
#include "RagdollWeaponGround.h"
#include "../Environment/EnvironmentOcclusion.h"
#include "../Environment/EnvironmentSurfaceMath.h"
#include "../Environment/SurfaceRayGridMath.h"
#include "../Debug/MvmDebugLog.h"
#include <future>
#include <memory>
#include <chrono>

namespace Filmmaker {
namespace {
using namespace Environment;
// The map collision is the Environment's (native map data, extracted on first load of a map).
// The ray grid is built on a worker, because a per-frame caller fires one short probe per
// physics body per frame and the unaccelerated sweep walks the whole map.
std::future<std::shared_ptr<SurfaceRayGrid>> pending;
std::shared_ptr<const OcclusionMesh> geometry, building;
std::shared_ptr<const SurfaceRayGrid> geometryGrid;
std::string status="waiting for map data";

// A grid build that fails is not fatal: SurfaceRaycastGrid falls back to the full sweep.
const SurfaceRayGrid& ActiveGrid() {
    static const SurfaceRayGrid kNone;
    return geometryGrid ? *geometryGrid : kNone;
}
}
void RagdollWeaponGround_Pump() {
    if(pending.valid()) {
        if(pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
        auto grid=pending.get();
        if(building==MapOcclusion()) {
            geometry=building;
            geometryGrid=grid;
            status="ready (static map collision)";
            MvmDebugLog_LinefAlways("ragdoll.weapon.ground","triangles=%zu cells=%zu buckets=%zu status=%s",
                geometry->indices.size()/3,geometryGrid?geometryGrid->CellCount():0,
                geometryGrid?geometryGrid->triangles.size():0,status.c_str());
        }
        building.reset();
    }
    const auto mesh=MapOcclusion();
    if(mesh==geometry) return;
    geometry.reset();
    geometryGrid.reset();
    if(!mesh) { status="waiting for map data"; return; }
    status="building map collision grid";
    building=mesh;
    pending=std::async(std::launch::async,[mesh]() {
        auto grid=std::make_shared<SurfaceRayGrid>();
        return BuildSurfaceRayGrid(*mesh,*grid)?grid:nullptr;
    });
}
const char* RagdollWeaponGround_Status() { return status.c_str(); }
bool RagdollWeaponGround_Ray(const float origin[3], const float dir[3], float maxDist,
    float hitPos[3], float hitNormal[3]) {
    if(!geometry || !origin || !dir || !hitPos || !hitNormal || maxDist <= 0.f) return false;
    SurfaceHit hit;
    if(!SurfaceRaycastGrid(*geometry,ActiveGrid(),{origin[0],origin[1],origin[2]},{dir[0],dir[1],dir[2]},maxDist,hit))
        return false;
    hitPos[0]=hit.position.x; hitPos[1]=hit.position.y; hitPos[2]=hit.position.z;
    hitNormal[0]=hit.normal.x; hitNormal[1]=hit.normal.y; hitNormal[2]=hit.normal.z;
    return true;
}
bool RagdollWeaponGround_Find(const float p[3], const float q[4], float& z, RagdollWeaponQuat& rotation) {
    if(!geometry) return false;
    SurfaceHit hit;
    // Drop vertically, not toward the demo's recorded weapon landing position.
    // A small support clearance keeps the visual prop above the collision surface.
    if(!SurfaceRaycastGrid(*geometry,ActiveGrid(),{p[0],p[1],p[2]+2},{0,0,-1},4096,hit) || hit.normal.z<0.5f) return false;
    z=(std::min)(p[2],hit.position.z+3.0f);
    rotation=RagdollWeaponGroundRotation({q[0],q[1],q[2],q[3]},hit.normal.x,hit.normal.y,hit.normal.z);
    return true;
}
}
