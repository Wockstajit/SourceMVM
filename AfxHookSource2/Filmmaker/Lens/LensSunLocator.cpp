#include "LensSunLocator.h"

#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace Filmmaker {

namespace {

// The target's size. Powers of two, so every mip halves cleanly down to 1 x 1 and the last mip is
// the plain mean of all texels. 256 wide puts the 0.5-degree disc at about 3 texels at a 90-degree
// field of view, enough to be covered by at least one texel centre.
constexpr UINT kWidth = 256, kHeight = 128, kMips = 9;

// The pixel shader: g_flColorBoost sits at float offset 108 of csgo_effects' pixel constants
// (SkyGpuRotation.cpp, ReadEffectColorBoost), the compact disc uses a large value and its halo 1.0.
// A smaller constant buffer reads 0 there and is dropped with everything else that is not the disc.
constexpr char kSource[] = R"HLSL(
cbuffer EngineEffect : register(b0) { float4 effectConstants[7]; };
float4 LocatePS(float4 position : SV_Position) : SV_Target {
    if (!(effectConstants[6].w > 4)) discard;
    return float4(position.xy / float2(256, 128), 1, 1);
}
)HLSL";

std::atomic<bool> s_wanted{ false };
std::atomic<unsigned> s_located{ 0 };

struct State {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11BlendState> blend;
	ComPtr<ID3D11DepthStencilState> noDepth;
	bool failed = false;
	// Whether a pixel shader is csgo_effects', by pointer: the name lookup is a private-data read.
	std::unordered_map<ID3D11PixelShader*, bool> effectShaders;
} s;

bool NameContains(ID3D11DeviceChild* shader, const char* needle) {
	char name[256] = {};
	UINT size = sizeof(name) - 1;
	if (!shader || FAILED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name))) return false;
	name[size < sizeof(name) ? size : sizeof(name) - 1] = '\0';
	for (char* p = name; *p; ++p) {
		const char* n = needle;
		const char* q = p;
		while (*q && *n && std::tolower((unsigned char)*q) == std::tolower((unsigned char)*n)) { ++q; ++n; }
		if (!*n) return true;
	}
	return false;
}

bool IsEffectShader(ID3D11PixelShader* ps) {
	if (!ps) return false;
	auto it = s.effectShaders.find(ps);
	if (it != s.effectShaders.end()) return it->second;
	if (s.effectShaders.size() > 4096) s.effectShaders.clear();
	const bool effect = NameContains(ps, "effects");
	s.effectShaders.emplace(ps, effect);
	return effect;
}

bool Ready(ID3D11DeviceContext* ctx) {
	if (s.rtv) return true;
	if (s.failed) return false;
	s.failed = true; // one attempt per device; cleared by EndDevice
	ComPtr<ID3D11Device> device;
	ctx->GetDevice(&device);
	if (!device) return false;
	// A float target that blends and builds its own mips; 32-bit keeps the tiny disc mean exact.
	DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	const UINT needed = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
	UINT support = 0;
	if (FAILED(device->CheckFormatSupport(format, &support)) || (support & needed) != needed) format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = kWidth; d.Height = kHeight; d.MipLevels = kMips; d.ArraySize = 1; d.Format = format;
	d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11ShaderResourceView> srv;
	if (FAILED(device->CreateTexture2D(&d, nullptr, &texture))) return false;
	D3D11_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Texture2D.MipSlice = 0;
	if (FAILED(device->CreateRenderTargetView(texture.Get(), &rd, &rtv))) return false;
	if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) return false;
	ComPtr<ID3DBlob> code, errors;
	if (FAILED(Environment::CompileEnvironmentShader(kSource, sizeof(kSource) - 1, "LensSunLocator", nullptr, "LocatePS",
		"ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &errors))) {
		MvmDebugLog_LinefAlways("lens.sun", "locator shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "");
		return false;
	}
	ComPtr<ID3D11PixelShader> ps;
	if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps))) return false;
	D3D11_RASTERIZER_DESC r{};
	r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = FALSE;
	ComPtr<ID3D11RasterizerState> raster;
	if (FAILED(device->CreateRasterizerState(&r, &raster))) return false;
	D3D11_BLEND_DESC b{};
	b.RenderTarget[0].BlendEnable = TRUE;
	b.RenderTarget[0].SrcBlend = b.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	b.RenderTarget[0].SrcBlendAlpha = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ComPtr<ID3D11BlendState> blend;
	if (FAILED(device->CreateBlendState(&b, &blend))) return false;
	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ComPtr<ID3D11DepthStencilState> noDepth;
	if (FAILED(device->CreateDepthStencilState(&ds, &noDepth))) return false;
	const float zero[4] = {};
	ctx->ClearRenderTargetView(rtv.Get(), zero);
	s.device = device; s.texture = texture; s.rtv = rtv; s.srv = srv; s.ps = ps;
	s.raster = raster; s.blend = blend; s.noDepth = noDepth; s.failed = false;
	MvmDebugLog_LinefAlways("lens.sun", "locator ready %ux%u format=%d", kWidth, kHeight, (int)format);
	return true;
}

