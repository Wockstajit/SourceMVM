#include "stdafx.h"

#include "ReShadeAdvancedfx.h"

#include "AfxShaders.h"
#include "Filmmaker/Debug/GpuPassProfiler.h"

#include "../Shaders/build/afx_drawtexture_vs_5_0.h"
#include "../Shaders/build/afx_blit_ps_5_0.h"

#include <Windows.h>
#include <d3d11.h>

#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

CReShadeAdvancedfx g_ReShadeAdvancedfx;
CReShadeScenePass g_ReShadeScenePass;

namespace {

typedef bool (*MvmReShadeRenderEffects_t)(void* colorTexture, void* depthTexture);

// No loader reference of ours, even for one call (UiInputGate.cpp has the crash that taught this),
// so the lookup and the call both run under the SEH guard: a free in between reads as "not loaded".
int CallRenderEffects(HMODULE addon, void* color, void* depth, bool call) {
	__try {
		auto fn = (MvmReShadeRenderEffects_t)GetProcAddress(addon, "MvmReShadeRenderEffects");
		if (!fn) return -1;
		if (call) fn(color, depth);
		return 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

HMODULE LookUpAddon() {
	HMODULE m = nullptr;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"mvm_reshade_state.addon64", &m) ? m : nullptr;
}

} // namespace

bool CReShadeAdvancedfx::IsConnected() {
	const HMODULE addon = LookUpAddon();
	return addon && CallRenderEffects(addon, nullptr, nullptr, false) > 0;
}

bool CReShadeAdvancedfx::IsReShadeLoaded() {
	// It loads as a proxy d3d11.dll or dxgi.dll, so only its export tells it apart from the real one.
	const wchar_t* const candidates[] = { L"d3d11.dll", L"dxgi.dll", L"ReShade64.dll" };
	for (const wchar_t* name : candidates)
		if (HMODULE m = GetModuleHandleW(name))
			if (GetProcAddress(m, "ReShadeRegisterAddon")) return true;
	return false;
}

void CReShadeAdvancedfx::AdvancedfxRenderEffects(void* pRenderTargetView, void* pDepthTextureResource) {
	if (const HMODULE addon = LookUpAddon())
		CallRenderEffects(addon, pRenderTargetView, pDepthTextureResource, true);
}

// ---- the scene pass ------------------------------------------------------------------------------
namespace {

// The 52-byte vertex afx_drawtexture_vs_5_0 expects; only POSITION (NDC) and TEXCOORD0.xy are read.
struct Vertex {
	float x, y, z;
	DWORD diffuse;
	float t0u, t0v, t0w;
	float t1u, t1v, t1w;
	float t2u, t2v, t2w;
};

struct VsConstants {
	DirectX::XMFLOAT4X4 matrix;
	DirectX::XMFLOAT4 plane0;
	DirectX::XMFLOAT4 planeN;
	DirectX::XMFLOAT4 screenInfo;
};

const Vertex c_Quad[4] = {
	{ -1.0f,  1.0f, 0.0f, 0xffffffff, 0.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f,  1.0f, 0.0f, 0xffffffff, 1.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{ -1.0f, -1.0f, 0.0f, 0xffffffff, 0.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
};

DXGI_FORMAT TypedFromTypeless(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:    return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:                               return f;
	}
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

} // namespace

CReShadeScenePass::~CReShadeScenePass() { EndDevice(); }

bool CReShadeScenePass::Ready() const {
	return m_Device && m_DeferredContext && m_VertexShader && m_PixelShader && m_InputLayout && m_VertexBuffer
		&& m_VsConstantBuffer && m_SamplerState && m_RasterizerState && m_DepthStencilState && m_BlendState;
}

void CReShadeScenePass::BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;
	device->AddRef();
	m_Device = device;
	m_Device->CreateDeferredContext(0, &m_DeferredContext);

	size_t size = 0;
	if (void* so = LoadFromAcsShaderFileInMemory(L"afx_drawtexture_vs_5_0.acs", ShaderCombo_afx_drawtexture_vs_5_0::GetCombo(), size)) {
		m_Device->CreateVertexShader(so, size, NULL, &m_VertexShader);
		const D3D11_INPUT_ELEMENT_DESC inputDesc[5] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
		};
		m_Device->CreateInputLayout(inputDesc, 5, so, size, &m_InputLayout);
		free(so);
	}
	if (void* so = LoadFromAcsShaderFileInMemory(L"afx_blit_ps_5_0.acs", ShaderCombo_afx_blit_ps_5_0::GetCombo(), size)) {
		m_Device->CreatePixelShader(so, size, NULL, &m_PixelShader);
		free(so);
	}
	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(c_Quad);
		bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA srd = {};
		srd.pSysMem = c_Quad;
		m_Device->CreateBuffer(&bd, &srd, &m_VertexBuffer);
	}
	{
		VsConstants cb = {};
		cb.matrix = DirectX::XMFLOAT4X4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(cb);
		bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA srd = {};
		srd.pSysMem = &cb;
		m_Device->CreateBuffer(&bd, &srd, &m_VsConstantBuffer);
	}
	{
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // same size both ways: every pixel maps to itself
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		m_Device->CreateSamplerState(&sd, &m_SamplerState);
	}
	{
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		m_Device->CreateRasterizerState(&rd, &m_RasterizerState);
	}
	{
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		m_Device->CreateDepthStencilState(&dd, &m_DepthStencilState);
	}
	{
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		m_Device->CreateBlendState(&bd, &m_BlendState);
	}
}

