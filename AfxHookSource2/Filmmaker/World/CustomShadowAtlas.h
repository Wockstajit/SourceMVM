#pragma once

// Phase B: capture the native live CSM depth atlas for CustomShadowFx sampling.
// Own module — research probe stays read-only; this owns the copy + suppress path.

#include <d3d11.h>

namespace Filmmaker {

void CustomShadowAtlas_SetActive(bool active);
bool CustomShadowAtlas_Active();

// Borrow live CSM fills (csm_max_num_cascades_override) even when Shadows master is off,
// so the atlas still receives depth while engine sampling is suppressed by clear-after-copy.
void CustomShadowAtlas_SetFillBorrow(bool borrow);
bool CustomShadowAtlas_FillBorrow();

void CustomShadowAtlas_BeginDevice(ID3D11Device* device);
void CustomShadowAtlas_EndDevice();

// Observe OMSetRenderTargets. When the CSM atlas DSV is unbound, copy it then clear so
// engine lighting samples an empty map while our BeforeUi2 pass uses the copy.
void CustomShadowAtlas_NoteRenderTargets(
	ID3D11DeviceContext* context,
	UINT numViews,
	ID3D11RenderTargetView* const* renderTargetViews,
	ID3D11DepthStencilView* depthStencilView);

// Stable SRV for the last captured atlas (R16_UNORM). Null if no capture yet.
ID3D11ShaderResourceView* CustomShadowAtlas_Srv();
bool CustomShadowAtlas_HaveCapture();
UINT CustomShadowAtlas_Width();
UINT CustomShadowAtlas_Height();

unsigned long long CustomShadowAtlas_CaptureCount();

} // namespace Filmmaker
