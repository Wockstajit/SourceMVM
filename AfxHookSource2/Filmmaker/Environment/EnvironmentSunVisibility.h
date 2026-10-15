#pragma once
#include "EnvironmentMath.h"
#include <d3d11.h>

namespace Filmmaker::Environment {
void BeginSunVisibility(ID3D11Device*);
void EndSunVisibility();
void NextSunVisibilityFrame();
// What the pass reads: the four sun cascades as they sit in the shadow atlas.
struct SunVisibilityInput {
    const Matrix* cascades=nullptr;          // 4 world->clip matrices, finest first (native loop order)
    const float* rects=nullptr;              // 4 x (offsetU, offsetV, scaleU, scaleV): uv = clip.xy*scale+offset
    ID3D11ShaderResourceView* atlas=nullptr; // R32_FLOAT shadow atlas
    float atlasWidth=0,atlasHeight=0,tileSize=0;
    Vec3 camera{};
    const Matrix* clipToWorld=nullptr;
    float tanHalfAngle=0;                    // tan(sun angular diameter / 2); 0 is a point sun
    float bias=0;                            // normalized depth bias
    float maxSearchDistance=8192;            // farthest blocker the search looks for, world units
};
// Per-pixel sun visibility for the main world pass: RGB is visibility, alpha is shared-footprint
// validity. Ambiguous MSAA receivers keep the native filter unless all covered samples are shadowed.
// Evaluated once per frame after the depth prepass; null keeps the native filter for this draw.
ID3D11ShaderResourceView* SunVisibility(ID3D11DeviceContext* immediate,const D3D11_VIEWPORT&,const SunVisibilityInput&);
} // namespace Filmmaker::Environment
