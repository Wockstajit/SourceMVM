#define NOMINMAX
#include "RoundHeatmap.h"
#include "RoundHeatmapMath.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

// THE FLOOR PASS. A full-screen pass over the finished world frame: each pixel's world position comes
// back from the scene depth, and an upward-facing surface at the height a player stood on is
// painted red by how often the round's players covered that spot (RoundHeatmapMath.h). Pixels past
// the radius around the camera, the sky, the 3D skybox, the first-person weapon and the live
// players' shoes are left alone.

namespace Filmmaker::Heatmap {
using Microsoft::WRL::ComPtr;
namespace {

constexpr int kMaxActors = 12; // Environment::Frame::kMaxActors

constexpr char kShader[] = R"HLSL(
cbuffer HeatConstants : register(b0) {
    row_major float4x4 inverseViewProjection;
    float4 dims;         // target width, height; depth width, height
    float4 depthInfo;    // viewport min depth, max depth, reversed depth, live players
    float4 cameraRadius; // camera position, shown radius
    float4 grid;         // x0, y0, cell, texels per side
    float4 shading;      // height base, 1 / log2(1 + full visits)
    float4 actors[12];   // the live players' feet
};
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t0);
#else
Texture2D<float> sceneDepth : register(t0);
#endif
Texture2D<float4> heat : register(t1); // visits A, height A, visits B, height B

float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

bool Reversed() { return depthInfo.z > .5; }
float RawDepth(int2 p) {
#if DEPTH_MS
    uint w, h, n;
    sceneDepth.GetDimensions(w, h, n);
    float d = sceneDepth.Load(p, 0);
    for (uint i = 1; i < n; ++i) d = Reversed() ? max(d, sceneDepth.Load(p, i)) : min(d, sceneDepth.Load(p, i));
    return d;
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}
// The world point under a pixel. False for the sky, CS2's 3D skybox band (outside the viewport's
// depth range) and the first-person weapon's own band (raw depth up to 0.1, forward depth only).
bool WorldAt(float2 pixel, out float3 world) {
    world = float3(0, 0, 0);
    int2 p = clamp(int2(pixel * dims.zw / dims.xy), int2(0, 0), int2(dims.zw) - 1);
    float d = RawDepth(p);
    if (Reversed() ? d < 1e-7 : d > .9999999) return false;
    if (!Reversed() && d <= .100001) return false;
    float n = (d - depthInfo.x) / max(depthInfo.y - depthInfo.x, 1e-12);
    if (n < 0 || n > 1) return false;
    float4 h = mul(inverseViewProjection, float4(pixel / dims.xy * float2(2, -2) + float2(-1, 1), n, 1));
    world = h.xyz / h.w;
    return true;
}
// The surface normal from the neighbours on the same surface (the nearer one on each axis), facing
// the camera, so a ceiling seen from below points down and is never taken for a floor.
float3 SurfaceNormal(float2 pixel, float3 c) {
    float3 r, l, d, u;
    bool hr = WorldAt(pixel + float2(1, 0), r), hl = WorldAt(pixel - float2(1, 0), l);
    bool hd = WorldAt(pixel + float2(0, 1), d), hu = WorldAt(pixel - float2(0, 1), u);
    if (!(hr || hl) || !(hd || hu)) return float3(0, 0, 0);
    float3 xr = r - c, xl = c - l, yd = d - c, yu = c - u;
    float3 dx = hr && (!hl || dot(xr, xr) < dot(xl, xl)) ? xr : xl;
    float3 dy = hd && (!hu || dot(yd, yd) < dot(yu, yu)) ? yd : yu;
    float3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-8) return float3(0, 0, 0);
    n /= len;
    return dot(n, cameraRadius.xyz - c) < 0 ? -n : n;
}
// The stored feet height is the floor; stairs and ramps sit within a step of it.
float Band(float dz) { return 1 - smoothstep(12, 20, abs(dz)); }
float LayerVisits(float4 t, float z) {
    float a = t.x > 0 ? t.x * Band(z - (t.y + shading.x)) : 0;
    float b = t.z > 0 ? t.z * Band(z - (t.w + shading.x)) : 0;
    return max(a, b);
}
// Bilinear over the four nearest texels, each matched to this point's height: cover is how much of
// the spot a body covered (its 0.5 line is the exact strip edge), visits the count where it did.
void HeatAt(float3 w, out float cover, out float visits) {
    float2 g = (w.xy - grid.xy) / grid.z - .5;
    float2 base = floor(g), f = g - base;
    int2 q = int2(base);
    int size = int(grid.w);
    float sum = 0;
    cover = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        int2 o = int2(k & 1, k >> 1);
        int2 t = q + o;
        if (t.x < 0 || t.y < 0 || t.x >= size || t.y >= size) continue;
        float weight = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        float v = LayerVisits(heat.Load(int3(t, 0)), w.z);
        sum += weight * v;
        cover += v > 0 ? weight : 0;
    }
    visits = cover > 0 ? sum / cover : 0;
}

