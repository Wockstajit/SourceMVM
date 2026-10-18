#include "stdafx.h"
#include "Filmmaker/Debug/GpuPassProfiler.h"

#include "RenderPassCompositor.h"

#include "AfxShaders.h"
#include "Filmmaker/Debug/MvmDebugLog.h"

#include "../Shaders/build/afx_drawtexture_vs_5_0.h"
#include "../Shaders/build/afx_depthms_ps_5_0.h"
#include "../Shaders/build/afx_render_pass_ps_5_0.h"

#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <atomic>
#include <cstdlib>

using Filmmaker::MvmDebugLog_Active;
using Filmmaker::MvmDebugLog_LinefAlways;

namespace AfxRenderPass {
namespace {

struct Vertex {
	float x, y, z;
	DWORD diffuse;
	float t0u, t0v, t0w;
	float t1u, t1v, t1w;
	float t2u, t2v, t2w;
};

const Vertex c_Quad[4] = {
	{ -1.0f,  1.0f, 0.0f, 0xffffffff, 0.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f,  1.0f, 0.0f, 0xffffffff, 1.0f, 0.0f, 0.0f, 0,0,0, 0,0,0 },
	{ -1.0f, -1.0f, 0.0f, 0xffffffff, 0.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
	{  1.0f, -1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f, 0.0f, 0,0,0, 0,0,0 },
};

struct VS_CONSTANT_BUFFER {
	DirectX::XMFLOAT4X4 matrix;
	DirectX::XMFLOAT4 plane0;
	DirectX::XMFLOAT4 planeN;
	DirectX::XMFLOAT4 screenInfo;
};

struct PS_CONSTANT_BUFFER {
	int modeMask[4];
	float keyIn[4];
	float keyOut[4];
	float nearFar[4];
	float camera[4];
	float depthRange[4];
	float forward[4];
	float right[4];
	float up[4];
	float invViewProj[16];
	float texel[4];
	float diff[4];
};

DXGI_FORMAT TypedFromTypeless(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

DXGI_FORMAT DepthTypeless(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
	switch (DepthTypeless(f)) {
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// One texture we own, with whichever views it needs.
struct Surface {
	ID3D11Texture2D* tex = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	void Release() { SafeRelease(rtv); SafeRelease(srv); SafeRelease(tex); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }

	// `texFormat` is the storage format, `viewFormat` the SRV/RTV format (typed).
	bool Ensure(ID3D11Device* dev, UINT width, UINT height, DXGI_FORMAT texFormat, DXGI_FORMAT viewFormat, bool renderTarget) {
		if (tex && w == width && h == height && fmt == texFormat) return true;
		Release();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = texFormat; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) || !tex) { Release(); return false; }
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = viewFormat; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
		if (FAILED(dev->CreateShaderResourceView(tex, &sd, &srv)) || !srv) { Release(); return false; }
		if (renderTarget) {
			D3D11_RENDER_TARGET_VIEW_DESC rd = {};
			rd.Format = viewFormat; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(dev->CreateRenderTargetView(tex, &rd, &rtv)) || !rtv) { Release(); return false; }
		}
		w = width; h = height; fmt = texFormat;
		return true;
	}
};

// Raw depth as a plain Texture2D<float>: MSAA depth is reduced to sample 0 by afx_depthms into an
// R32F target; single-sample depth is copied into a typeless twin that has a shader view.
struct DepthSurface {
	Surface extracted; // R32F render target (MSAA source)
	Surface copied;    // typeless copy (single-sample source)
	ID3D11ShaderResourceView* srv = nullptr; // whichever one holds the current frame (borrowed)
	void Release() { extracted.Release(); copied.Release(); srv = nullptr; }
};

class Compositor {
public:
	void BeginDevice(ID3D11Device* device) {
		EndDevice();
		if (!device) return;
		device->AddRef();
		m_Device = device;
		m_Device->CreateDeferredContext(0, &m_Ctx);
		{
			size_t size = 0;
			void* so = LoadFromAcsShaderFileInMemory(L"afx_drawtexture_vs_5_0.acs", ShaderCombo_afx_drawtexture_vs_5_0::GetCombo(), size);
			if (so) {
				m_Device->CreateVertexShader(so, size, nullptr, &m_Vs);
				D3D11_INPUT_ELEMENT_DESC in[5] = {
					{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
					{"COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
					{"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
					{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
					{"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
				};
				m_Device->CreateInputLayout(in, 5, so, size, &m_Layout);
				free(so);
			}
		}
		{
			size_t size = 0;
			void* so = LoadFromAcsShaderFileInMemory(L"afx_depthms_ps_5_0.acs", ShaderCombo_afx_depthms_ps_5_0::GetCombo(), size);
			if (so) { m_Device->CreatePixelShader(so, size, nullptr, &m_PsDepthMs); free(so); }
		}
		{
			size_t size = 0;
			void* so = LoadFromAcsShaderFileInMemory(L"afx_render_pass_ps_5_0.acs", ShaderCombo_afx_render_pass_ps_5_0::GetCombo(), size);
			if (so) { m_Device->CreatePixelShader(so, size, nullptr, &m_PsPass); free(so); }
		}
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(c_Quad); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA init = {}; init.pSysMem = c_Quad;
			m_Device->CreateBuffer(&bd, &init, &m_Vb);
		}
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(VS_CONSTANT_BUFFER); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			VS_CONSTANT_BUFFER cb = {};
			DirectX::XMStoreFloat4x4(&cb.matrix, DirectX::XMMatrixIdentity());
			D3D11_SUBRESOURCE_DATA init = {}; init.pSysMem = &cb;
			m_Device->CreateBuffer(&bd, &init, &m_VsCb);
		}
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(PS_CONSTANT_BUFFER); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			m_Device->CreateBuffer(&bd, nullptr, &m_PsCb);
		}
		{
			D3D11_RASTERIZER_DESC rd = {};
			rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
			m_Device->CreateRasterizerState(&rd, &m_Rs);
			D3D11_DEPTH_STENCIL_DESC dd = {};
			dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			m_Device->CreateDepthStencilState(&dd, &m_Ds);
			D3D11_BLEND_DESC bd = {};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			m_Device->CreateBlendState(&bd, &m_Bs);
		}
		MvmDebugLog_LinefAlways("render.pass.compositor", "BeginDevice ready=%d", Ready() ? 1 : 0);
	}

	void EndDevice() {
		m_Color.Release(); m_Depth.Release(); m_RefColor.Release(); m_RefDepth.Release(); m_Out.Release();
		m_WorldDepth.Release();
		m_RefValid = false; m_WorldRefValid = false;
		SafeRelease(m_Bs); SafeRelease(m_Ds); SafeRelease(m_Rs);
		SafeRelease(m_PsCb); SafeRelease(m_VsCb); SafeRelease(m_Vb); SafeRelease(m_Layout);
		SafeRelease(m_PsPass); SafeRelease(m_PsDepthMs); SafeRelease(m_Vs);
		SafeRelease(m_Ctx); SafeRelease(m_Device);
	}

	bool Ready() const {
		return m_Device && m_Ctx && m_Vs && m_Layout && m_PsDepthMs && m_PsPass && m_Vb && m_VsCb && m_PsCb && m_Rs && m_Ds && m_Bs;
	}

	bool StoreReference(ID3D11DeviceContext* immediate, ID3D11Texture2D* color, ID3D11DepthStencilView* depth) {
		m_RefValid = false;
		if (!Ready() || !immediate || !color || !depth) return Fail("reference: not ready");
		// The depth reduction needs the depth direction: the last frame's (it never changes mid-take).
		PS_CONSTANT_BUFFER cb = {};
		cb.camera[3] = m_Reversed ? 1.0f : 0.0f;
		m_Ctx->UpdateSubresource(m_PsCb, 0, nullptr, &cb, 0, 0);
		if (!CaptureColor(color, m_RefColor) || !CaptureDepth(depth, m_RefDepth)) { Discard(); return false; }
		if (!Submit(immediate)) return Fail("reference: submit");
		m_RefValid = true;
		return true;
	}

	// `target` null: into the owned RGBA8 output (returned). Otherwise drawn straight over it.
	ID3D11Texture2D* Compose(ID3D11DeviceContext* immediate, ID3D11Texture2D* color, ID3D11DepthStencilView* depth, const ComposeParams& p,
		ID3D11RenderTargetView* target = nullptr) {
		if (!Ready() || !immediate || !color) { Fail("compose: not ready"); return nullptr; }
		// The players mask keys the sky by depth; the live gun mask (4) is the viewmodel depth band.
		const bool needDepth = p.mode != 0 || p.mask == 1 || p.mask == 4;
		const bool needRef = p.mask == 2;
		if (needRef && !m_RefValid) { Fail("compose: gun pass without a Main reference this frame"); return nullptr; }
		const bool worldRef = p.mask == 1 && p.worldRef && m_WorldRefValid;
		if (p.mask == 1 && p.worldRef && !m_WorldRefValid) Fail("compose: players pass without a World Only reference, colour key only");
		// Constants first: the MSAA depth reduction reads the depth direction from them.
		D3D11_TEXTURE2D_DESC cd;
		color->GetDesc(&cd);
		PS_CONSTANT_BUFFER cb = {};
		cb.modeMask[0] = p.mode; cb.modeMask[1] = worldRef ? 3 : p.mask;
		for (int k = 0; k < 4; ++k) cb.keyIn[k] = p.keyIn[k];
		cb.keyOut[0] = p.keyOut[0]; cb.keyOut[1] = p.keyOut[1]; cb.keyOut[2] = p.keyOut[2]; cb.keyOut[3] = 1.0f;
		cb.nearFar[0] = p.nearUnits; cb.nearFar[1] = p.farUnits; cb.nearFar[2] = p.skyCutoff;
		for (int k = 0; k < 3; ++k) {
			cb.camera[k] = p.camera[k]; cb.forward[k] = p.forward[k]; cb.right[k] = p.right[k]; cb.up[k] = p.up[k];
		}
		cb.camera[3] = p.reversedDepth ? 1.0f : 0.0f;
		cb.depthRange[0] = p.depthMin; cb.depthRange[1] = p.depthMax;
		for (int k = 0; k < 16; ++k) cb.invViewProj[k] = p.invViewProj[k];
		cb.texel[0] = 1.0f / (float)cd.Width; cb.texel[1] = 1.0f / (float)cd.Height;
		cb.texel[2] = (float)cd.Width; cb.texel[3] = (float)cd.Height;
		cb.diff[0] = p.diffLo; cb.diff[1] = p.diffHi;
		// Drawn straight into an sRGB view (the live game view): the colour read from it is already
		// encoded, so write it linear and let the view encode it once, not twice (pale live plates).
		if (target) {
			D3D11_RENDER_TARGET_VIEW_DESC rd;
			target->GetDesc(&rd);
			cb.diff[2] = (rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
				|| rd.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) ? 1.0f : 0.0f;
		}
		m_Ctx->UpdateSubresource(m_PsCb, 0, nullptr, &cb, 0, 0);
		m_Reversed = p.reversedDepth;
		if (!CaptureColor(color, m_Color)) { Discard(); return nullptr; }
		if (needDepth && (!depth || !CaptureDepth(depth, m_Depth))) { Discard(); Fail("compose: no depth"); return nullptr; }
		if (!target && !m_Out.Ensure(m_Device, m_Color.w, m_Color.h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, true)) {
			Discard(); Fail("compose: output"); return nullptr;
		}


		ID3D11ShaderResourceView* srvs[5] = {
			m_Color.srv,
			needDepth ? m_Depth.srv : nullptr,
			needRef ? m_RefColor.srv : nullptr,
			needRef ? m_RefDepth.srv : nullptr,
			worldRef ? m_WorldDepth.srv : nullptr };
		Draw(m_PsPass, target ? target : m_Out.rtv, m_Color.w, m_Color.h, srvs, 5);
		if (!Submit(immediate)) { Fail("compose: submit"); return nullptr; }
		return target ? color : m_Out.tex;
	}

	bool StoreWorldReference(ID3D11DeviceContext* immediate, ID3D11DepthStencilView* depth) {
		m_WorldRefValid = false;
		if (!Ready() || !immediate || !depth) return Fail("world reference: not ready");
		PS_CONSTANT_BUFFER cb = {};
		cb.camera[3] = m_Reversed ? 1.0f : 0.0f;
		m_Ctx->UpdateSubresource(m_PsCb, 0, nullptr, &cb, 0, 0);
		if (!CaptureDepth(depth, m_WorldDepth)) { Discard(); return false; }
		if (!Submit(immediate)) return Fail("world reference: submit");
		m_WorldRefValid = true;
		return true;
	}

private:
	bool Fail(const char* why) {
		static std::atomic<unsigned> s_n{0};
		if (MvmDebugLog_Active() && s_n.fetch_add(1, std::memory_order_relaxed) < 32)
			MvmDebugLog_LinefAlways("render.pass.compositor", "FAIL %s", why);
		return false;
	}

	void Discard() {
		ID3D11CommandList* cl = nullptr;
		m_Ctx->FinishCommandList(FALSE, &cl);
		SafeRelease(cl);
	}

	bool Submit(ID3D11DeviceContext* immediate) {
		ID3D11CommandList* cl = nullptr;
		m_Ctx->FinishCommandList(FALSE, &cl);
		if (!cl) return false;
		{ GPU_PASS_SCOPE(immediate, "render pass compositor"); immediate->ExecuteCommandList(cl, TRUE); }
		cl->Release();
		return true;
	}

	bool CaptureColor(ID3D11Texture2D* color, Surface& dst) {
		D3D11_TEXTURE2D_DESC d;
		color->GetDesc(&d);
		const DXGI_FORMAT typed = TypedFromTypeless(d.Format);
		if (!dst.Ensure(m_Device, d.Width, d.Height, typed, typed, false)) return Fail("color: surface");
		if (d.SampleDesc.Count > 1) m_Ctx->ResolveSubresource(dst.tex, 0, color, 0, typed);
		else m_Ctx->CopyResource(dst.tex, color);
		return true;
	}

	bool CaptureDepth(ID3D11DepthStencilView* dsv, DepthSurface& dst) {
		ID3D11Resource* res = nullptr;
		dsv->GetResource(&res);
		if (!res) return Fail("depth: no resource");
		ID3D11Texture2D* tex = nullptr;
		res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
		res->Release();
		if (!tex) return Fail("depth: not a texture");
		D3D11_TEXTURE2D_DESC d;
		tex->GetDesc(&d);
		const DXGI_FORMAT srvFmt = DepthSrvFormat(d.Format);
		bool ok = false;
		if (srvFmt == DXGI_FORMAT_UNKNOWN) {
			Fail("depth: unsupported format");
		} else if (d.SampleDesc.Count > 1) {
			// Sample 0 of the MSAA buffer into an R32F target.
			ID3D11ShaderResourceView* src = nullptr;
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = srvFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
			if (SUCCEEDED(m_Device->CreateShaderResourceView(tex, &sd, &src)) && src
				&& dst.extracted.Ensure(m_Device, d.Width, d.Height, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true)) {
				ID3D11ShaderResourceView* srvs[1] = { src };
				Draw(m_PsDepthMs, dst.extracted.rtv, d.Width, d.Height, srvs, 1);
				dst.srv = dst.extracted.srv;
				ok = true;
			} else Fail("depth: msaa view");
			SafeRelease(src);
		} else {
			if (dst.copied.Ensure(m_Device, d.Width, d.Height, DepthTypeless(d.Format), srvFmt, false)) {
				m_Ctx->CopyResource(dst.copied.tex, tex);
				dst.srv = dst.copied.srv;
				ok = true;
			} else Fail("depth: copy surface");
		}
		tex->Release();
		return ok;
	}

	void Draw(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* const* srvs, UINT srvCount) {
		ID3D11RenderTargetView* rtvs[1] = { rtv };
		m_Ctx->OMSetRenderTargets(1, rtvs, nullptr);
		D3D11_VIEWPORT vp = {};
		vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.0f;
		m_Ctx->RSSetViewports(1, &vp);
		m_Ctx->IASetInputLayout(m_Layout);
		UINT stride = sizeof(Vertex), offset = 0;
		m_Ctx->IASetVertexBuffers(0, 1, &m_Vb, &stride, &offset);
		m_Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_Ctx->VSSetShader(m_Vs, nullptr, 0);
		m_Ctx->VSSetConstantBuffers(0, 1, &m_VsCb);
		m_Ctx->PSSetShader(ps, nullptr, 0);
		m_Ctx->PSSetConstantBuffers(0, 1, &m_PsCb);
		m_Ctx->PSSetShaderResources(0, srvCount, srvs);
		m_Ctx->RSSetState(m_Rs);
		m_Ctx->OMSetDepthStencilState(m_Ds, 0);
		const float bf[4] = {0, 0, 0, 0};
		m_Ctx->OMSetBlendState(m_Bs, bf, 0xffffffff);
		m_Ctx->Draw(4, 0);
		ID3D11ShaderResourceView* nulls[5] = {};
		m_Ctx->PSSetShaderResources(0, srvCount, nulls);
		ID3D11RenderTargetView* nullRt[1] = {};
		m_Ctx->OMSetRenderTargets(1, nullRt, nullptr);
	}

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_Ctx = nullptr;
	ID3D11VertexShader* m_Vs = nullptr;
	ID3D11InputLayout* m_Layout = nullptr;
	ID3D11PixelShader* m_PsDepthMs = nullptr;
	ID3D11PixelShader* m_PsPass = nullptr;
	ID3D11Buffer* m_Vb = nullptr;
	ID3D11Buffer* m_VsCb = nullptr;
	ID3D11Buffer* m_PsCb = nullptr;
	ID3D11RasterizerState* m_Rs = nullptr;
	ID3D11DepthStencilState* m_Ds = nullptr;
	ID3D11BlendState* m_Bs = nullptr;

	Surface m_Color, m_RefColor, m_Out;
	DepthSurface m_Depth, m_RefDepth, m_WorldDepth;
	bool m_RefValid = false;
	bool m_WorldRefValid = false;
	bool m_Reversed = false;
};

Compositor g_Compositor;

} // namespace

void BeginDevice(ID3D11Device* device) { g_Compositor.BeginDevice(device); }
void EndDevice() { g_Compositor.EndDevice(); }

bool StoreReference(ID3D11DeviceContext* immediate, ID3D11Texture2D* color, ID3D11DepthStencilView* depth) {
	return g_Compositor.StoreReference(immediate, color, depth);
}

bool StoreWorldReference(ID3D11DeviceContext* immediate, ID3D11DepthStencilView* depth) {
	return g_Compositor.StoreWorldReference(immediate, depth);
}

ID3D11Texture2D* Compose(ID3D11DeviceContext* immediate, ID3D11Texture2D* color,
	ID3D11DepthStencilView* depth, const ComposeParams& params) {
	return g_Compositor.Compose(immediate, color, depth, params);
}

bool ComposeInto(ID3D11DeviceContext* immediate, ID3D11Texture2D* color,
	ID3D11DepthStencilView* depth, const ComposeParams& params, ID3D11RenderTargetView* target) {
	return target && g_Compositor.Compose(immediate, color, depth, params, target) != nullptr;
}

} // namespace AfxRenderPass
