#include "stdafx.h"

#include "ContactShadowPass.h"

#include "AfxShaders.h"
#include "WrpConsole.h"
#include "Filmmaker/Debug/MvmDebugLog.h"
#include "Filmmaker/World/SunRotationMath.h"

#include "../Shaders/build/afx_drawtexture_vs_5_0.h"
#include "../Shaders/build/afx_contact_shadow_ps_5_0.h"

#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <cmath>
#include <mutex>

extern ID3D11DepthStencilView* g_pCurrentDepthStencilView;

using Filmmaker::MvmDebugLog_Active;
using Filmmaker::MvmDebugLog_Linef;
using Filmmaker::MvmDebugLog_LinefAlways;

namespace {

struct Vertex {
	float x, y, z;
	DWORD diffuse;
	float t0u, t0v, t0w;
	float t1u, t1v, t1w;
	float t2u, t2v, t2w;
};

struct VS_CONSTANT_BUFFER {
	DirectX::XMFLOAT4X4 matrix;
	DirectX::XMFLOAT4 plane0;
	DirectX::XMFLOAT4 planeN;
	DirectX::XMFLOAT4 screenInfo;
};

struct PS_CONSTANT_BUFFER {
	DirectX::XMFLOAT4 invViewProj[4];
	DirectX::XMFLOAT4 lightViewProj[4];
	float camX, camY, camZ, strength;
	float bias, softTexels, taps, cascadeRange;
	float atlasW, atlasH, tilePx, unused0;
	float cascadeScale0, cascadeScale1, cascadeScale2, cascadeScale3;
};

void StoreRows(DirectX::XMMATRIX m, DirectX::XMFLOAT4 out[4]) {
	DirectX::XMFLOAT4X4 f;
	DirectX::XMStoreFloat4x4(&f, m);
	out[0] = DirectX::XMFLOAT4(f._11, f._12, f._13, f._14);
	out[1] = DirectX::XMFLOAT4(f._21, f._22, f._23, f._24);
	out[2] = DirectX::XMFLOAT4(f._31, f._32, f._33, f._34);
	out[3] = DirectX::XMFLOAT4(f._41, f._42, f._43, f._44);
}

DirectX::XMMATRIX BuildViewFromCamera(const float origin[3], const float angles[3]) {
	const Filmmaker::SunRotationMath::Vec3 forward =
		Filmmaker::SunRotationMath::AnglesToDirection(angles[1], angles[0]);
	DirectX::XMVECTOR eye = DirectX::XMVectorSet(origin[0], origin[1], origin[2], 1.0f);
	DirectX::XMVECTOR at = DirectX::XMVectorSet(
		origin[0] + forward.x, origin[1] + forward.y, origin[2] + forward.z, 1.0f);
	DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	return DirectX::XMMatrixLookAtRH(eye, at, up);
}

DirectX::XMMATRIX BuildLightViewProj(float sunYaw, float sunPitch, const float focus[3], float halfExtent) {
	const Filmmaker::SunRotationMath::Vec3 toSun =
		Filmmaker::SunRotationMath::AnglesToDirection(sunYaw, sunPitch);
	const float dist = 6000.0f;
	DirectX::XMVECTOR eye = DirectX::XMVectorSet(
		focus[0] + toSun.x * dist, focus[1] + toSun.y * dist, focus[2] + toSun.z * dist, 1.0f);
	DirectX::XMVECTOR at = DirectX::XMVectorSet(focus[0], focus[1], focus[2], 1.0f);
	DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	if (fabsf(toSun.z) > 0.95f)
		up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	const DirectX::XMMATRIX view = DirectX::XMMatrixLookAtRH(eye, at, up);
	const DirectX::XMMATRIX proj = DirectX::XMMatrixOrthographicOffCenterRH(
		-halfExtent, halfExtent, -halfExtent, halfExtent, 1.0f, dist * 2.0f);
	return DirectX::XMMatrixMultiply(view, proj);
}

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

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT depthFmt) {
	switch (depthFmt) {
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32_FLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return DXGI_FORMAT_R16_UNORM;
	default:
		return DXGI_FORMAT_R32_FLOAT;
	}
}

