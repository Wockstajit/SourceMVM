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
    // World units along its normal that a receiver reads the volume from (the plain trilinear
    // query only; the distance-aware one has its own bias). Zero for a volume that needs none.
    float normalBias=0;
};
// Neutral binding establishes the native probe ABI before publishing real bounce.
ProbeBinding NeutralProbe(ID3D11Device* device,Vec3 low,Vec3 high);
// One live ambient cube for the whole map, with no baked input (AmbientFill). Also used while traced GI converges.
ProbeBinding AmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,const AmbientCube& cube);
// Independent ambient cube for the miniature skybox; never shares foreground GI history.
ProbeBinding SkyboxAmbientProbe(ID3D11Device* device,Vec3 low,Vec3 high,const AmbientCube& cube);
// Lite lighting: adds the map-wide sky-over-ground reflection cube (LiteReflectionRadiance) to an
// ambient binding, in the fields the traced capture fills. Leaves the binding as it was on failure.
void AddLiteReflection(ID3D11Device* device,const Scene& scene,Vec3 camera,ProbeBinding& binding);
void EndProbes();
ProbeBinding PrepareProbeGi(ID3D11Device* device,ID3D11DeviceContext* immediate,
    const std::shared_ptr<const Environment::OcclusionMesh>& world,const Scene& scene,
    const std::vector<LightRecord>& records,ID3D11ShaderResourceView* shadow,
    ID3D11SamplerState* comparison,uint64_t frame,unsigned residentStaticLights=0,Vec3 camera={},
    ID3D11ShaderResourceView* glassCookies=nullptr,Vec3 viewForward={});
// Diagnostic (mirv_relight maskdedupe): skip the atomic for visible cells already marked.
extern std::atomic<bool> ProbeMaskDedupe;
// Diagnostic (mirv_relight gicarry): carry the probe history with a compute copy, or CopyResource.
extern std::atomic<bool> ProbeHistoryCarry;
// Diagnostic (mirv_relight dumpprobes): nonzero writes the probe volumes to a file on the next update.
extern std::atomic<unsigned> ProbeDumpRequests;
// Diagnostic (mirv_relight twosided): a voxel holding faces on opposite sides gives a probe no
// back-face evidence (1), or every voxel is judged by the one normal it kept (0).
extern std::atomic<bool> ProbeTwoSidedVoxels;
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