// What can be the sun disc: a csgo_effects draw of a handful of vertices (a quad or a triangle, one
// or a few instances). The pixel shader keeps only the high-boost disc among them, so a near miss
// costs one tiny draw. With mvm_debug on, every small effects draw is logged once per shape so a
// map that draws its sun differently can be read from the log.
bool Candidate(ID3D11DeviceContext* ctx, const LensSunDraw& draw) {
	if (draw.count < 3 || draw.count > 12 || draw.instances < 1 || draw.instances > 4) return false;
	ComPtr<ID3D11PixelShader> ps;
	ctx->PSGetShader(&ps, nullptr, nullptr);
	if (!IsEffectShader(ps.Get())) return false;
	if (MvmDebugLog_Active()) {
		ComPtr<ID3D11Buffer> ib, vb;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT ibOffset = 0, stride = 0, offset = 0;
		ctx->IAGetIndexBuffer(&ib, &format, &ibOffset);
		ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
		D3D11_BUFFER_DESC ibDesc{}, vbDesc{};
		if (ib) ib->GetDesc(&ibDesc);
		if (vb) vb->GetDesc(&vbDesc);
		D3D11_VIEWPORT vp{};
		UINT vpCount = 1;
		ctx->RSGetViewports(&vpCount, &vp);
		static std::unordered_map<unsigned long long, ULONGLONG> s_seen;
		const unsigned long long shape = (unsigned long long)draw.kind | (unsigned long long)draw.count << 4
			| (unsigned long long)draw.instances << 12 | (unsigned long long)(stride & 0xff) << 16
			| (unsigned long long)(vbDesc.ByteWidth & 0xffffff) << 24 | (unsigned long long)(ibDesc.ByteWidth & 0xffff) << 48;
		const ULONGLONG now = GetTickCount64();
		auto& last = s_seen[shape];
		if (now - last > 5000) {
			last = now;
			MvmDebugLog_LinefAlways("lens.sun.draw", "kind=%d count=%u instances=%u ib=%u fmt=%d vb=%u stride=%u offset=%u vpDepth=%.4f..%.4f vp=%.0fx%.0f",
				(int)draw.kind, draw.count, draw.instances, ibDesc.ByteWidth, (int)format, vbDesc.ByteWidth, stride, offset,
				vp.MinDepth, vp.MaxDepth, vp.Width, vp.Height);
		}
	}
	return true;
}

} // namespace

void LensSunLocator_SetWanted(bool wanted) { s_wanted.store(wanted, std::memory_order_relaxed); }
bool LensSunLocator_Wanted() { return s_wanted.load(std::memory_order_relaxed); }

void LensSunLocator_AfterDraw(ID3D11DeviceContext* ctx, const LensSunDraw& draw,
	void (*redraw)(ID3D11DeviceContext*, const LensSunDraw&)) {
	if (!ctx || !redraw || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
	if (!Candidate(ctx, draw) || !Ready(ctx)) return;
	// Everything this draw changes, saved and put back exactly.
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* dsv = nullptr;
	ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	ctx->RSGetViewports(&viewportCount, viewports);
	ID3D11RasterizerState* raster = nullptr;
	ctx->RSGetState(&raster);
	ID3D11BlendState* blend = nullptr;
	float blendFactor[4] = {};
	UINT sampleMask = 0;
	ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
	ID3D11DepthStencilState* depth = nullptr;
	UINT stencilRef = 0;
	ctx->OMGetDepthStencilState(&depth, &stencilRef);
	ID3D11PixelShader* ps = nullptr;
	ID3D11ClassInstance* instances[256] = {};
	UINT instanceCount = 256;
	ctx->PSGetShader(&ps, instances, &instanceCount);

	ID3D11RenderTargetView* target = s.rtv.Get();
	ctx->OMSetRenderTargets(1, &target, nullptr);
	const D3D11_VIEWPORT viewport = { 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
	ctx->RSSetViewports(1, &viewport);
	ctx->RSSetState(s.raster.Get());
	ctx->OMSetBlendState(s.blend.Get(), nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(s.noDepth.Get(), 0);
	ctx->PSSetShader(s.ps.Get(), nullptr, 0);
	redraw(ctx, draw);
	s_located.fetch_add(1, std::memory_order_relaxed);

	ctx->PSSetShader(ps, instances, instanceCount);
	ctx->OMSetDepthStencilState(depth, stencilRef);
	ctx->OMSetBlendState(blend, blendFactor, sampleMask);
	ctx->RSSetState(raster);
	ctx->RSSetViewports(viewportCount, viewports);
	ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
	if (ps) ps->Release();
	for (UINT i = 0; i < instanceCount && i < 256; ++i) if (instances[i]) instances[i]->Release();
	if (depth) depth->Release();
	if (blend) blend->Release();
	if (raster) raster->Release();
	for (auto* v : rtvs) if (v) v->Release();
	if (dsv) dsv->Release();
}

ID3D11ShaderResourceView* LensSunLocator_Result() { return s.srv.Get(); }

void LensSunLocator_Resolve(ID3D11DeviceContext* ctx) {
	if (ctx && s.srv) ctx->GenerateMips(s.srv.Get());
}

void LensSunLocator_Clear(ID3D11DeviceContext* ctx) {
	const float zero[4] = {};
	if (ctx && s.rtv) ctx->ClearRenderTargetView(s.rtv.Get(), zero);
}

unsigned LensSunLocator_TakeDrawCount() { return s_located.exchange(0, std::memory_order_relaxed); }

void LensSunLocator_EndDevice() {
	s.device.Reset(); s.texture.Reset(); s.rtv.Reset(); s.srv.Reset(); s.ps.Reset();
	s.raster.Reset(); s.blend.Reset(); s.noDepth.Reset();
	s.effectShaders.clear();
	s.failed = false;
}

} // namespace Filmmaker
