#define NOMINMAX
#include "LensFx.h"
#include "../Debug/GpuPassProfiler.h"
#include "LensFxShaders.h"
#include "LensFxConstants.h"

#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Environment/EnvironmentMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Filmmaker {

using Microsoft::WRL::ComPtr;
using namespace Lens;

namespace {

using MaskImage = LensMaskImage;

// ================================================================================================
// GPU
// ================================================================================================

using Constants = GpuConstants;

struct PreparedShader {
	std::string entry;
	int depthMs = 0, colorMs = 0;
	ComPtr<ID3DBlob> code;
};

const std::string& ShaderSource() {
	static const std::string source = std::string(kShaderCommon) + kShaderDof + kShaderFlare + kShaderComposite;
	return source;
}

struct Request { const char* entry; const char* profile; int dms, cms; };
std::vector<Request> ShaderRequests() {
	std::vector<Request> r{ { "VS", "vs_5_0", 0, 0 } };
	for (int ms = 0; ms < 2; ++ms) {
		r.push_back({ "FocusPS", "ps_5_0", ms, 0 });
		r.push_back({ "CocPS", "ps_5_0", ms, 0 });
		r.push_back({ "SunPS", "ps_5_0", ms, 0 });
	}
	for (const char* e : { "MipPS", "TilePS", "DilateHPS", "DilateVPS", "GatherPS", "DenoisePS",
		"BrightPS", "Down13PS", "SourcePS", "SourceHPS", "ApertureBlurPS", "GhostPS", "StreakPS", "GlowHPS", "GlowVPS" })
		r.push_back({ e, "ps_5_0", 0, 0 });
	for (int cm = 0; cm < 2; ++cm) r.push_back({ "CompositePS", "ps_5_0", 0, cm });
	return r;
}

bool CompileOne(const Request& req, PreparedShader& out) {
	D3D_SHADER_MACRO macros[] = { { "DEPTH_MS", req.dms ? "1" : "0" }, { "COLOR_MS", req.cms ? "1" : "0" }, { nullptr, nullptr } };
	const std::string& src = ShaderSource();
	ComPtr<ID3DBlob> errors;
	bool cached = false;
	const HRESULT hr = Environment::CompileEnvironmentShader(src.data(), src.size(), "LensShaders", macros, req.entry,
		req.profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &out.code, &errors, &cached);
	if (FAILED(hr)) {
		MvmDebugLog_LinefAlways("lens.shader", "FAILED entry=%s hr=%08x %s", req.entry, (unsigned)hr,
			errors ? (const char*)errors->GetBufferPointer() : "");
		return false;
	}
	out.entry = req.entry; out.depthMs = req.dms; out.colorMs = req.cms;
	return true;
}

std::vector<PreparedShader> PrepareShaders() {
	const auto requests = ShaderRequests();
	std::vector<PreparedShader> result(requests.size());
	std::atomic<size_t> next{ 0 };
	std::atomic<bool> failed{ false };
	const ULONGLONG started = GetTickCount64();
	const unsigned workers = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
	std::vector<std::future<void>> jobs;
	for (unsigned w = 0; w < workers; ++w)
		jobs.push_back(std::async(std::launch::async, [&] {
			for (;;) {
				const size_t i = next.fetch_add(1);
				if (i >= requests.size()) break;
				if (!CompileOne(requests[i], result[i])) failed = true;
			}
		}));
	for (auto& j : jobs) j.get();
	MvmDebugLog_LinefAlways("lens.shader", "batch ready count=%zu failed=%d ms=%llu", requests.size(), (int)failed.load(),
		GetTickCount64() - started);
	if (failed) throw std::runtime_error("lens shader compilation failed");
	return result;
}

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx; // deferred
	bool ready = false;

	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> focusPS[2], cocPS[2], compositePS[2], sunPS[2];
	ComPtr<ID3D11PixelShader> mipPS, tilePS, dilateH, dilateV, gatherPS, denoisePS;
	ComPtr<ID3D11PixelShader> brightPS, down13PS, sourcePS, sourceHPS, apertureBlurPS, ghostPS, streakPS, glowH, glowV;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> linear;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> noDepth;
	ComPtr<ID3D11BlendState> opaque;

