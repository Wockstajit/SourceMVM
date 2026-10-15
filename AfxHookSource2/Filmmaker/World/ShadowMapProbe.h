#pragma once

// Research-only observer for the native DX11 shadow resources. It does not alter render state
// and stays inert unless explicitly armed with `world shadowmap probe on`.

#include <d3d11.h>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void ShadowMapProbe_NoteRenderTargets(
	ID3D11DeviceContext* context,
	UINT numViews,
	ID3D11RenderTargetView* const* renderTargetViews,
	ID3D11DepthStencilView* depthStencilView);

void ShadowMapProbe_NotePixelShader(ID3D11PixelShader* shader, const char* name);

void ShadowMapProbe_NoteShaderResources(
	ID3D11DeviceContext* context,
	UINT startSlot,
	UINT numViews,
	ID3D11ShaderResourceView* const* shaderResourceViews);

void ShadowMapProbe_NoteUpdateSubresource(
	ID3D11Resource* resource,
	const void* sourceData,
	UINT sourceRowPitch,
	UINT sourceDepthPitch);

bool ShadowMapProbe_Armed();

void ShadowMapProbe_NoteMap(ID3D11Resource* resource, void* mappedData);
void ShadowMapProbe_NoteUnmap(ID3D11Resource* resource);

void ShadowMapProbe_NoteDraw(
	ID3D11DeviceContext* context,
	const char* operation,
	UINT arg0,
	UINT arg1,
	UINT arg2,
	UINT arg3,
	void* caller);

void ShadowMapProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