const Vertex c_Quad[4] = {
	{ -1.0f,  1.0f, 0.0f, 0xffffffff, 0.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f,  1.0f, 0.0f, 0xffffffff, 1.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{ -1.0f, -1.0f, 0.0f, 0xffffffff, 0.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
};

CContactShadowPass g_ContactShadowPass;

} // namespace

CContactShadowPass::~CContactShadowPass() {
	EndDevice();
}

bool CContactShadowPass::Ready() const {
	return m_Device && m_DeviceContext && m_VertexShader && m_PixelShader && m_InputLayout
		&& m_VertexBuffer && m_VsConstantBuffer && m_PsConstantBuffer && m_SamplerState
		&& m_ShadowSampler && m_RasterizerState && m_DepthStencilState && m_BlendState;
}

void CContactShadowPass::ReleaseColorTemp() {
	if (m_ColorSrv) { m_ColorSrv->Release(); m_ColorSrv = nullptr; }
	if (m_ColorTemp) { m_ColorTemp->Release(); m_ColorTemp = nullptr; }
	m_ColorWidth = m_ColorHeight = 0;
	m_ColorFormat = DXGI_FORMAT_UNKNOWN;
}

void CContactShadowPass::ReleaseDepthTemp() {
	if (m_DepthSrv) { m_DepthSrv->Release(); m_DepthSrv = nullptr; }
	if (m_DepthTemp) { m_DepthTemp->Release(); m_DepthTemp = nullptr; }
	m_DepthWidth = m_DepthHeight = 0;
	m_DepthFormat = DXGI_FORMAT_UNKNOWN;
	m_DepthSamples = 0;
}

void CContactShadowPass::BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;

	device->AddRef();
	m_Device = device;
	m_Device->CreateDeferredContext(0, &m_DeviceContext);

	{
		size_t size = 0;
		void* so = LoadFromAcsShaderFileInMemory(L"afx_drawtexture_vs_5_0.acs",
			ShaderCombo_afx_drawtexture_vs_5_0::GetCombo(), size);
		if (so) {
			m_Device->CreateVertexShader(so, size, NULL, &m_VertexShader);
			D3D11_INPUT_ELEMENT_DESC inputDesc[5] = {
				{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
			};
			m_Device->CreateInputLayout(inputDesc, 5, so, size, &m_InputLayout);
			free(so);
		}
	}

	{
		size_t size = 0;
		void* so = LoadFromAcsShaderFileInMemory(L"afx_contact_shadow_ps_5_0.acs",
			ShaderCombo_afx_contact_shadow_ps_5_0::GetCombo(), size);
		if (so) {
			m_Device->CreatePixelShader(so, size, NULL, &m_PixelShader);
			free(so);
		}
	}

	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(c_Quad);
		bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = c_Quad;
		m_Device->CreateBuffer(&bd, &init, &m_VertexBuffer);
	}

	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(VS_CONSTANT_BUFFER);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		VS_CONSTANT_BUFFER cb = {};
		DirectX::XMStoreFloat4x4(&cb.matrix, DirectX::XMMatrixIdentity());
		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = &cb;
		m_Device->CreateBuffer(&bd, &init, &m_VsConstantBuffer);
	}

	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(PS_CONSTANT_BUFFER);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		m_Device->CreateBuffer(&bd, nullptr, &m_PsConstantBuffer);
	}

	{
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		m_Device->CreateSamplerState(&sd, &m_SamplerState);

		D3D11_SAMPLER_DESC ss = {};
		ss.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		ss.ComparisonFunc = D3D11_COMPARISON_NEVER;
		ss.MaxLOD = D3D11_FLOAT32_MAX;
		m_Device->CreateSamplerState(&ss, &m_ShadowSampler);
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
		m_Device->CreateDepthStencilState(&dd, &m_DepthStencilState);
	}

	{
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		m_Device->CreateBlendState(&bd, &m_BlendState);
	}

	MvmDebugLog_LinefAlways("world.customshadow",
		"ContactShadowPass BeginDevice phase=b-shadowmap ready=%d", Ready() ? 1 : 0);
}

void CContactShadowPass::EndDevice() {
	ReleaseColorTemp();
	ReleaseDepthTemp();
	if (m_BlendState) { m_BlendState->Release(); m_BlendState = nullptr; }
	if (m_DepthStencilState) { m_DepthStencilState->Release(); m_DepthStencilState = nullptr; }
	if (m_RasterizerState) { m_RasterizerState->Release(); m_RasterizerState = nullptr; }
	if (m_ShadowSampler) { m_ShadowSampler->Release(); m_ShadowSampler = nullptr; }
	if (m_SamplerState) { m_SamplerState->Release(); m_SamplerState = nullptr; }
	if (m_PsConstantBuffer) { m_PsConstantBuffer->Release(); m_PsConstantBuffer = nullptr; }
	if (m_VsConstantBuffer) { m_VsConstantBuffer->Release(); m_VsConstantBuffer = nullptr; }
	if (m_VertexBuffer) { m_VertexBuffer->Release(); m_VertexBuffer = nullptr; }
	if (m_InputLayout) { m_InputLayout->Release(); m_InputLayout = nullptr; }
	if (m_PixelShader) { m_PixelShader->Release(); m_PixelShader = nullptr; }
	if (m_VertexShader) { m_VertexShader->Release(); m_VertexShader = nullptr; }
	if (m_DeviceContext) { m_DeviceContext->Release(); m_DeviceContext = nullptr; }
	if (m_Device) { m_Device->Release(); m_Device = nullptr; }
}

