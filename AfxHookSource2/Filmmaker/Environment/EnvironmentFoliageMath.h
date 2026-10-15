#pragma once
#include "EnvironmentCasterMath.h"
#include <cmath>
#include <limits>
namespace Filmmaker::Environment {
// Conservative displacement for normalized noise samples and native weights
// clamped to [.001,1]. Independent of time: valid for every phase of this wind.
inline float FoliageDisplacementRadius(const CasterAnimation& a,Vec3 wind,float strength) {
    for(float x:a.sway)if(!std::isfinite(x))return std::numeric_limits<float>::infinity();
    for(float x:a.flutter)if(!std::isfinite(x))return std::numeric_limits<float>::infinity();
    auto weight=[](float exponent){return std::pow(exponent<0?.001f:1.f,exponent);};
    const float sway=150*std::abs(a.sway[0]*strength)*weight(a.sway[3]);
    const float flutter=25*std::abs(a.flutter[0]*strength)*weight(a.flutter[3]);
    const float x=(.5f+std::abs(wind.x))*(sway+flutter);
    const float y=(.5f+std::abs(wind.y))*(sway+flutter);
    const float z=(.25f+std::abs(wind.z))*(sway+flutter)+.314f*std::sqrt(x*x+y*y);
    const float radius=(std::max)({x,y,z});
    if(!std::isfinite(radius)||!std::isfinite(wind.x)||!std::isfinite(wind.y)||!std::isfinite(wind.z)||!std::isfinite(strength))
        return std::numeric_limits<float>::infinity();
    return radius+.001f; // Preserve a floating-point margin at the bound.
}
}
