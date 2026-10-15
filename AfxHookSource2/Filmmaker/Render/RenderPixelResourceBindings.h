#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace Filmmaker {
// Snapshot before any replacement, then bind/restore adjacent slots together.
// Gaps retain the engine's resources; duplicate slots use the last replacement.
struct RenderPixelResourceBindings {
    static constexpr unsigned Capacity = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    std::array<bool, Capacity> bound{};
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Capacity> previous{};

    template<class F> void Ranges(F action) {
        for (unsigned slot=0; slot<Capacity;) {
            if (!bound[slot]) { ++slot; continue; }
            const auto start=slot++;
            while (slot<Capacity && bound[slot]) ++slot;
            action(start,slot-start);
        }
    }
    void Bind(ID3D11DeviceContext* context, unsigned count, const UINT* slots,
              ID3D11ShaderResourceView* const* replacements) {
        Restore(context);
        std::array<ID3D11ShaderResourceView*, Capacity> views{};
        for (unsigned i=0; i<count; ++i) if (slots[i]<Capacity) {
            bound[slots[i]]=true; views[slots[i]]=replacements[i];
        }
        Ranges([&](unsigned start,unsigned length) {
            std::array<ID3D11ShaderResourceView*, Capacity> saved{};
            context->PSGetShaderResources(start,length,saved.data());
            for (unsigned i=0; i<length; ++i) previous[start+i].Attach(saved[i]);
            context->PSSetShaderResources(start,length,views.data()+start);
        });
    }
    void Restore(ID3D11DeviceContext* context) {
        Ranges([&](unsigned start,unsigned length) {
            std::array<ID3D11ShaderResourceView*, Capacity> views{};
            for (unsigned i=0; i<length; ++i) views[i]=previous[start+i].Get();
            context->PSSetShaderResources(start,length,views.data());
            for (unsigned i=0; i<length; ++i) previous[start+i].Reset();
        });
        bound.fill(false);
    }
};
}