bool CContactShadowPass::EnsureColorTemp(const D3D11_TEXTURE2D_DESC& bb, DXGI_FORMAT viewFormat) {
	if (m_ColorTemp && m_ColorSrv
		&& m_ColorWidth == bb.Width && m_ColorHeight == bb.Height && m_ColorFormat == viewFormat)
		return true;
	ReleaseColorTemp();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = bb.Width;
	td.Height = bb.Height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = viewFormat;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &m_ColorTemp)) || !m_ColorTemp) {
		ReleaseColorTemp();
		return false;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = viewFormat;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	if (FAILED(m_Device->CreateShaderResourceView(m_ColorTemp, &sd, &m_ColorSrv)) || !m_ColorSrv) {
		ReleaseColorTemp();
		return false;
	}
	m_ColorWidth = bb.Width;
	m_ColorHeight = bb.Height;
	m_ColorFormat = viewFormat;
	return true;
}

bool CContactShadowPass::EnsureDepthTemp(UINT width, UINT height) {
	if (m_DepthTemp && m_DepthSrv && m_DepthWidth == width && m_DepthHeight == height)
		return true;
	ReleaseDepthTemp();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_TYPELESS;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &m_DepthTemp)) || !m_DepthTemp) {
		// Fallback: SRV-only R32_FLOAT (copy via staging path may still fail; try Create).
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &m_DepthTemp)) || !m_DepthTemp) {
			ReleaseDepthTemp();
			return false;
		}
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R32_FLOAT;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	if (FAILED(m_Device->CreateShaderResourceView(m_DepthTemp, &sd, &m_DepthSrv)) || !m_DepthSrv) {
		ReleaseDepthTemp();
		return false;
	}
	m_DepthWidth = width;
	m_DepthHeight = height;
	return true;
}

bool CContactShadowPass::CopyDepthToTemp(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
	UINT width, UINT height) {
	(void)ctx;
	(void)width;
	(void)height;
	if (!dsv) return false;

	ID3D11Resource* depthRes = nullptr;
	dsv->GetResource(&depthRes);
	if (!depthRes) return false;

	ID3D11Texture2D* depthTex = nullptr;
	if (FAILED(depthRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&depthTex)) || !depthTex) {
		depthRes->Release();
		return false;
	}

	D3D11_TEXTURE2D_DESC srcDesc = {};
	depthTex->GetDesc(&srcDesc);
	m_DepthFormat = srcDesc.Format;
	m_DepthSamples = srcDesc.SampleDesc.Count;

	ID3D11ShaderResourceView* srcSrv = nullptr;
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DepthSrvFormat(srcDesc.Format);
	sd.ViewDimension = (srcDesc.SampleDesc.Count > 1)
		? D3D11_SRV_DIMENSION_TEXTURE2DMS
		: D3D11_SRV_DIMENSION_TEXTURE2D;
	if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
		sd.Texture2D.MipLevels = 1;

	const HRESULT hr = m_Device->CreateShaderResourceView(depthTex, &sd, &srcSrv);
	depthTex->Release();
	depthRes->Release();
	if (FAILED(hr) || !srcSrv)
		return false;

	if (m_DepthSrv) m_DepthSrv->Release();
	m_DepthSrv = srcSrv;
	// Depth temp texture unused when sampling the engine resource directly.
	if (m_DepthTemp) { m_DepthTemp->Release(); m_DepthTemp = nullptr; }
	m_DepthWidth = srcDesc.Width;
	m_DepthHeight = srcDesc.Height;
	return true;
}

