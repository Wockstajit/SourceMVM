#define NOMINMAX
#include "RagdollWeaponGround.h"
#include "../Environment/MapCollisionGrid.h"

namespace Filmmaker {
using namespace Environment;
// The map collision and its ray grid are shared (Environment/MapCollisionGrid.h). A per-frame
// caller fires one short probe per physics body per frame, so these probes wait for the grid
// instead of falling back to the full sweep while it builds.
bool RagdollWeaponGround_Ray(const float origin[3], const float dir[3], float maxDist,
    float hitPos[3], float hitNormal[3]) {
    if(!MapCollisionGrid_Ready() || !origin || !dir || !hitPos || !hitNormal || maxDist <= 0.f) return false;
    SurfaceHit hit;
    if(!MapCollisionRaycast({origin[0],origin[1],origin[2]},{dir[0],dir[1],dir[2]},maxDist,hit))
        return false;
    hitPos[0]=hit.position.x; hitPos[1]=hit.position.y; hitPos[2]=hit.position.z;
    hitNormal[0]=hit.normal.x; hitNormal[1]=hit.normal.y; hitNormal[2]=hit.normal.z;
    return true;
}
bool RagdollWeaponGround_Find(const float p[3], const float q[4], float& z, RagdollWeaponQuat& rotation) {
    if(!MapCollisionGrid_Ready()) return false;
    SurfaceHit hit;
    // Drop vertically, not toward the demo's recorded weapon landing position.
    // A small support clearance keeps the visual prop above the collision surface.
    if(!MapCollisionRaycast({p[0],p[1],p[2]+2},{0,0,-1},4096,hit) || hit.normal.z<0.5f) return false;
    z=(std::min)(p[2],hit.position.z+3.0f);
    rotation=RagdollWeaponGroundRotation({q[0],q[1],q[2],q[3]},hit.normal.x,hit.normal.y,hit.normal.z);
    return true;
}
}