void CReShadeScenePass::ReleaseCopy() {
	SafeRelease(m_CopySrv);
	SafeRelease(m_Copy);
	m_CopyWidth = m_CopyHeight = 0;
	m_CopyFormat = m_CopyViewFormat = 0;
}

void CReShadeScenePass::EndDevice() {
	ReleaseCopy();
	SafeRelease(m_BlendState);
	SafeRelease(m_DepthStencilState);
	SafeRelease(m_RasterizerState);
	SafeRelease(m_SamplerState);
	SafeRelease(m_VsConstantBuffer);
	SafeRelease(m_VertexBuffer);
	SafeRelease(m_InputLayout);
	SafeRelease(m_PixelShader);
	SafeRelease(m_VertexShader);
	SafeRelease(m_DeferredContext);
	SafeRelease(m_Device);
}

bool CReShadeScenePass::EnsureCopy(unsigned width, unsigned height, int format, int viewFormat) {
	if (m_Copy && m_CopySrv && m_CopyWidth == width && m_CopyHeight == height && m_CopyFormat == format && m_CopyViewFormat == viewFormat)
		return true;
	ReleaseCopy();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = (DXGI_FORMAT)format; // typeless when the scene is: ReShade makes its linear and sRGB views of it
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &m_Copy)) || !m_Copy) { ReleaseCopy(); return false; }
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = (DXGI_FORMAT)viewFormat;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	if (FAILED(m_Device->CreateShaderResourceView(m_Copy, &sd, &m_CopySrv)) || !m_CopySrv) { ReleaseCopy(); return false; }
	m_CopyWidth = width;
	m_CopyHeight = height;
	m_CopyFormat = format;
	m_CopyViewFormat = viewFormat;
	return true;
}

bool CReShadeScenePass::Apply(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget, ID3D11Resource* pDepth) {
	if (!Ready() || !pImmediateContext || !pTarget) return false;

	ID3D11Resource* pResource = nullptr;
	pTarget->GetResource(&pResource);
	if (!pResource) return false;
	ID3D11Texture2D* pScene = nullptr;
	if (FAILED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pScene)) || !pScene) {
		pResource->Release();
		return false;
	}
	bool ran = false;
	D3D11_TEXTURE2D_DESC sceneDesc;
	pScene->GetDesc(&sceneDesc);
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
	pTarget->GetDesc(&rtvDesc);
	const DXGI_FORMAT viewFormat = rtvDesc.Format != DXGI_FORMAT_UNKNOWN ? rtvDesc.Format : TypedFromTypeless(sceneDesc.Format);

	if (EnsureCopy(sceneDesc.Width, sceneDesc.Height, sceneDesc.Format, viewFormat)) {
		// 1. The finished scene into the single-sample copy.
		if (sceneDesc.SampleDesc.Count > 1) m_DeferredContext->ResolveSubresource(m_Copy, 0, pScene, 0, viewFormat);
		else m_DeferredContext->CopyResource(m_Copy, pScene);
		ID3D11CommandList* pList = nullptr;
		m_DeferredContext->FinishCommandList(FALSE, &pList);
		if (pList) {
			pImmediateContext->ExecuteCommandList(pList, TRUE);
			pList->Release();

			// 2. ReShade on the copy. It saves and restores the immediate context's state around this.
			g_ReShadeAdvancedfx.AdvancedfxRenderEffects(m_Copy, pDepth);

			// 3. The graded copy back over the scene.
			ID3D11RenderTargetView* rtvs[1] = { pTarget };
			m_DeferredContext->OMSetRenderTargets(1, rtvs, nullptr);
			D3D11_VIEWPORT vp = {};
			vp.Width = (float)sceneDesc.Width;
			vp.Height = (float)sceneDesc.Height;
			vp.MaxDepth = 1.0f;
			m_DeferredContext->RSSetViewports(1, &vp);
			m_DeferredContext->IASetInputLayout(m_InputLayout);
			UINT stride = sizeof(Vertex), offset = 0;
			m_DeferredContext->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
			m_DeferredContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			m_DeferredContext->VSSetShader(m_VertexShader, nullptr, 0);
			m_DeferredContext->VSSetConstantBuffers(0, 1, &m_VsConstantBuffer);
			m_DeferredContext->PSSetShader(m_PixelShader, nullptr, 0);
			m_DeferredContext->PSSetShaderResources(0, 1, &m_CopySrv);
			m_DeferredContext->PSSetSamplers(0, 1, &m_SamplerState);
			m_DeferredContext->RSSetState(m_RasterizerState);
			m_DeferredContext->OMSetDepthStencilState(m_DepthStencilState, 0);
			const float blendFactor[4] = { 0, 0, 0, 0 };
			m_DeferredContext->OMSetBlendState(m_BlendState, blendFactor, 0xffffffff);
			m_DeferredContext->Draw(4, 0);
			pList = nullptr;
			m_DeferredContext->FinishCommandList(FALSE, &pList);
			if (pList) {
				GPU_PASS_SCOPE(pImmediateContext, "reshade");
				pImmediateContext->ExecuteCommandList(pList, TRUE);
				pList->Release();
			}
			ran = true;
		}
	}
	pScene->Release();
	pResource->Release();
	return ran;
}
