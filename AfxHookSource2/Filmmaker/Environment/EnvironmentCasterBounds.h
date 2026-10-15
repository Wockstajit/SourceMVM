#pragma once
#include "EnvironmentCasterBoundsMath.h"
#include <d3d11.h>
#include <wrl/client.h>
namespace Filmmaker::Environment {
struct CasterVertexStream {ID3D11Buffer* buffer; UINT count,stride;ID3D11ShaderResourceView* gpuCount=nullptr;};
// World units the late (non-blocking) bounds are grown by: covers a caster running for the two
// frames the readback lags, at any playable frame rate, plus animation reach.
constexpr float LateCasterBoundsPadding=64.f;
// Reads GPU caster positions into per-block bounds.
class CasterBoundsReader {
    static constexpr unsigned Slots=3;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants,output,readback[Slots];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> view;
    UINT slotBlocks[Slots]{};bool slotFilled[Slots]{};unsigned nextSlot=0;
public:
    bool Initialize(ID3D11Device*);
    // waitForGpu: this frame's exact bounds (stalls on the GPU; exports).
    // Otherwise the bounds of the read issued two calls earlier, grown by LateCasterBoundsPadding:
    // a same-frame read never completes without a stall, so demanding current bounds made every
    // live read fail and every shadow face count as dynamic -- players' shadows then refreshed
    // only when the face's turn came up in the background budget (the jitter report, 2026-10-13).
    // Returns false when no completed read is available yet; callers then keep every caster.
    bool Read(ID3D11DeviceContext*,const CasterVertexStream*,UINT,std::vector<CasterBounds>&,bool waitForGpu=true);
};
}
