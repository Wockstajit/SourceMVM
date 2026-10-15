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
inline bool CasterBoundsIntersect(const std::vector<CasterBounds>& bounds,const Matrix& matrix,bool complete) {
    if(!complete)return true; // An unavailable or malformed current pose must never lose its shadow.
    const ShadowFrustum frustum(matrix);
    for(const auto& b:bounds)if(!ValidCasterBounds(b)||frustum.Intersects(b.low,b.high))return true;
    return false;
}
}
