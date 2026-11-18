#pragma once
// Between the layer's state (RenderPixelStateLayer.cpp, also built into the renderer tests) and its
// device-context detours (RenderPixelStateLayerHooks.cpp, hook DLL only).
#include <d3d11.h>

namespace Filmmaker::PixelStateLayer::Detail {

struct Originals {
    bool attached = false;
    void (STDMETHODCALLTYPE* setShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT) = nullptr;
    void (STDMETHODCALLTYPE* setShaderResources)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*) = nullptr;
    void (STDMETHODCALLTYPE* setSamplers)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*) = nullptr;
    void (STDMETHODCALLTYPE* setConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*) = nullptr;
    void (STDMETHODCALLTYPE* getConstantBuffers)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer**) = nullptr;
};
void SetOriginals(const Originals& originals);
const Originals& GetOriginals();

bool Holds(ID3D11DeviceContext* context);
bool ShaderHeld();
void GetShader(ID3D11PixelShader** shader, ID3D11ClassInstance** instances, UINT* instanceCount);
void FixViews(UINT start, UINT count, ID3D11ShaderResourceView** views);
void FixSamplers(UINT start, UINT count, ID3D11SamplerState** samplers);
void FixBuffers(UINT start, UINT count, ID3D11Buffer** buffers);
ID3D11ShaderResourceView* const* SetViews(UINT start, UINT count, ID3D11ShaderResourceView* const* views, ID3D11ShaderResourceView** scratch);
ID3D11SamplerState* const* SetSamplers(UINT start, UINT count, ID3D11SamplerState* const* samplers, ID3D11SamplerState** scratch);
ID3D11Buffer* const* SetBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers, ID3D11Buffer** scratch, bool& flush);
bool TouchesBuffers(UINT start, UINT count);
bool SameVertexShader(ID3D11VertexShader* shader);

} // namespace Filmmaker::PixelStateLayer::Detail