	struct Tex {
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11RenderTargetView> rtv;
		UINT w = 0, h = 0;
	};
	// Sized to the target; rebuilt when its size, format or sample count changes.
	UINT tw = 0, th = 0, tSamples = 0, tQuality = 0, dw = 0, dh = 0, dSamples = 0, dQuality = 0;
	bool targetsReady = false;
	DXGI_FORMAT tFormat = DXGI_FORMAT_UNKNOWN, dFormat = DXGI_FORMAT_UNKNOWN;
	Tex srcRes, msaaCopy, depthCopy, soft, gather;
	Tex tile[2];
	// The flare chain: bright pass (half) -> quarter -> eighth, the isolated source map (quarter and
	// eighth), the aperture blobs (eighth), then the ghost/halo, streak/spike and glow layers.
	Tex halfBright, quarterBright, eighthBright, sourceQ, sourceE, sourceH, ghostSrc, ghostOut, streakOut, glowA, glowB;
	ComPtr<ID3D11Texture2D> coc;
	ComPtr<ID3D11ShaderResourceView> cocAll, cocMip[kCocMipLevels];
	ComPtr<ID3D11RenderTargetView> cocRtv[kCocMipLevels];
	// 1 world, 0 the sharp first-person weapon, with the same mips: the gather weights every tap by it
	// (see CocOut in LensFxShaders.h).
	ComPtr<ID3D11Texture2D> worldMask;
	ComPtr<ID3D11ShaderResourceView> worldAll, worldMip[kCocMipLevels];
	ComPtr<ID3D11RenderTargetView> worldRtv[kCocMipLevels];
	int cocLevels = 1; // mip levels this target size can hold (at most kCocMipLevels)
	Tex focus[2];
	Tex sunVis; // 1x1: how much of the sun's disc is open sky (SunPS)
	int focusCur = 0;
	bool focusValid = false;
	ComPtr<ID3D11Texture2D> focusStaging;

	Tex irisTex, dirtTex;
	unsigned irisUploaded = 0, dirtUploaded = 0;
	bool irisLoaded = false, dirtLoaded = false;

	std::chrono::steady_clock::time_point lastFrame;
	bool haveLastFrame = false;
	unsigned probeCounter = 0;
} g;

std::vector<PreparedShader> g_prepared;
std::future<std::vector<PreparedShader>> g_pending;
bool g_devicePending = false;
bool g_prepFailed = false;

const PreparedShader* Find(const char* entry, int dms, int cms) {
	for (const auto& p : g_prepared)
		if (p.entry == entry && p.depthMs == dms && p.colorMs == cms) return &p;
	return nullptr;
}
template <class T>
bool MakePs(const char* entry, int dms, int cms, ComPtr<T>& out) {
	const PreparedShader* p = Find(entry, dms, cms);
	return p && SUCCEEDED(g.device->CreatePixelShader(p->code->GetBufferPointer(), p->code->GetBufferSize(), nullptr, &out));
}

bool CreateObjects() {
	const PreparedShader* vs = Find("VS", 0, 0);
	if (!vs || FAILED(g.device->CreateVertexShader(vs->code->GetBufferPointer(), vs->code->GetBufferSize(), nullptr, &g.vs)))
		return false;
	for (int ms = 0; ms < 2; ++ms) {
		if (!MakePs("FocusPS", ms, 0, g.focusPS[ms]) || !MakePs("CocPS", ms, 0, g.cocPS[ms]) || !MakePs("SunPS", ms, 0, g.sunPS[ms])) return false;
		if (!MakePs("CompositePS", 0, ms, g.compositePS[ms])) return false;
	}
	if (!MakePs("MipPS", 0, 0, g.mipPS) || !MakePs("TilePS", 0, 0, g.tilePS) || !MakePs("DilateHPS", 0, 0, g.dilateH) ||
		!MakePs("DilateVPS", 0, 0, g.dilateV) || !MakePs("GatherPS", 0, 0, g.gatherPS) ||
		!MakePs("DenoisePS", 0, 0, g.denoisePS) ||
		!MakePs("BrightPS", 0, 0, g.brightPS) || !MakePs("Down13PS", 0, 0, g.down13PS) || !MakePs("SourcePS", 0, 0, g.sourcePS) || !MakePs("SourceHPS", 0, 0, g.sourceHPS) ||
		!MakePs("ApertureBlurPS", 0, 0, g.apertureBlurPS) || !MakePs("GhostPS", 0, 0, g.ghostPS) ||
		!MakePs("StreakPS", 0, 0, g.streakPS) || !MakePs("GlowHPS", 0, 0, g.glowH) || !MakePs("GlowVPS", 0, 0, g.glowV))
		return false;

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(Constants);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(g.device->CreateBuffer(&bd, nullptr, &g.cb))) return false;

	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(g.device->CreateSamplerState(&sd, &g.linear))) return false;

	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	rd.MultisampleEnable = TRUE;
	if (FAILED(g.device->CreateRasterizerState(&rd, &g.raster))) return false;

	D3D11_DEPTH_STENCIL_DESC dd{};
	dd.DepthEnable = FALSE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	if (FAILED(g.device->CreateDepthStencilState(&dd, &g.noDepth))) return false;

	D3D11_BLEND_DESC bl{};
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(g.device->CreateBlendState(&bl, &g.opaque))) return false;

	if (FAILED(g.device->CreateDeferredContext(0, &g.ctx))) return false;
	return true;
}


