#pragma once
#include "RelightStainedGlassMath.h"
#include <d3d11.h>
#include <memory>
namespace Filmmaker::Environment {struct VisualCasters;}
namespace Filmmaker::Relight {
// Sunlight through the map's stained glass (RelightStainedGlassMath.h). Renders every window's
// cookie for the current sun and returns one beam record per window whose glass the sun reaches,
// plus the cookie volume those records select by radiance.w. Immediate render thread only.
struct GlassBeams {
    std::vector<LightRecord> records;
    ID3D11ShaderResourceView* cookies=nullptr;
};
// `sun` is the map's sun record (whole-map shadow projection and atlas rect) used to test whether
// sunlight reaches the outside of the glass; missing visibility data produces no beams.
GlassBeams PrepareStainedGlass(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<const Environment::VisualCasters>& visual,const LightRecord* sun,
    ID3D11ShaderResourceView* atlas,ID3D11SamplerState* comparison,Vec3 towardSun,Vec3 radiance,uint64_t frame);
void EndStainedGlass();
// Main thread: remove the map's baked self illumination only while lighting is owned.
void UpdateStainedGlassMaterials(bool owned);
void ResetStainedGlassMaterials();
}
