#pragma once
#include "EnvironmentMath.h"
#include <algorithm>
#include <cmath>

namespace Filmmaker::Environment {
// The depth-derived horizon term is bounded in world units. Tangent-plane
// samples and surfaces beyond the radius cannot darken an open surface.
inline float AmbientHorizon(Vec3 normal,Vec3 delta,float radius) {
    const float d2=Dot(delta,delta);
    if(d2<1e-8f||radius<=0||d2>=radius*radius)return 0;
    return (std::max)(0.f,Dot(normal,delta)/std::sqrt(d2)-.03f)*(1-d2/(radius*radius));
}
inline bool AmbientViewport(float width,float height,float minimum,float maximum) {
    return width>=32&&height>=32&&minimum>=0&&minimum<.01f&&maximum>=.9f&&maximum<=1;
}
} // namespace Filmmaker::Environment
