#pragma once
#include "RelightProbeMath.h"
#include <d3d11.h>
#include <atomic>
#include <memory>
namespace Filmmaker::Environment {struct OcclusionMesh;}
namespace Filmmaker::Relight {
struct ProbeBinding {
    ID3D11Buffer* constants=nullptr;
    ID3D11ShaderResourceView* irradiance=nullptr;
    ID3D11ShaderResourceView* sunOcclusion=nullptr;
    ProbePacking packing{};
    ID3D11SamplerState* sampler=nullptr;
    // Live reflection capture (TextureCubeArray, one cube) at reflectionOrigin, and a 16-byte
    // buffer holding its ambient normalization row. Null when unavailable.
    ID3D11ShaderResourceView* reflection=nullptr;
    ID3D11Buffer* reflectionNorm=nullptr;
    unsigned reflectionMips=0;
    Vec3 reflectionOrigin{};
    ID3D11ShaderResourceView* distance=nullptr; // directional first/second hit-distance moments
};
// Neutral binding establishes the native probe ABI before publishing real bounce.
ProbeBinding NeutralProbe(ID3D11Device* device,Vec3 low,Vec3 high);
// Live hemisphere radiance, with no baked input. Also used while traced GI converges.
ProbeBinding AmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,Vec3 sky,Vec3 ground);
// Independent ambient cube for the miniature skybox; never shares foreground GI history.
ProbeBinding SkyboxAmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,Vec3 sky,Vec3 ground);
void EndProbes();
ProbeBinding PrepareProbeGi(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<const Environment::OcclusionMesh>& world,const Scene& scene,
    const std::vector<LightRecord>& records,ID3D11ShaderResourceView* shadow,
    ID3D11SamplerState* comparison,uint64_t frame,unsigned residentStaticLights=0,Vec3 camera={},
    ID3D11ShaderResourceView* glassCookies=nullptr,Vec3 viewForward={});
// Diagnostic (mirv_relight maskdedupe): skip the atomic for visible cells already marked.
extern std::atomic<bool> ProbeMaskDedupe;
// Capture current visible cages for the next frame's prioritized transport pass.
// Material draws consume one immutable irradiance/distance/reflection snapshot.
void MarkVisibleProbeGi(ID3D11DeviceContext*,
    ID3D11ShaderResourceView* positions,UINT width,UINT height,uint64_t frame);
void EndProbeGi();
void EndScreenGi();
// Current-frame depth-derived positions only. Missing depth falls back to world probes.
ID3D11ShaderResourceView* PrepareScreenGi(ID3D11Device*,ID3D11DeviceContext*,const ProbeBinding&,
    ID3D11ShaderResourceView* positions,UINT width,UINT height,Vec3 camera,
    const Environment::Matrix& worldToClip,uint64_t frame);
}
