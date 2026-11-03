#pragma once
#include <array>
namespace Filmmaker::Lens {
inline constexpr float kFisheyeK1=.22f,kFisheyeK2=.10f;
// Same corner-anchored radial mapping as Shaders/afx_fisheye_uv.hlsli.
inline std::array<float,2> FisheyeUv(float u,float v,float aspect,float strength=1.f) {
    float x=(u-.5f)*aspect,y=v-.5f;
    const float r2=x*x+y*y,corner=.25f*(aspect*aspect+1);
    const float scale=(1+r2*strength*(kFisheyeK1+kFisheyeK2*r2))
        /(1+corner*strength*(kFisheyeK1+kFisheyeK2*corner));
    return {x*scale/aspect+.5f,y*scale+.5f};
}
inline std::array<float,4> FisheyeParameters(unsigned width,unsigned height,float strength=1.f) {
    return {kFisheyeK1*strength,kFisheyeK2*strength,height ? (float)width/height : 1.f,0};
}
} // namespace Filmmaker::Lens