DXGI_FORMAT TypedFrom(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:                                return f;
	}
}

bool MakeTarget(Gpu::Tex& t, UINT w, UINT h, DXGI_FORMAT fmt, bool renderTarget = true, bool mipmapped = false) {
	t = Gpu::Tex{};
	t.w = std::max(w, 1u);
	t.h = std::max(h, 1u);
	D3D11_TEXTURE2D_DESC d{};
	d.Width = t.w; d.Height = t.h; d.MipLevels = mipmapped ? 0 : 1; d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
	d.MiscFlags = mipmapped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
	if (FAILED(g.device->CreateTexture2D(&d, nullptr, &t.tex)) || FAILED(g.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))
		return false;
	if (renderTarget && FAILED(g.device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return false;
	return true;
}

bool UploadMask(Gpu::Tex& t, const MaskImage& m) {
	t = Gpu::Tex{};
	if (m.pixels.empty() || m.width == 0 || m.height == 0) return false;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = m.width; d.Height = m.height; d.MipLevels = 1; d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8_UNORM;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_IMMUTABLE;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{ m.pixels.data(), m.width, 0 };
	if (FAILED(g.device->CreateTexture2D(&d, &init, &t.tex)) || FAILED(g.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))
		return false;
	t.w = m.width; t.h = m.height;
	return true;
}

// Everything that depends on the target's size, format or sample count. Returns false if any
// allocation failed, in which case nothing has been drawn and the frame is left as the engine made it.
bool EnsureTargets(const D3D11_TEXTURE2D_DESC& td, const D3D11_TEXTURE2D_DESC* dd, DXGI_FORMAT fmt) {
	const DXGI_FORMAT dFmt = dd ? dd->Format : DXGI_FORMAT_UNKNOWN;
	const UINT dSamples = dd ? dd->SampleDesc.Count : 0;
	if (g.targetsReady && g.tw == td.Width && g.th == td.Height && g.tSamples == td.SampleDesc.Count &&
		g.tQuality == td.SampleDesc.Quality && g.tFormat == fmt && g.dw == (dd ? dd->Width : 0) &&
		g.dh == (dd ? dd->Height : 0) && g.dFormat == dFmt && g.dSamples == dSamples &&
		g.dQuality == (dd ? dd->SampleDesc.Quality : 0))
		return true;

	g.targetsReady = false;
	g.srcRes = g.msaaCopy = g.depthCopy = g.soft = g.gather = Gpu::Tex{};
	g.tile[0] = g.tile[1] = g.halfBright = g.quarterBright = g.eighthBright = Gpu::Tex{};
	g.sourceQ = g.sourceE = g.sourceH = Gpu::Tex{};
	g.ghostSrc = g.ghostOut = g.streakOut = g.glowA = g.glowB = Gpu::Tex{};
	g.coc.Reset(); g.cocAll.Reset(); g.worldMask.Reset(); g.worldAll.Reset();
	for (int i = 0; i < kCocMipLevels; ++i) { g.cocMip[i].Reset(); g.cocRtv[i].Reset(); g.worldMip[i].Reset(); g.worldRtv[i].Reset(); }
	g.focus[0] = g.focus[1] = Gpu::Tex{};
	g.focusStaging.Reset();
	g.focusValid = false;

	const UINT w = td.Width, h = td.Height;
	const DXGI_FORMAT hdr = DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!MakeTarget(g.srcRes, w, h, fmt, false)) return false;
	if (td.SampleDesc.Count > 1) {
		D3D11_TEXTURE2D_DESC d = td;
		d.Format = fmt; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		d.CPUAccessFlags = 0; d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = fmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
		if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.msaaCopy.tex)) ||
			FAILED(g.device->CreateShaderResourceView(g.msaaCopy.tex.Get(), &sv, &g.msaaCopy.srv)))
			return false;
	}
	if (dd) {
		D3D11_TEXTURE2D_DESC d = *dd;
		d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
		DXGI_FORMAT view;
		switch (d.Format) {
		case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
			d.Format = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; break;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
			d.Format = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
		case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
			d.Format = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; break;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
			d.Format = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
		default: return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = view;
		sv.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.depthCopy.tex)) ||
			FAILED(g.device->CreateShaderResourceView(g.depthCopy.tex.Get(), &sv, &g.depthCopy.srv)))
			return false;
	}

	// The blur radius / linear-colour image carries a mip chain (up to kCocMipLevels) the wide gather
	// reads from; a small target cannot hold all of them.
	{
		int levels = 1;
		while (levels < kCocMipLevels && ((std::min)(w, h) >> levels) >= 1) ++levels;
		g.cocLevels = levels;
		auto chain = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& all,
			ComPtr<ID3D11ShaderResourceView>* mips, ComPtr<ID3D11RenderTargetView>* rtvs) {
			D3D11_TEXTURE2D_DESC d{};
			d.Width = w; d.Height = h; d.MipLevels = (UINT)levels; d.ArraySize = 1; d.Format = format;
			d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (FAILED(g.device->CreateTexture2D(&d, nullptr, &tex)) ||
				FAILED(g.device->CreateShaderResourceView(tex.Get(), nullptr, &all)))
				return false;
			for (int m = 0; m < levels; ++m) {
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MostDetailedMip = m; sv.Texture2D.MipLevels = 1;
				D3D11_RENDER_TARGET_VIEW_DESC rv{};
				rv.Format = format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rv.Texture2D.MipSlice = m;
				if (FAILED(g.device->CreateShaderResourceView(tex.Get(), &sv, &mips[m])) ||
					FAILED(g.device->CreateRenderTargetView(tex.Get(), &rv, &rtvs[m])))
					return false;
			}
			return true;
		};
		if (!chain(hdr, g.coc, g.cocAll, g.cocMip, g.cocRtv) ||
			!chain(DXGI_FORMAT_R16_FLOAT, g.worldMask, g.worldAll, g.worldMip, g.worldRtv))
			return false;
	}
	const UINT gw = (w + 15) / 16, gh = (h + 15) / 16;
	if (!MakeTarget(g.tile[0], gw, gh, DXGI_FORMAT_R16_FLOAT) || !MakeTarget(g.tile[1], gw, gh, DXGI_FORMAT_R16_FLOAT) ||
		!MakeTarget(g.gather, w, h, hdr) || !MakeTarget(g.soft, w, h, hdr) ||
		!MakeTarget(g.halfBright, (w + 1) / 2, (h + 1) / 2, hdr) ||
		!MakeTarget(g.quarterBright, (w + 3) / 4, (h + 3) / 4, hdr) ||
		!MakeTarget(g.eighthBright, (w + 7) / 8, (h + 7) / 8, hdr) ||
		!MakeTarget(g.sourceQ, (w + 3) / 4, (h + 3) / 4, hdr) || !MakeTarget(g.sourceE, (w + 7) / 8, (h + 7) / 8, hdr, true, true) ||
		!MakeTarget(g.sourceH, (w + 7) / 8, (h + 1) / 2, hdr) ||
		!MakeTarget(g.ghostSrc, (w + 3) / 4, (h + 3) / 4, hdr) ||
		!MakeTarget(g.ghostOut, (w + 3) / 4, (h + 3) / 4, hdr) ||
		!MakeTarget(g.streakOut, (w + 1) / 2, (h + 1) / 2, hdr) ||
		!MakeTarget(g.glowA, (w + 7) / 8, (h + 7) / 8, hdr) || !MakeTarget(g.glowB, (w + 7) / 8, (h + 7) / 8, hdr) ||
		!MakeTarget(g.focus[0], 1, 1, DXGI_FORMAT_R32_FLOAT) || !MakeTarget(g.focus[1], 1, 1, DXGI_FORMAT_R32_FLOAT) ||
		!MakeTarget(g.sunVis, 1, 1, DXGI_FORMAT_R16_FLOAT))
		return false;
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = d.Height = 1; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT;
		d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		g.device->CreateTexture2D(&d, nullptr, &g.focusStaging); // diagnostics only
	}
	g.tw = w; g.th = h; g.tSamples = td.SampleDesc.Count; g.tQuality = td.SampleDesc.Quality; g.tFormat = fmt;
	g.dw = dd ? dd->Width : 0; g.dh = dd ? dd->Height : 0; g.dFormat = dFmt; g.dSamples = dSamples;
	g.dQuality = dd ? dd->SampleDesc.Quality : 0; g.targetsReady = true;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("lens.targets", "target=%ux%u samples=%u quality=%u depthCopy=%ux%u depthSamples=%u depthQuality=%u",
			g.tw, g.th, g.tSamples, g.tQuality, g.dw, g.dh, g.dSamples, g.dQuality);
	return true;
}

