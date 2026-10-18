#pragma once
#include "EnvironmentShadowSpatialMath.h"
#include <cmath>
namespace Filmmaker::Environment {
struct CasterBounds { Vec3 low; float valid; Vec3 high; float reserved; };
inline bool ValidCasterBounds(const CasterBounds& b) {
    return b.valid==1 && std::isfinite(b.low.x)&&std::isfinite(b.low.y)&&std::isfinite(b.low.z)&&
        std::isfinite(b.high.x)&&std::isfinite(b.high.y)&&std::isfinite(b.high.z)&&
        b.low.x<=b.high.x&&b.low.y<=b.high.y&&b.low.z<=b.high.z;
}
// With reach > 0, a caster also has to reach into the cube of that half size around `light`: a
// lamp's shadow lookups stop past its shadow reach along the face axis (EnvironmentNativeShadowMath
// .h), and a caster outside that cube only shadows receivers further out, which never look.
inline bool CasterBoundsIntersect(const std::vector<CasterBounds>& bounds,const Matrix& matrix,bool complete,
                                  Vec3 light={},float reach=0) {
    if(!complete)return true; // An unavailable or malformed current pose must never lose its shadow.
    const ShadowFrustum frustum(matrix);
    for(const auto& b:bounds){
        if(!ValidCasterBounds(b))return true;
        if(reach>0&&(b.high.x<light.x-reach||b.low.x>light.x+reach||b.high.y<light.y-reach||b.low.y>light.y+reach||
                     b.high.z<light.z-reach||b.low.z>light.z+reach))continue;
        if(frustum.Intersects(b.low,b.high))return true;
    }
    return false;
}
}