float4 PS(float4 pos : SV_Position) : SV_Target {
    float3 w;
    if (!WorldAt(pos.xy, w)) discard;
    float fade = 1 - smoothstep(cameraRadius.w * .85, cameraRadius.w, length(w.xy - cameraRadius.xy));
    if (fade <= 0) discard;
    float cover, visits;
    HeatAt(w, cover, visits);
    float presence = smoothstep(.3, .7, cover);
    if (presence <= 0) discard;
    float up = smoothstep(.62, .78, SurfaceNormal(pos.xy, w).z);
    if (up <= 0) discard;
    [loop] for (int i = 0; i < int(depthInfo.w); ++i) {
        float3 a = actors[i].xyz;
        if (length(w.xy - a.xy) < 20 && w.z > a.z + 2.5 && w.z < a.z + 80) discard;
    }
    float k = saturate(log2(1 + visits) * shading.y);
    float3 rgb = lerp(float3(.50, .03, .03), float3(1, .10, .04), k);
    return float4(rgb, presence * up * fade * lerp(.42, .82, k));
}
)HLSL";

struct Constants {
	float inverseViewProjection[16];
	float dims[4];
	float depthInfo[4];
	float cameraRadius[4];
	float grid[4];
	float shading[4];
	float actors[kMaxActors][4];
};
static_assert(sizeof(Constants) % 16 == 0, "constant buffer size");

struct Blobs {
	ComPtr<ID3DBlob> vs, ps[2];
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const Blobs> g_blobs;
std::thread g_compiler;
std::atomic<bool> g_compileStarted{ false };

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> commands;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps[2];
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11BlendState> blend;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> noDepth;
	ComPtr<ID3D11Texture2D> heat;
	ComPtr<ID3D11ShaderResourceView> heatSrv;
	int heatSize = 0;
	uint64_t heatSerial = ~0ull;
	bool heatValid = false;
	ComPtr<ID3D11Texture2D> depthCopy;
	ComPtr<ID3D11ShaderResourceView> depthSrv;
	D3D11_TEXTURE2D_DESC depthDesc{};
	bool failed = false;
} g_gpu; // render thread only

void CompileAll() {
	auto blobs = std::make_shared<Blobs>();
	ComPtr<ID3DBlob> errors;
	const D3D_SHADER_MACRO single[] = { { "DEPTH_MS", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO multi[] = { { "DEPTH_MS", "1" }, { nullptr, nullptr } };
	const size_t n = sizeof(kShader) - 1;
	blobs->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "RoundHeatmap", single, "VS", "vs_5_0",
					D3DCOMPILE_ENABLE_STRICTNESS, &blobs->vs, &errors))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "RoundHeatmap", single, "PS", "ps_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, &blobs->ps[0], &errors))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "RoundHeatmap", multi, "PS", "ps_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, &blobs->ps[1], &errors));
	if (!blobs->ok)
		MvmDebugLog_LinefAlways("heatmap", "shader failed: %s",
			errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compiler unavailable");
	std::lock_guard<std::mutex> lock(g_blobLock);
	g_blobs = blobs;
}

void ReleaseGpu() { g_gpu = Gpu{}; }

bool GpuReady(ID3D11Device* device) {
	if (g_gpu.device.Get() != device) { g_gpu = Gpu{}; g_gpu.device = device; }
	if (g_gpu.vs) return true;
	if (g_gpu.failed) return false;
	std::shared_ptr<const Blobs> blobs;
	{
		std::lock_guard<std::mutex> lock(g_blobLock);
		blobs = g_blobs;
	}
	if (!blobs) return false; // still compiling
	g_gpu.failed = true;
	if (!blobs->ok) return false;
	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = sizeof(Constants);
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	D3D11_BLEND_DESC bd{};
	auto& rt = bd.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
	D3D11_DEPTH_STENCIL_DESC dd{};
	dd.DepthEnable = FALSE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	if (FAILED(device->CreateVertexShader(blobs->vs->GetBufferPointer(), blobs->vs->GetBufferSize(), nullptr, &g_gpu.vs))
		|| FAILED(device->CreatePixelShader(blobs->ps[0]->GetBufferPointer(), blobs->ps[0]->GetBufferSize(), nullptr, &g_gpu.ps[0]))
		|| FAILED(device->CreatePixelShader(blobs->ps[1]->GetBufferPointer(), blobs->ps[1]->GetBufferSize(), nullptr, &g_gpu.ps[1]))
		|| FAILED(device->CreateBuffer(&cb, nullptr, &g_gpu.constants))
		|| FAILED(device->CreateBlendState(&bd, &g_gpu.blend))
		|| FAILED(device->CreateRasterizerState(&rd, &g_gpu.raster))
		|| FAILED(device->CreateDepthStencilState(&dd, &g_gpu.noDepth))
		|| FAILED(device->CreateDeferredContext(0, &g_gpu.commands))) {
		g_gpu.vs.Reset();
		MvmDebugLog_LinefAlways("heatmap", "GPU objects could not be created");
		return false;
	}
	g_gpu.failed = false;
	return true;
}