void CContactShadowPass::Apply(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget,
	ID3D11DepthStencilView* pDepth, float sunYawDeg, float sunPitchDeg,
	float strength, float softTexels, float thickness, int steps,
	const float cameraOrigin[3], const float cameraAngles[3],
	const float projectionMatrix16[16],
	ID3D11ShaderResourceView* shadowAtlasSrv, UINT atlasWidth, UINT atlasHeight) {
	static int s_prev = -999;
	static unsigned long long s_applyFrames = 0;
	const unsigned long long applyFrame = ++s_applyFrames;
	auto changed = [&](int code) {
		if (s_prev != code) { s_prev = code; return true; }
		return false;
	};

	if (!Ready() || !pImmediateContext || !pTarget) {
		if (changed(1) && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("world.customshadow", "SKIP ready=%d", Ready() ? 1 : 0);
		return;
	}
	if (!(strength > 0.0f) || !shadowAtlasSrv || atlasWidth == 0 || atlasHeight == 0) {
		if (changed(2) && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("world.customshadow",
				"SKIP atlas srv=%p %ux%u strength=%.3f",
				(void*)shadowAtlasSrv, atlasWidth, atlasHeight, strength);
		return;
	}

	ID3D11Resource* pResource = nullptr;
	pTarget->GetResource(&pResource);
	if (!pResource) return;

	ID3D11Texture2D* pBackbuffer = nullptr;
	if (FAILED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pBackbuffer)) || !pBackbuffer) {
		pResource->Release();
		return;
	}

	D3D11_TEXTURE2D_DESC bbDesc;
	pBackbuffer->GetDesc(&bbDesc);
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
	pTarget->GetDesc(&rtvDesc);
	DXGI_FORMAT viewFmt = rtvDesc.Format;
	if (viewFmt == DXGI_FORMAT_UNKNOWN) viewFmt = TypedFromTypeless(bbDesc.Format);

	if (!EnsureColorTemp(bbDesc, viewFmt)) {
		if (changed(3) && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("world.customshadow", "SKIP EnsureColorTemp failed");
		pBackbuffer->Release();
		pResource->Release();
		return;
	}

	ID3D11DepthStencilView* depthToUse = pDepth ? pDepth : g_pCurrentDepthStencilView;
	if (!depthToUse || !CopyDepthToTemp(pImmediateContext, depthToUse, bbDesc.Width, bbDesc.Height)) {
		if (changed(4) && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("world.customshadow",
				"SKIP no depth (dsv=%p g=%p)", (void*)pDepth, (void*)g_pCurrentDepthStencilView);
		pBackbuffer->Release();
		pResource->Release();
		return;
	}

	if (bbDesc.SampleDesc.Count > 1)
		m_DeviceContext->ResolveSubresource(m_ColorTemp, 0, pBackbuffer, 0, viewFmt);
	else
		m_DeviceContext->CopyResource(m_ColorTemp, pBackbuffer);

	if (softTexels < 0.0f) softTexels = 0.0f;
	if (softTexels > 16.0f) softTexels = 16.0f;
	if (thickness < 0.00001f) thickness = 0.00001f;
	if (thickness > 0.05f) thickness = 0.05f;
	if (steps < 1) steps = 1;
	if (steps > 32) steps = 32;

	float cam[3] = {
		cameraOrigin ? cameraOrigin[0] : 0.0f,
		cameraOrigin ? cameraOrigin[1] : 0.0f,
		cameraOrigin ? cameraOrigin[2] : 0.0f
	};
	float ang[3] = {
		cameraAngles ? cameraAngles[0] : 0.0f,
		cameraAngles ? cameraAngles[1] : 0.0f,
		cameraAngles ? cameraAngles[2] : 0.0f
	};

	DirectX::XMMATRIX proj = DirectX::XMMatrixIdentity();
	if (projectionMatrix16) {
		DirectX::XMFLOAT4X4 pf(
			projectionMatrix16[0], projectionMatrix16[1], projectionMatrix16[2], projectionMatrix16[3],
			projectionMatrix16[4], projectionMatrix16[5], projectionMatrix16[6], projectionMatrix16[7],
			projectionMatrix16[8], projectionMatrix16[9], projectionMatrix16[10], projectionMatrix16[11],
			projectionMatrix16[12], projectionMatrix16[13], projectionMatrix16[14], projectionMatrix16[15]);
		proj = DirectX::XMLoadFloat4x4(&pf);
	}
	const DirectX::XMMATRIX view = BuildViewFromCamera(cam, ang);
	const DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(view, proj);
	DirectX::XMVECTOR det;
	const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&det, viewProj);
	const float cascadeRange = 512.0f;
	const DirectX::XMMATRIX lightViewProj = BuildLightViewProj(sunYawDeg, sunPitchDeg, cam, cascadeRange);

	PS_CONSTANT_BUFFER cb = {};
	StoreRows(invViewProj, cb.invViewProj);
	StoreRows(lightViewProj, cb.lightViewProj);
	cb.camX = cam[0]; cb.camY = cam[1]; cb.camZ = cam[2]; cb.strength = strength;
	cb.bias = thickness; cb.softTexels = softTexels; cb.taps = (float)steps; cb.cascadeRange = cascadeRange;
	cb.atlasW = (float)atlasWidth; cb.atlasH = (float)atlasHeight; cb.tilePx = 1280.0f;
	cb.cascadeScale0 = 1.0f; cb.cascadeScale1 = 2.0f; cb.cascadeScale2 = 4.0f; cb.cascadeScale3 = 8.0f;
	m_DeviceContext->UpdateSubresource(m_PsConstantBuffer, 0, nullptr, &cb, 0, 0);

	if (MvmDebugLog_Active() && (applyFrame % 30ull) == 1ull) {
		MvmDebugLog_Linef("world.customshadow.frame",
			"APPLY frame=%llu target=%p atlas=%ux%u srv=%p yaw=%.2f pitch=%.2f "
			"strength=%.3f soft=%.3f bias=%.6f taps=%d cam=(%.1f,%.1f,%.1f)",
			applyFrame, (void*)pTarget, atlasWidth, atlasHeight, (void*)shadowAtlasSrv,
			sunYawDeg, sunPitchDeg, strength, softTexels, thickness, steps,
			cam[0], cam[1], cam[2]);
	}

	ID3D11RenderTargetView* rtvs[1] = { pTarget };
	m_DeviceContext->OMSetRenderTargets(1, rtvs, nullptr);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)bbDesc.Width;
	vp.Height = (float)bbDesc.Height;
	vp.MaxDepth = 1.0f;
	m_DeviceContext->RSSetViewports(1, &vp);

	m_DeviceContext->IASetInputLayout(m_InputLayout);
	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	m_DeviceContext->IASetVertexBuffers(0, 1, &m_VertexBuffer, &stride, &offset);
	m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
	m_DeviceContext->VSSetConstantBuffers(0, 1, &m_VsConstantBuffer);
	m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
	m_DeviceContext->PSSetConstantBuffers(0, 1, &m_PsConstantBuffer);
	ID3D11ShaderResourceView* srvs[3] = { m_ColorSrv, m_DepthSrv, shadowAtlasSrv };
	m_DeviceContext->PSSetShaderResources(0, 3, srvs);
	ID3D11SamplerState* samplers[2] = { m_SamplerState, m_ShadowSampler };
	m_DeviceContext->PSSetSamplers(0, 2, samplers);
	m_DeviceContext->RSSetState(m_RasterizerState);
	m_DeviceContext->OMSetDepthStencilState(m_DepthStencilState, 0);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	m_DeviceContext->OMSetBlendState(m_BlendState, blendFactor, 0xffffffff);
	m_DeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
	m_DeviceContext->PSSetShaderResources(0, 3, nullSrvs);

	ID3D11CommandList* pCommandList = nullptr;
	m_DeviceContext->FinishCommandList(FALSE, &pCommandList);
	bool submitted = false;
	if (pCommandList) {
		pImmediateContext->ExecuteCommandList(pCommandList, TRUE);
		pCommandList->Release();
		submitted = true;
	}

	if (changed(5) && MvmDebugLog_Active()) {
		MvmDebugLog_LinefAlways("world.customshadow",
			"RUN phase=b submitted=%d atlas=%ux%u yaw=%.1f pitch=%.1f strength=%.2f soft=%.2f bias=%.6f taps=%d",
			submitted ? 1 : 0, atlasWidth, atlasHeight, sunYawDeg, sunPitchDeg,
			strength, softTexels, thickness, steps);
	}

	pBackbuffer->Release();
	pResource->Release();
}

namespace AfxContactShadow {

static std::mutex s_Mutex;
static AfxContactShadowRequest s_Request;

void SetRequest(const AfxContactShadowRequest& request) {
	std::lock_guard<std::mutex> lock(s_Mutex);
	s_Request = request;
}

bool Active(AfxContactShadowRequest* outRequest) {
	std::lock_guard<std::mutex> lock(s_Mutex);
	if (outRequest) *outRequest = s_Request;
	return s_Request.active;
}

CContactShadowPass* Pass() {
	return &g_ContactShadowPass;
}

void BeginDevice(ID3D11Device* device) {
	g_ContactShadowPass.BeginDevice(device);
}

void EndDevice() {
	g_ContactShadowPass.EndDevice();
}

} // namespace AfxContactShadow