bool PumpDevice() {
	if (g.ready) return true;
	if (g_devicePending && g_pending.valid() && g_pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		g_devicePending = false;
		try {
			g_prepared = g_pending.get();
			if (g.device && CreateObjects()) {
				g.ready = true;
				LensFx_SetRenderStatus("Ready");
				MvmDebugLog_LinefAlways("lens.shader", "device ready");
			} else {
				g_prepFailed = true;
				LensFx_SetRenderStatus("Could not create lens render objects");
			}
		} catch (const std::exception& e) {
			g_prepFailed = true;
			LensFx_SetRenderStatus("Lens shader preparation failed; see mvm_debug");
			MvmDebugLog_LinefAlways("lens.shader", "background preparation failed: %s", e.what());
		}
	}
	return g.ready;
}

} // namespace

// ================================================================================================
// Render thread
// ================================================================================================

void LensFx_BeginDevice(ID3D11Device* device) {
	if (device && g.device.Get() == device && (g.ready || g_devicePending)) return;
	LensFx_EndDevice();
	if (!device) return;
	g.device = device;
	if (g_prepared.empty()) {
		if (g_prepFailed) return;
		g_devicePending = true;
		if (!g_pending.valid()) g_pending = std::async(std::launch::async, PrepareShaders);
		LensFx_SetRenderStatus("Preparing lens shaders in the background");
		return;
	}
	if (CreateObjects()) { g.ready = true; LensFx_SetRenderStatus("Ready"); }
	else LensFx_SetRenderStatus("Could not create lens render objects");
}