// A shader-readable copy of the scene depth, matching its size, format and sample count.
bool EnsureDepthCopy(const D3D11_TEXTURE2D_DESC& src) {
	const auto& have = g_gpu.depthDesc;
	if (g_gpu.depthCopy && have.Width == src.Width && have.Height == src.Height && have.Format == src.Format
		&& have.SampleDesc.Count == src.SampleDesc.Count && have.SampleDesc.Quality == src.SampleDesc.Quality)
		return true;
	g_gpu.depthCopy.Reset();
	g_gpu.depthSrv.Reset();
	D3D11_TEXTURE2D_DESC d = src;
	d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
	d.MipLevels = 1; d.ArraySize = 1;
	DXGI_FORMAT view;
	switch (src.Format) {
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
	if (FAILED(g_gpu.device->CreateTexture2D(&d, nullptr, &g_gpu.depthCopy))
		|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.depthCopy.Get(), &sv, &g_gpu.depthSrv))) {
		g_gpu.depthCopy.Reset();
		g_gpu.depthSrv.Reset();
		return false;
	}
	g_gpu.depthDesc = src;
	return true;
}

// The grid's placement, kept with the texture it was uploaded into.
struct Placement { float x0 = 0, y0 = 0, cell = 0, baseZ = 0, radius = 0; int size = 0; };
Placement g_placement;

// The published grid in a texture, uploaded once per new grid.
bool UploadGrid(ID3D11DeviceContext* immediate, const std::shared_ptr<const GpuGrid>& grid, uint64_t serial) {
	if (serial == g_gpu.heatSerial) return g_gpu.heatValid;
	g_gpu.heatSerial = serial;
	g_gpu.heatValid = false;
	if (!grid || grid->size <= 0 || grid->texels.size() != size_t(grid->size) * size_t(grid->size) * 4) return false;
	if (!g_gpu.heat || g_gpu.heatSize != grid->size) {
		g_gpu.heat.Reset();
		g_gpu.heatSrv.Reset();
		D3D11_TEXTURE2D_DESC d{};
		d.Width = d.Height = UINT(grid->size);
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(g_gpu.device->CreateTexture2D(&d, nullptr, &g_gpu.heat))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.heat.Get(), nullptr, &g_gpu.heatSrv))) {
			g_gpu.heat.Reset();
			g_gpu.heatSrv.Reset();
			g_gpu.heatSize = 0;
			return false;
		}
		g_gpu.heatSize = grid->size;
	}
	immediate->UpdateSubresource(g_gpu.heat.Get(), 0, nullptr, grid->texels.data(), UINT(grid->size) * 8u, 0);
	g_placement = { grid->x0, grid->y0, grid->cell, grid->baseZ, grid->radius, grid->size };
	g_gpu.heatValid = true;
	return true;
}

} // namespace

void PrepareShaders() {
	if (g_compileStarted.exchange(true)) return;
	g_compiler = std::thread(CompileAll);
}

void ShutdownRender() {
	if (g_compiler.joinable()) g_compiler.join();
	ReleaseGpu();
}

void Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth,
	const float* worldToClip, const float* projection, const float camera[3], float depthMin, float depthMax,
	const float* actors, int actorCount, bool recording) {
	if (camera && worldToClip) NoteCamera(camera[0], camera[1], camera[2]);
	if (!Enabled()) {
		if (g_gpu.device) ReleaseGpu();
		return;
	}
	if (recording || !immediate || !target || !depth || !worldToClip || !projection || !camera || !(depthMax > depthMin))
		return;
	ComPtr<ID3D11Device> device;
	immediate->GetDevice(&device);
	if (!GpuReady(device.Get())) return;
	uint64_t serial = 0;
	const auto grid = PublishedGrid(serial);
	if (!UploadGrid(immediate, grid, serial)) return;

	ComPtr<ID3D11Resource> depthRes, targetRes;
	ComPtr<ID3D11Texture2D> depthTex, targetTex;
	depth->GetResource(&depthRes);
	target->GetResource(&targetRes);
	if (!depthRes || FAILED(depthRes.As(&depthTex)) || !targetRes || FAILED(targetRes.As(&targetTex))) return;
	D3D11_TEXTURE2D_DESC dd{}, td{};
	depthTex->GetDesc(&dd);
	targetTex->GetDesc(&td);
	if (dd.ArraySize != 1 || !EnsureDepthCopy(dd)) return;

	Environment::Matrix vp{}, inverse{}, proj{};
	std::memcpy(vp.data(), worldToClip, sizeof(float) * 16);
	std::memcpy(proj.data(), projection, sizeof(float) * 16);
	if (!Environment::Inverse(vp, inverse)) return;
	const bool reversed = Environment::Transform(proj, { 0, 0, -10000000 }).z < .5f;

	auto ctx = g_gpu.commands.Get();
	ctx->ClearState();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(g_gpu.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
	Constants c{};
	std::memcpy(c.inverseViewProjection, inverse.data(), sizeof(c.inverseViewProjection));
	c.dims[0] = float(td.Width); c.dims[1] = float(td.Height); c.dims[2] = float(dd.Width); c.dims[3] = float(dd.Height);
	const int count = actors ? (std::min)(actorCount, kMaxActors) : 0;
	c.depthInfo[0] = depthMin; c.depthInfo[1] = depthMax; c.depthInfo[2] = reversed ? 1.f : 0.f; c.depthInfo[3] = float(count);
	c.cameraRadius[0] = camera[0]; c.cameraRadius[1] = camera[1]; c.cameraRadius[2] = camera[2];
	c.cameraRadius[3] = g_placement.radius;
	c.grid[0] = g_placement.x0; c.grid[1] = g_placement.y0; c.grid[2] = g_placement.cell; c.grid[3] = float(g_placement.size);
	c.shading[0] = g_placement.baseZ;
	c.shading[1] = 1.f / std::log2(1.f + kFullVisits);
	for (int i = 0; i < count; ++i) {
		c.actors[i][0] = actors[i * 3]; c.actors[i][1] = actors[i * 3 + 1]; c.actors[i][2] = actors[i * 3 + 2];
	}
	std::memcpy(mapped.pData, &c, sizeof(c));
	ctx->Unmap(g_gpu.constants.Get(), 0);

	ctx->CopyResource(g_gpu.depthCopy.Get(), depthTex.Get());
	const D3D11_VIEWPORT viewport{ 0, 0, float(td.Width), float(td.Height), 0, 1 };
	ctx->RSSetViewports(1, &viewport);
	ctx->RSSetState(g_gpu.raster.Get());
	ctx->OMSetRenderTargets(1, &target, nullptr);
	ctx->OMSetBlendState(g_gpu.blend.Get(), nullptr, ~0u);
	ctx->OMSetDepthStencilState(g_gpu.noDepth.Get(), 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
	ctx->PSSetShader(g_gpu.ps[dd.SampleDesc.Count > 1 ? 1 : 0].Get(), nullptr, 0);
	ID3D11Buffer* cb = g_gpu.constants.Get();
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* srvs[2] = { g_gpu.depthSrv.Get(), g_gpu.heatSrv.Get() };
	ctx->PSSetShaderResources(0, 2, srvs);
	ctx->Draw(3, 0);
	ctx->ClearState();
	ComPtr<ID3D11CommandList> list;
	if (FAILED(ctx->FinishCommandList(FALSE, &list))) return;
	{ GPU_PASS_SCOPE(immediate, "heatmap: floor"); immediate->ExecuteCommandList(list.Get(), TRUE); }

	if (MvmDebugLog_Active()) {
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last >= 3000) {
			last = now;
			MvmDebugLog_LinefAlways("heatmap", "drew target=%ux%u depth=%ux%u fmt=%u samples=%u reversed=%d range=%.3f-%.3f grid=%d cell=%.2f origin=(%.0f %.0f) baseZ=%.0f camera=(%.0f %.0f %.0f) radius=%.0f players=%d",
				td.Width, td.Height, dd.Width, dd.Height, unsigned(dd.Format), dd.SampleDesc.Count, int(reversed), depthMin, depthMax,
				g_placement.size, g_placement.cell, g_placement.x0, g_placement.y0, g_placement.baseZ, camera[0], camera[1], camera[2],
				g_placement.radius, count);
		}
	}
}

} // namespace Filmmaker::Heatmap
