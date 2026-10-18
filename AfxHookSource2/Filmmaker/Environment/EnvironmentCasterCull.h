#pragma once
#include "EnvironmentCasterBounds.h"
#include <map>
#include <atomic>

namespace Filmmaker::Environment {
inline std::atomic<bool> ShadowCasterCulling{true};
// Exact current-pose GPU culling. The CPU's delayed scheduling bounds must never
// decide which triangles are drawn: a fast move or a demo seek can outpace them.
// Retain whole triangle blocks and the original vertex buffer/coverage shader.
class CasterShadowCuller {
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    using Srv = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    using Uav = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    struct Entry {
        Buffer source, bounds, indices, arguments;
        Srv sourceView, boundsView;
        Uav boundsOutput, indexOutput, argumentOutput;
        ID3D11ShaderResourceView* countSource = nullptr;
        UINT count=0, stride=0, blocks=0;
        uint64_t generation=~0ull, used=0;
        uint64_t prepared=~0ull;
        UINT capacity=0, faceCount=0;
        std::array<Matrix,16> matrices{};
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> measure, cull;
    Buffer constants;
    std::map<ID3D11Buffer*,Entry> entries;
public:
    static constexpr UINT BatchFaces=16;
    bool Initialize(ID3D11Device*);
    // One dispatch handles a batch of faces, avoiding a compute/graphics transition per face.
    bool Prepare(ID3D11DeviceContext*,const CasterVertexStream&,const Matrix*,UINT faces,uint64_t generation);
    // Prepared faces only. false: caller issues the original draw, including on allocation failure.
    bool Draw(ID3D11DeviceContext*,const CasterVertexStream&,const Matrix&,uint64_t generation);
    void NextFrame(uint64_t generation);
};
}
