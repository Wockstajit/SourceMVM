#include "DeadlockViewportBlit.h"

#include "DeadlockBlitShaders.h"

#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"

#include <cstring>

namespace Deadlock {

namespace {

// A TYPELESS backbuffer cannot back a shader-resource view. Normally the render-target view's own
// typed format is used; this is the fallback for the case where the view reports UNKNOWN.
DXGI_FORMAT TypedFromTypeless(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:                                return f;
	}
}

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_deferred = nullptr;

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_rasterizer = nullptr;
ID3D11DepthStencilState* g_depthStencil = nullptr;
ID3D11BlendState* g_blend = nullptr;

ID3D11Texture2D* g_temp = nullptr;
ID3D11ShaderResourceView* g_tempSrv = nullptr;
UINT g_tempW = 0, g_tempH = 0;
DXGI_FORMAT g_tempFormat = DXGI_FORMAT_UNKNOWN;

bool g_active = false;
float g_rect[4] = { 0, 0, 1, 1 };

unsigned long long g_blits = 0;
unsigned long long g_skips = 0;
unsigned long long g_viewportRestores = 0;
UINT g_lastBbW = 0, g_lastBbH = 0;
float g_lastVp[4] = { 0, 0, 0, 0 };

void ReleaseTemp() {
	if (g_tempSrv) { g_tempSrv->Release(); g_tempSrv = nullptr; }
	if (g_temp) { g_temp->Release(); g_temp = nullptr; }
	g_tempW = g_tempH = 0;
	g_tempFormat = DXGI_FORMAT_UNKNOWN;
}

// The temp texture must match the backbuffer's size and the view's typed format, and it is always
// single-sampled: an MSAA backbuffer is RESOLVED into it rather than copied.
bool EnsureTemp(const D3D11_TEXTURE2D_DESC& bb, DXGI_FORMAT viewFormat) {
	if (g_temp && g_tempW == bb.Width && g_tempH == bb.Height && g_tempFormat == viewFormat)
		return true;
	ReleaseTemp();
	if (!g_device) return false;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = bb.Width;
	td.Height = bb.Height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = viewFormat;
	td.SampleDesc.Count = 1;
	td.SampleDesc.Quality = 0;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_temp)) || !g_temp) {
		ReleaseTemp();
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = viewFormat;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	if (FAILED(g_device->CreateShaderResourceView(g_temp, &sd, &g_tempSrv)) || !g_tempSrv) {
		ReleaseTemp();
		return false;
	}

	g_tempW = bb.Width;
	g_tempH = bb.Height;
	g_tempFormat = viewFormat;
	return true;
}

} // namespace

bool ViewportBlit_Ready() {
	return g_device && g_deferred && g_vs && g_ps
		&& g_sampler && g_rasterizer && g_depthStencil && g_blend;
}

void ViewportBlit_BeginDevice(ID3D11Device* device) {
	ViewportBlit_EndDevice();
	if (!device) return;

	device->AddRef();
	g_device = device;
	g_device->CreateDeferredContext(0, &g_deferred);

	// The vertex shader synthesises a full-screen triangle from SV_VertexID. Never bind a vertex
	// buffer here: Deadlock retains callback bindings in an internal 16-bit-indexed list.
	g_device->CreateVertexShader(kDeadlockBlitTriangleVs, kDeadlockBlitTriangleVsSize, nullptr, &g_vs);
	g_device->CreatePixelShader(kDeadlockBlitPs, kDeadlockBlitPsSize, nullptr, &g_ps);

	{
		D3D11_SAMPLER_DESC sd = {};
		// Linear, because this is a downscale and point sampling would alias the whole picture.
		// Clamped, so the quad's edge texels cannot wrap in from the far side.
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		g_device->CreateSamplerState(&sd, &g_sampler);
	}
	{
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		g_device->CreateRasterizerState(&rd, &g_rasterizer);
	}
	{
		// Depth off: this is a full-screen quad over a finished frame, and any depth state left
		// bound by the world pass would reject it.
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		g_device->CreateDepthStencilState(&dd, &g_depthStencil);
	}
	{
		// Opaque overwrite. The copy already contains everything; blending it would double the
		// frame against whatever the clear left behind.
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		g_device->CreateBlendState(&bd, &g_blend);
	}

	advancedfx::Message("AfxHookDeadlock: viewport blit device %s.\n",
	                    ViewportBlit_Ready() ? "ready" : "INCOMPLETE");
}

