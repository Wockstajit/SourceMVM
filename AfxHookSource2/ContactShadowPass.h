#pragma once

#include <d3d11.h>

// Phase B directional shadow-map composite (CustomShadowFx).
// Samples the captured native CSM atlas in sun space (not screen-space contact march).

class CContactShadowPass {
public:
	~CContactShadowPass();

	void BeginDevice(ID3D11Device* device);
	void EndDevice();
	bool Ready() const;

	void Apply(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget,
		ID3D11DepthStencilView* pDepth, float sunYawDeg, float sunPitchDeg,
		float strength, float softTexels, float thickness, int steps,
		const float cameraOrigin[3], const float cameraAngles[3],
		const float projectionMatrix16[16],
		ID3D11ShaderResourceView* shadowAtlasSrv, UINT atlasWidth, UINT atlasHeight);

private:
	void ReleaseColorTemp();
	void ReleaseDepthTemp();
	bool EnsureColorTemp(const D3D11_TEXTURE2D_DESC& backbufferDesc, DXGI_FORMAT viewFormat);
	bool EnsureDepthTemp(UINT width, UINT height);
	bool CopyDepthToTemp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
		UINT width, UINT height);

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeviceContext = nullptr;

	ID3D11VertexShader* m_VertexShader = nullptr;
	ID3D11PixelShader* m_PixelShader = nullptr;
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;
	ID3D11Buffer* m_VsConstantBuffer = nullptr;
	ID3D11Buffer* m_PsConstantBuffer = nullptr;
	ID3D11SamplerState* m_SamplerState = nullptr;
	ID3D11SamplerState* m_ShadowSampler = nullptr;
	ID3D11RasterizerState* m_RasterizerState = nullptr;
	ID3D11DepthStencilState* m_DepthStencilState = nullptr;
	ID3D11BlendState* m_BlendState = nullptr;

	ID3D11Texture2D* m_ColorTemp = nullptr;
	ID3D11ShaderResourceView* m_ColorSrv = nullptr;
	UINT m_ColorWidth = 0;
	UINT m_ColorHeight = 0;
	DXGI_FORMAT m_ColorFormat = DXGI_FORMAT_UNKNOWN;

	ID3D11Texture2D* m_DepthTemp = nullptr;
	ID3D11ShaderResourceView* m_DepthSrv = nullptr;
	UINT m_DepthWidth = 0;
	UINT m_DepthHeight = 0;
	DXGI_FORMAT m_DepthFormat = DXGI_FORMAT_UNKNOWN;
	UINT m_DepthSamples = 0;
};

struct AfxContactShadowRequest {
	bool active = false;
	float strength = 0.65f;
	float stepPixels = 2.5f;   // soft radius in atlas texels (Phase B)
	float thickness = 0.0008f; // depth bias
	int steps = 16;            // PCF taps
	float cascadeRange = 512.0f;
};

namespace AfxContactShadow {
	void SetRequest(const AfxContactShadowRequest& request);
	bool Active(AfxContactShadowRequest* outRequest = nullptr);
	CContactShadowPass* Pass();
	void BeginDevice(ID3D11Device* device);
	void EndDevice();
}