void LensFx_EndDevice() {
	g = Gpu{};
	g_devicePending = false;
}

bool LensFx_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth,
	const float* worldToClip, float depthMin, float depthMax, const LensSmoke& smoke, const LensSun& sun,
	bool keyPass, bool holdFocus) {
	const LensRenderState state = LensFx_ReadRenderState(g.irisUploaded,g.dirtUploaded);
    const Values& L = state.values;
    const MaskImage& irisPending = state.iris;
    const MaskImage& dirtPending = state.dirt;
    const bool irisChanged = state.irisChanged, dirtChanged = state.dirtChanged;
	bool dofOn = L[DofOn] > 0.5f;
	const bool flareWanted = L[FlareOn] > 0.5f;
	// A key (green screen) frame keeps its chroma clean: the flare is additive light over the
	// whole picture, so it would tint the background and the subject's edges (seen as a green
	// halo around a scoped rifle, 2026-10-20). It comes back on the next non-key frame.
	bool flareOn = flareWanted && !keyPass;
	if (flareWanted && keyPass && MvmDebugLog_Active()) {
		static unsigned s_held = 0;
		if ((s_held++ % 120) == 0)
			MvmDebugLog_LinefAlways("lens.flare", "held back: key (green screen) pass, dof=%d frames=%u", (int)dofOn, s_held);
	}
	if (!dofOn && !flareOn) {
		g.focusValid = false;
		LensFx_SetRenderStatus(flareWanted ? "Flare held back on the green screen pass" : "Off");
		return false;
	}
	if (!immediate || !target) return false;
	if (!PumpDevice()) return false;

	ComPtr<ID3D11Resource> targetRes;
	target->GetResource(&targetRes);
	ComPtr<ID3D11Texture2D> targetTex;
	if (!targetRes || FAILED(targetRes.As(&targetTex))) return false;
	D3D11_TEXTURE2D_DESC td{};
	targetTex->GetDesc(&td);
	D3D11_RENDER_TARGET_VIEW_DESC rv{};
	target->GetDesc(&rv);
	const DXGI_FORMAT fmt = rv.Format != DXGI_FORMAT_UNKNOWN ? rv.Format : TypedFrom(td.Format);
	if (td.ArraySize != 1 || td.Width < 16 || td.Height < 16) return false;

	// Depth: without a valid world depth the blur has nothing to follow, so DOF sits out the frame
	// (the flare only needs colour).
	ComPtr<ID3D11Texture2D> depthTex;
	D3D11_TEXTURE2D_DESC dd{};
	Environment::Matrix w2c{}, inv{};
	bool haveDepth = false;
	if (depth && worldToClip && depthMax > depthMin && depthMin >= 0 && depthMax <= 1) {
		ComPtr<ID3D11Resource> dr;
		depth->GetResource(&dr);
		if (dr && SUCCEEDED(dr.As(&depthTex))) {
			depthTex->GetDesc(&dd);
			for (int i = 0; i < 16; ++i) w2c[i] = worldToClip[i];
			haveDepth = dd.ArraySize == 1 && Environment::Inverse(w2c, inv);
		}
	}
	if (dofOn && !haveDepth) {
		dofOn = false;
		LensFx_SetRenderStatus("Waiting for world depth");
		if (!flareOn) return false;
	}

	if (!EnsureTargets(td, haveDepth ? &dd : nullptr, fmt)) {
		LensFx_SetRenderStatus("Could not allocate lens surfaces");
		return false;
	}
	if (irisChanged) {
		g.irisLoaded = UploadMask(g.irisTex, irisPending);
		g.irisUploaded = irisPending.serial;
	}
	if (dirtChanged) {
		g.dirtLoaded = UploadMask(g.dirtTex, dirtPending);
		g.dirtUploaded = dirtPending.serial;
	}

	const float W = (float)td.Width, H = (float)td.Height;
	const bool autoFocus = dofOn && L[DofAuto] > 0.5f;
	if (!autoFocus) g.focusValid = false;
	// A held render reads the focus the Main render settled on; with none yet it finds its own.
	const bool advanceFocus = autoFocus && !(holdFocus && g.focusValid);

	const auto now = std::chrono::steady_clock::now();
	float dt = g.haveLastFrame ? std::chrono::duration<float>(now - g.lastFrame).count() : 0.0f;
	if (!holdFocus || advanceFocus) { g.lastFrame = now; g.haveLastFrame = true; }
	dt = std::min(std::max(dt, 0.0f), 0.25f);
	const UINT hw = g.halfBright.w, hh = g.halfBright.h, qw = g.quarterBright.w, qh = g.quarterBright.h;
	const UINT ew = g.eighthBright.w, eh = g.eighthBright.h;

    Constants k=MakeGpuConstants(L,td.Width,td.Height,dd.Width,dd.Height,inv.data(),dt,g.focusValid,
        g.irisLoaded,g.dirtLoaded,dofOn,flareOn,autoFocus);
    k.depthInfo = { depthMin, depthMax, 0, 0 };
    const UINT gw=g.tile[0].w,gh=g.tile[0].h;
    const float maxPx=k.dofC.x;
	// Smoke: only with all three parts (coverage, where the smokes are, where the camera is).
	const int smokeCount = (smoke.absorbance && smoke.width && smoke.height && smoke.haveCamera && smoke.spheres)
		? std::min(smoke.count, kLensMaxSmokes) : 0;
	k.smokeInfo = { (float)smokeCount, smokeCount > 0 ? 1.f : 0.f, (float)smoke.width, (float)smoke.height };
	k.cameraPos = { smoke.camera[0], smoke.camera[1], smoke.camera[2], 0 };
	for (int i = 0; i < smokeCount; ++i)
		k.smokeSpheres[i] = { smoke.spheres[i * 4], smoke.spheres[i * 4 + 1], smoke.spheres[i * 4 + 2], smoke.spheres[i * 4 + 3] };
	ID3D11ShaderResourceView* smokeSrv = smokeCount > 0 ? smoke.absorbance : nullptr;
	// The sun, from the sky (LensFxMath.h): on screen, above the horizon, and depth to test it against.
	float sunU = 0, sunV = 0;
	const bool sunOn = flareOn && haveDepth && sun.have && L[FlareSun] > 0.0f && sun.dir[2] > -0.05f
		&& SunScreenUv(worldToClip, sun.dir, sunU, sunV);
	k.sunInfo = { sunU, sunV, sunOn ? L[FlareSun] : 0.0f, 0 };

	ID3D11DeviceContext* c = g.ctx.Get();
	c->ClearState();
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g.vs.Get(), nullptr, 0);
	ID3D11Buffer* cbs[] = { g.cb.Get() };
	c->PSSetConstantBuffers(0, 1, cbs);
	ID3D11SamplerState* samplers[] = { g.linear.Get() };
	c->PSSetSamplers(0, 1, samplers);
	c->RSSetState(g.raster.Get());
	c->OMSetDepthStencilState(g.noDepth.Get(), 0);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	c->OMSetBlendState(g.opaque.Get(), blendFactor, 0xffffffff);

	struct Bind { UINT slot; ID3D11ShaderResourceView* srv; };
	auto pass = [&](ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h, std::initializer_list<Bind> binds) {
		ID3D11ShaderResourceView* none[23] = {};
		c->PSSetShaderResources(0, 23, none);
		c->OMSetRenderTargets(1, &rtv, nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
		c->RSSetViewports(1, &vp);
		c->PSSetShader(ps, nullptr, 0);
		for (const Bind& b : binds) c->PSSetShaderResources(b.slot, 1, &b.srv);
		c->Draw(3, 0);
	};
	// The same with two targets: the CoC pass and its mips also write the world mask.
	auto pass2 = [&](ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv0, ID3D11RenderTargetView* rtv1, UINT w, UINT h,
		std::initializer_list<Bind> binds) {
		ID3D11ShaderResourceView* none[23] = {};
		c->PSSetShaderResources(0, 23, none);
		ID3D11RenderTargetView* rtvs[2] = { rtv0, rtv1 };
		c->OMSetRenderTargets(2, rtvs, nullptr);
		const D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
		c->RSSetViewports(1, &vp);
		c->PSSetShader(ps, nullptr, 0);
		for (const Bind& b : binds) c->PSSetShaderResources(b.slot, 1, &b.srv);
		c->Draw(3, 0);
	};
	auto setPassSource = [&](UINT w, UINT h) {
		k.passInfo = { 1.0f / (float)w, 1.0f / (float)h, 0, 0 };
		c->UpdateSubresource(g.cb.Get(), 0, nullptr, &k, 0, 0);
	};
	c->UpdateSubresource(g.cb.Get(), 0, nullptr, &k, 0, 0);

	// 1. Snapshot the frame. An MSAA target is resolved for the passes and also copied whole, so the
	//    final composite can keep the engine's own per-sample edges wherever nothing was blurred.
	const bool msaa = td.SampleDesc.Count > 1;
	if (msaa) {
		c->ResolveSubresource(g.srcRes.tex.Get(), 0, targetTex.Get(), 0, fmt);
		c->CopyResource(g.msaaCopy.tex.Get(), targetTex.Get());
	} else {
		c->CopyResource(g.srcRes.tex.Get(), targetTex.Get());
	}
	const int dms = (haveDepth && dd.SampleDesc.Count > 1) ? 1 : 0;
	if (dofOn || sunOn) c->CopyResource(g.depthCopy.tex.Get(), depthTex.Get());

	// 2. Depth of field.
	if (dofOn) {
		const int cur = advanceFocus ? 1 - g.focusCur : g.focusCur;
		if (advanceFocus)
			pass(g.focusPS[dms].Get(), g.focus[cur].rtv.Get(), 1, 1,
				{ { 1, g.depthCopy.srv.Get() }, { 3, g.focus[g.focusCur].srv.Get() }, { 20, smokeSrv } });
		pass2(g.cocPS[dms].Get(), g.cocRtv[0].Get(), g.worldRtv[0].Get(), td.Width, td.Height,
			{ { 0, g.srcRes.srv.Get() }, { 1, g.depthCopy.srv.Get() }, { 2, g.focus[cur].srv.Get() }, { 20, smokeSrv } });
		for (int m = 1; m < g.cocLevels; ++m)
			pass2(g.mipPS.Get(), g.cocRtv[m].Get(), g.worldRtv[m].Get(), std::max(td.Width >> m, 1u), std::max(td.Height >> m, 1u),
				{ { 4, g.cocMip[m - 1].Get() }, { 21, g.worldMip[m - 1].Get() } });
		pass(g.tilePS.Get(), g.tile[0].rtv.Get(), gw, gh, { { 4, g.cocAll.Get() } });
		pass(g.dilateH.Get(), g.tile[1].rtv.Get(), gw, gh, { { 5, g.tile[0].srv.Get() } });
		pass(g.dilateV.Get(), g.tile[0].rtv.Get(), gw, gh, { { 5, g.tile[1].srv.Get() } });
		pass(g.gatherPS.Get(), g.gather.rtv.Get(), td.Width, td.Height,
			{ { 4, g.cocAll.Get() }, { 5, g.tile[0].srv.Get() }, { 7, g.irisTex.srv.Get() }, { 21, g.worldAll.Get() } });
		pass(g.denoisePS.Get(), g.soft.rtv.Get(), td.Width, td.Height, { { 4, g.cocAll.Get() }, { 6, g.gather.srv.Get() } });
		if (advanceFocus) g.focusCur = cur;
		g.focusValid = autoFocus;
	}

	// 3. Lens flare, from the (possibly already blurred) frame.
	if (flareOn) {
		// The flare reads the SHARP frame, blurred or not: a lifted bokeh disc is a huge white shape, and a flare cast
		// from it would be a wash. Light sources flare where they are, in focus or not.
		ID3D11ShaderResourceView* flareSrc = g.srcRes.srv.Get();
		if (sunOn)
			pass(g.sunPS[dms].Get(), g.sunVis.rtv.Get(), 1, 1, { { 1, g.depthCopy.srv.Get() }, { 20, smokeSrv } });
		pass(g.brightPS.Get(), g.halfBright.rtv.Get(), hw, hh, { { 8, flareSrc } });
		setPassSource(hw, hh);
		pass(g.down13PS.Get(), g.quarterBright.rtv.Get(), qw, qh, { { 9, g.halfBright.srv.Get() } });
		setPassSource(qw, qh);
		pass(g.down13PS.Get(), g.eighthBright.rtv.Get(), ew, eh, { { 9, g.quarterBright.srv.Get() } });
		// The source map: the bright pass kept only where the source is small (a bright ring around it drops
		// it) and faded at the frame edge. Everything after this reads the source map, never the bright pass.
		pass(g.sourcePS.Get(), g.sourceQ.rtv.Get(), qw, qh,
			{ { 8, flareSrc }, { 9, g.quarterBright.srv.Get() }, { 14, g.eighthBright.srv.Get() }, { 22, g.sunVis.srv.Get() } });
		pass(g.down13PS.Get(), g.sourceE.rtv.Get(), ew, eh, { { 9, g.sourceQ.srv.Get() } });
		// Each aperture tap reads its full footprint, preventing sparse ghost dots.
		c->OMSetRenderTargets(0, nullptr, nullptr);
		c->GenerateMips(g.sourceE.srv.Get());
		pass(g.sourceHPS.Get(), g.sourceH.rtv.Get(), ew, hh, { { 17, g.sourceQ.srv.Get() } });
		pass(g.apertureBlurPS.Get(), g.ghostSrc.rtv.Get(), qw, qh, { { 18, g.sourceE.srv.Get() } });
		pass(g.ghostPS.Get(), g.ghostOut.rtv.Get(), qw, qh, { { 16, g.ghostSrc.srv.Get() } });
		pass(g.streakPS.Get(), g.streakOut.rtv.Get(), hw, hh, { { 17, g.sourceQ.srv.Get() }, { 19, g.sourceH.srv.Get() } });
		pass(g.glowH.Get(), g.glowB.rtv.Get(), ew, eh, { { 18, g.sourceE.srv.Get() } });
		pass(g.glowV.Get(), g.glowA.rtv.Get(), ew, eh, { { 12, g.glowB.srv.Get() } });
	}

	// 4. Composite back onto the engine's target.
	pass(g.compositePS[msaa ? 1 : 0].Get(), target, td.Width, td.Height,
		{ { 0, g.srcRes.srv.Get() }, { 4, g.cocAll.Get() }, { 6, g.soft.srv.Get() },
		  { 10, g.ghostOut.srv.Get() }, { 11, g.streakOut.srv.Get() }, { 12, g.glowA.srv.Get() },
		  { 13, msaa ? g.msaaCopy.srv.Get() : nullptr }, { 15, g.dirtTex.srv.Get() },
		  { 14, flareOn ? g.eighthBright.srv.Get() : nullptr }, { 22, sunOn ? g.sunVis.srv.Get() : nullptr } });
	c->ClearState();

	ComPtr<ID3D11CommandList> list;
	if (FAILED(c->FinishCommandList(FALSE, &list))) {
		LensFx_SetRenderStatus("Could not submit lens frame");
		return false;
	}
	{ GPU_PASS_SCOPE(immediate, "lens: depth of field, flare"); immediate->ExecuteCommandList(list.Get(), TRUE); }
	LensFx_NoteRenderedFrame();

	// Diagnostics: while mvm_debug runs, read back where autofocus is sitting and say what was drawn.
	if (MvmDebugLog_Active()) {
		if (autoFocus && g.focusStaging) {
			D3D11_MAPPED_SUBRESOURCE map{};
			if (g.probeCounter > 0 && SUCCEEDED(immediate->Map(g.focusStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) {
				MvmDebugLog_Linef("lens.focus", "autofocus distance=%.2f units", *(const float*)map.pData);
				immediate->Unmap(g.focusStaging.Get(), 0);
			}
			immediate->CopyResource(g.focusStaging.Get(), g.focus[g.focusCur].tex.Get());
		}
		if ((g.probeCounter++ % 120) == 0)
			MvmDebugLog_Linef("lens.frame",
				"dof=%d flare=%d target=%ux%u samples=%u fmt=%d depth=%ux%u depthSamples=%u depthFmt=%d mode=%d shape=%d "
				"depthCopy=%ux%u focus=%.1f fstop=%.2f band=%.0f maxBlurPx=%.1f taps=%d auto=%d iris=%d ghosts=%d streak=%.2f star=%.2f threshold=%.2f "
				"smoke: absorbance=%ux%u spheres=%d camera=%d used=%d sun: have=%d dir=(%.3f %.3f %.3f) on=%d uv=(%.3f %.3f)",
				(int)dofOn, (int)flareOn, td.Width, td.Height, td.SampleDesc.Count, (int)fmt, dd.Width, dd.Height,
				dd.SampleDesc.Count, (int)dd.Format, (int)L[DofMode], (int)L[DofShape], g.dw, g.dh, L[DofFocus], L[DofFstop], L[DofRange], maxPx,
				TapCount(L), (int)autoFocus, (int)g.irisLoaded, (int)L[FlareGhosts], L[FlareStreak], L[FlareStar], L[FlareThreshold],
				smoke.absorbance ? smoke.width : 0u, smoke.absorbance ? smoke.height : 0u, smoke.count, (int)smoke.haveCamera, smokeCount,
				(int)sun.have, sun.dir[0], sun.dir[1], sun.dir[2], (int)sunOn, sunU, sunV);
	}
	LensFx_SetRenderStatus("Rendering");
	return true;
}

} // namespace Filmmaker
