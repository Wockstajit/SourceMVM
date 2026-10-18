#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "RelightReflectionFilterMath.h"
#include <vector>

namespace Filmmaker::Relight {
// Filters the owned live cube. Base-level mirror detail is copied exactly; roughness
// levels integrate directions across cube edges, with solid-angle source mip selection.
class ReflectionFilter {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> outputs;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> constants;
    unsigned mips=0,size=0;
    bool initialized=false;
public:
    bool Initialize(ID3D11Device*,unsigned size);
    // Private deferred context, owned by the GI pass. Touched CS bindings are cleared.
    bool Apply(ID3D11DeviceContext*,ID3D11Texture2D*,ID3D11ShaderResourceView*);
    ID3D11ShaderResourceView* View()const{return view.Get();}
};
}
