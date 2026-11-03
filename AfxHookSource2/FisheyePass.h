#pragma once

#include <d3d11.h>

// CFisheyePass -- render-layer post-process that redraws the just-rendered full-screen frame
// through a barrel-distortion ("fisheye") pixel shader. Standalone toggle, wired from
// Filmmaker::Fisheye_Enabled / "mirv_filmmaker fisheye". GoPro-style look without touching the engine's projection.
//
// Pipeline, run on the render thread at the BeforeUi2 callback (world rendered, Panorama UI
// not yet composited -- so HUD/menus are never distorted):
//   1. copy (or MSAA-resolve) the backbuffer into a temp shader-resource texture,
//   2. draw a full-NDC quad back over the whole backbuffer through afx_fisheye_ps_5_0,
//      which samples the temp texture with a radial k1/k2 distortion (corner-anchored, so
//      the frame stays filled -- slight centre zoom instead of clamped-edge streaks).
//
// All draw work is recorded on a DEFERRED context and replayed with
// ExecuteCommandList(RestoreContextState = TRUE), so the engine's immediate-context state for
// the following UI pass is left untouched (same isolation pattern as CViewportScaler, which
// this class deliberately mirrors -- see ViewportScaler.h for the pattern rationale).

class CFisheyePass {
public:
	~CFisheyePass();

	// Create / release the D3D objects. Called from the swapchain-backbuffer device hook,
	// right next to g_ViewportScaler.BeginDevice / EndDevice.
	void BeginDevice(ID3D11Device* device);
	void EndDevice();

	// True once all device objects exist.
	bool Ready() const;

	// Distort the whole backbuffer in place. strength is a plain multiplier on the reference
	// k1/k2 coefficients (1.0 = default look). Runs on the render thread with
	// the immediate context + backbuffer RTV delivered by the BeforeUi2 callback.
	void Apply(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget,
		float strength);

private:
	void ReleaseTemp();
	// viewFormat is the TYPED format used for the temp texture + SRV (the backbuffer itself may
	// be TYPELESS, which cannot back an SRV); it comes from the render-target view's format.
	bool EnsureTemp(const D3D11_TEXTURE2D_DESC& backbufferDesc, DXGI_FORMAT viewFormat);

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeviceContext = nullptr; // deferred

	ID3D11VertexShader* m_VertexShader = nullptr;   // reuses afx_drawtexture_vs_5_0
	ID3D11PixelShader* m_PixelShader = nullptr;     // afx_fisheye_ps_5_0
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;         // static 4-vertex NDC quad
	ID3D11Buffer* m_VsConstantBuffer = nullptr;     // static identity matrix
	ID3D11Buffer* m_PsConstantBuffer = nullptr;     // per-frame k1/k2/aspect (UpdateSubresource)
	ID3D11SamplerState* m_SamplerState = nullptr;   // linear clamp
	ID3D11RasterizerState* m_RasterizerState = nullptr; // solid, cull none
	ID3D11DepthStencilState* m_DepthStencilState = nullptr; // depth off
	ID3D11BlendState* m_BlendState = nullptr;       // opaque overwrite

	ID3D11Texture2D* m_TempTexture = nullptr;       // single-sample copy of the backbuffer
	ID3D11ShaderResourceView* m_TempSrv = nullptr;
	UINT m_TempWidth = 0;
	UINT m_TempHeight = 0;
	DXGI_FORMAT m_TempFormat = DXGI_FORMAT_UNKNOWN;
};