void ViewportBlit_EndDevice() {
	ReleaseTemp();
	if (g_blend) { g_blend->Release(); g_blend = nullptr; }
	if (g_depthStencil) { g_depthStencil->Release(); g_depthStencil = nullptr; }
	if (g_rasterizer) { g_rasterizer->Release(); g_rasterizer = nullptr; }
	if (g_sampler) { g_sampler->Release(); g_sampler = nullptr; }
	if (g_ps) { g_ps->Release(); g_ps = nullptr; }
	if (g_vs) { g_vs->Release(); g_vs = nullptr; }
	if (g_deferred) { g_deferred->Release(); g_deferred = nullptr; }
	if (g_device) { g_device->Release(); g_device = nullptr; }
}

bool g_suppressed = false;

void ViewportBlit_SetRequest(bool active, float x0, float y0, float x1, float y1) {
	const bool changed = g_active != active || g_rect[0] != x0 || g_rect[1] != y0
		|| g_rect[2] != x1 || g_rect[3] != y1;
	g_rect[0] = x0;
	g_rect[1] = y0;
	g_rect[2] = x1;
	g_rect[3] = y1;
	g_active = active;
	if (changed && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("deadlock.blit", "request active=%d rect=%.5f %.5f %.5f %.5f",
			active ? 1 : 0, x0, y0, x1, y1);
}

bool ViewportBlit_Active() { return g_active; }

void ViewportBlit_SetSuppressed(bool suppressed) { g_suppressed = suppressed; }
bool ViewportBlit_Suppressed() { return g_suppressed; }

void ViewportBlit_RunPreUi() {
	if (g_suppressed || !g_active || !ViewportBlit_Ready()) return;

	// The immediate context and whatever render target is bound RIGHT NOW. At the pre-UI moment
	// that is the target the world was just drawn into, which is what we want to resample --
	// asking the swapchain for its backbuffer instead would be a guess about which target the
	// engine happened to be using.
	ID3D11DeviceContext* immediate = nullptr;
	g_device->GetImmediateContext(&immediate);
	if (!immediate) { ++g_skips; return; }

	ID3D11RenderTargetView* target = nullptr;
	immediate->OMGetRenderTargets(1, &target, nullptr);
	if (!target) { immediate->Release(); ++g_skips; return; }

	ID3D11Resource* resource = nullptr;
	target->GetResource(&resource);
	ID3D11Texture2D* backbuffer = nullptr;
	if (resource)
		resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backbuffer);

	if (backbuffer) {
		D3D11_TEXTURE2D_DESC bb = {};
		backbuffer->GetDesc(&bb);

		D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
		target->GetDesc(&rtv);
		DXGI_FORMAT viewFormat = rtv.Format;
		if (viewFormat == DXGI_FORMAT_UNKNOWN) viewFormat = TypedFromTypeless(bb.Format);

		float x0 = g_rect[0], y0 = g_rect[1], x1 = g_rect[2], y1 = g_rect[3];
		auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
		x0 = clamp01(x0); y0 = clamp01(y0); x1 = clamp01(x1); y1 = clamp01(y1);

		// A degenerate rect is refused rather than drawn. Scaling the frame into four pixels is
		// not a preview, and it would also hide whatever produced the bad rectangle.
		if (x1 - x0 >= 0.001f && y1 - y0 >= 0.001f && EnsureTemp(bb, viewFormat)) {
			const float fw = (float)bb.Width, fh = (float)bb.Height;

			// 1. Snapshot the finished frame. MSAA has to be resolved; a plain copy of a
			//    multisampled resource into a single-sampled one is invalid.
			if (bb.SampleDesc.Count > 1)
				g_deferred->ResolveSubresource(g_temp, 0, backbuffer, 0, viewFormat);
			else
				g_deferred->CopyResource(g_temp, backbuffer);

			// 2. Clear. THIS is what the viewport-write approach could never do, and why it left
			//    stale frames showing through everywhere the world no longer reached.
			const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			g_deferred->ClearRenderTargetView(target, black);

			// 3. Draw the snapshot back, scaled into the rect.
			ID3D11RenderTargetView* rtvs[1] = { target };
			g_deferred->OMSetRenderTargets(1, rtvs, nullptr);

			D3D11_VIEWPORT vp = {};
			vp.TopLeftX = x0 * fw;
			vp.TopLeftY = y0 * fh;
			vp.Width = (x1 - x0) * fw;
			vp.Height = (y1 - y0) * fh;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			g_deferred->RSSetViewports(1, &vp);

			g_deferred->IASetInputLayout(nullptr);
			g_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g_deferred->VSSetShader(g_vs, nullptr, 0);
			g_deferred->PSSetShader(g_ps, nullptr, 0);
			g_deferred->PSSetShaderResources(0, 1, &g_tempSrv);
			g_deferred->PSSetSamplers(0, 1, &g_sampler);
			g_deferred->RSSetState(g_rasterizer);
			g_deferred->OMSetDepthStencilState(g_depthStencil, 0);
			const float blendFactor[4] = { 0, 0, 0, 0 };
			g_deferred->OMSetBlendState(g_blend, blendFactor, 0xffffffff);
			g_deferred->Draw(3, 0);

			// Replay on the immediate context, RESTORING its state. Without the restore, the UI
			// pass that runs next inherits our shaders and viewport and draws Panorama into the
			// preview rect with a blit shader bound.
			ID3D11CommandList* commands = nullptr;
			g_deferred->FinishCommandList(FALSE, &commands);
			if (commands) {
				immediate->ExecuteCommandList(commands, TRUE);
				commands->Release();
			}

			g_lastBbW = bb.Width;
			g_lastBbH = bb.Height;
			g_lastVp[0] = vp.TopLeftX; g_lastVp[1] = vp.TopLeftY;
			g_lastVp[2] = vp.Width;    g_lastVp[3] = vp.Height;
			++g_blits;
			if (Filmmaker::MvmDebugLog_Active() && (g_blits == 1 || 0 == (g_blits % 600)))
				Filmmaker::MvmDebugLog_Linef("deadlock.blit",
					"run count=%llu target=%ux%u samples=%u viewport=%.0f %.0f %.0f %.0f",
					g_blits, bb.Width, bb.Height, bb.SampleDesc.Count,
					vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
		} else {
			++g_skips;
		}
		backbuffer->Release();
	} else {
		++g_skips;
	}

	if (resource) resource->Release();
	target->Release();
	immediate->Release();
}

void ViewportBlit_RestoreFullViewportPreUi() {
	if (!g_device) return;
	ID3D11DeviceContext* immediate = nullptr;
	g_device->GetImmediateContext(&immediate);
	if (!immediate) return;

	ID3D11RenderTargetView* target = nullptr;
	immediate->OMGetRenderTargets(1, &target, nullptr);
	if (target) {
		ID3D11Resource* resource = nullptr;
		target->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
		if (texture) {
			D3D11_TEXTURE2D_DESC desc = {};
			texture->GetDesc(&desc);
			if (desc.Width > 0 && desc.Height > 0) {
				D3D11_VIEWPORT viewport = {};
				viewport.Width = (float)desc.Width;
				viewport.Height = (float)desc.Height;
				viewport.MaxDepth = 1.0f;
				immediate->RSSetViewports(1, &viewport);
				D3D11_RECT scissor = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
				immediate->RSSetScissorRects(1, &scissor);
				++g_viewportRestores;
			}
			texture->Release();
		}
		if (resource) resource->Release();
		target->Release();
	}
	immediate->Release();
}

void ViewportBlit_Report() {
	if (g_suppressed)
		advancedfx::Warning("  blit:      SUPPRESSED by 'mirv_filmmaker blit off' -- the frame is untouched.\n");
	advancedfx::Message(
		"  blit:      %s  active=%s  blits=%llu skipped=%llu  preUiViewportRestores=%llu\n",
		ViewportBlit_Ready() ? "ready" : "NOT ready", g_active ? "yes" : "no",
		g_blits, g_skips, g_viewportRestores);
	if (g_blits)
		advancedfx::Message(
			"             last: backbuffer %ux%u -> viewport %.0f,%.0f %.0fx%.0f\n",
			g_lastBbW, g_lastBbH, g_lastVp[0], g_lastVp[1], g_lastVp[2], g_lastVp[3]);
}

} // namespace Deadlock
