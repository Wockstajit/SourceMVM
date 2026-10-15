#include "DeadlockOverlayDrawer.h"

#include "DeadlockOverlayShaders.h"
#include "DeadlockOverlayFont.h"
#include "DeadlockOverlayMath.h"
#include "DeadlockCameraRuntime.h"

#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace Deadlock {

namespace {

constexpr int kMaxHeroes = 64;
constexpr int kMaxPathPoints = 1024;
constexpr int kMaxMarkers = 128;
// Every stroke is two triangles. This covers CS2's worst case here many times over: 128 markers x
// 5 glow passes x 10 gizmo edges, a 1024-point trajectory, and a few target boxes x 3 passes.
constexpr int kMaxVertices = 65536;
// Key-number digits: up to three per marker, two triangles each.
constexpr int kMaxTexVertices = 4096;
// CameraBridge_SetMarkerStyle turns CS2's keyframe index on at 18 world units.
constexpr double kKeyframeIndexSize = 18.0;

struct OverlayVertex {
	float posX = 0.0f, posY = 0.0f;
	float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

struct OverlayTexVertex {
	float posX = 0.0f, posY = 0.0f;
	float u = 0.0f, v = 0.0f;
};

struct VsConstants {
	float backbufferW = 1.0f, backbufferH = 1.0f;
	float pad0 = 0.0f, pad1 = 0.0f;
};

struct Rgba { float r, g, b, a; };
Rgba Rgb255(int r, int g, int b, int a) { return { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }; }

// CampathDrawer.cpp, marker style: Live gold / Freeze blue for markers and trajectory, the aimed-at
// marker white, the follow target cyan.
const Rgba kMarkerLive = Rgb255(255, 205, 40, 255);
const Rgba kMarkerFreeze = Rgb255(60, 150, 255, 255);
const Rgba kMarkerAimed = Rgb255(255, 255, 255, 255);
const Rgba kTrajLive = Rgb255(255, 200, 40, 150);
const Rgba kTrajFreeze = Rgb255(80, 160, 255, 150);
const Rgba kTargetBox = Rgb255(80, 230, 255, 255);

struct GlowPass { double widthMul; int alpha; };
// Wide + dim down to the bright core, additive, so light accumulates at the centre of each stroke.
const GlowPass kMarkerGlow[] = { {4.5, 20}, {3.0, 30}, {2.0, 48}, {1.4, 90}, {1.0, 255} };
const GlowPass kBoxGlow[] = { {3.4, 30}, {2.0, 60}, {1.0, 220} };

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_deferred = nullptr;

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Buffer* g_vertexData = nullptr;
ID3D11ShaderResourceView* g_vertexDataSrv = nullptr;
ID3D11RasterizerState* g_rasterizer = nullptr;
ID3D11DepthStencilState* g_depthStencil = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11BlendState* g_blendAdditive = nullptr;

// The digit pipeline: its own shader pair and vertex buffer, HLAE's hexfont as a texture, CS2's
// sampler. Kept separate from Ready() so a failure here costs the numbers, never the strokes.
ID3D11VertexShader* g_texVs = nullptr;
ID3D11PixelShader* g_texPs = nullptr;
ID3D11Buffer* g_texVertexData = nullptr;
ID3D11ShaderResourceView* g_texVertexDataSrv = nullptr;
ID3D11Texture2D* g_font = nullptr;
ID3D11ShaderResourceView* g_fontSrv = nullptr;
ID3D11SamplerState* g_sampler = nullptr;

bool g_enabled = true; // see the header: CS2's gizmo is automatic, so is this one

std::mutex g_snapshotMutex;
std::vector<OverlayHeroBox> g_snapshotHeroes;
std::vector<OverlayPathPoint> g_snapshotPath;
std::vector<OverlayMarker> g_snapshotMarkers;
bool g_snapshotFreeze = false;

unsigned long long g_draws = 0;
unsigned long long g_skips = 0;
unsigned long long g_truncated = 0;
int g_lastVertexCount = 0;
int g_lastDigitVertexCount = 0;

bool TexReady() {
	return g_texVs && g_texPs && g_texVertexData && g_texVertexDataSrv && g_fontSrv && g_sampler;
}

void PushQuad(std::vector<OverlayVertex>& out, const OverlayPixel q[4], const Rgba& c) {
	const auto v = [&](const OverlayPixel& p) {
		out.push_back({ (float)p.x, (float)p.y, c.r, c.g, c.b, c.a });
	};
	v(q[0]); v(q[1]); v(q[2]);
	v(q[2]); v(q[1]); v(q[3]);
}

// One world-space line as a screen-space stroke: clipped against the near plane in WORLD space
// first (a point behind the camera projects to the wrong side of the screen), then extruded.
void PushWorldLine(std::vector<OverlayVertex>& out, const OverlayVec3& a, const OverlayVec3& b,
	const OverlayCameraPose& pose, double width, const Rgba& c) {
	const OverlayClippedSegment seg = ClipSegmentNearPlane(a, b, pose);
	if (!seg.visible) return;
	const OverlayProjection pa = ProjectPoint(seg.a, pose), pb = ProjectPoint(seg.b, pose);
	if (!pa.visible || !pb.visible) return;
	OverlayPixel q[4];
	if (ThickLineQuad({ pa.pixelX, pa.pixelY }, { pb.pixelX, pb.pixelY }, width, q)) PushQuad(out, q, c);
}

// A connected run of projected points as ONE stroke with CS2's mitred joins, not overlapping
// per-segment quads -- an overlap doubles the trajectory's 150-alpha colour at every joint.
void PushStrokeRun(std::vector<OverlayVertex>& out, const std::vector<OverlayPixel>& run, double width, const Rgba& c) {
	if (run.size() < 2) return;
	std::vector<OverlayPixel> off(run.size());
	for (size_t i = 0; i < run.size(); ++i)
		off[i] = PolylineJoinOffset(i > 0 ? &run[i - 1] : nullptr, run[i], i + 1 < run.size() ? &run[i + 1] : nullptr, width);
	for (size_t i = 1; i < run.size(); ++i) {
		const OverlayPixel q[4] = {
			{ run[i - 1].x + off[i - 1].x, run[i - 1].y + off[i - 1].y },
			{ run[i - 1].x - off[i - 1].x, run[i - 1].y - off[i - 1].y },
			{ run[i].x + off[i].x, run[i].y + off[i].y },
			{ run[i].x - off[i].x, run[i].y - off[i].y } };
		PushQuad(out, q, c);
	}
}

// The trajectory. A segment that crosses the near plane ends the current run at its crossing, so a
// clipped end never gets mitred against a point that is really behind the camera.
void PushTrajectory(std::vector<OverlayVertex>& out, const std::vector<OverlayPathPoint>& path,
	const OverlayCameraPose& pose, double width, const Rgba& c) {
	std::vector<OverlayPixel> run;
	bool runEndsUnclipped = false;
	const auto flush = [&]() { PushStrokeRun(out, run, width, c); run.clear(); runEndsUnclipped = false; };
	for (size_t i = 1; i < path.size(); ++i) {
		const OverlayVec3 a{ path[i - 1].x, path[i - 1].y, path[i - 1].z };
		const OverlayVec3 b{ path[i].x, path[i].y, path[i].z };
		const OverlayClippedSegment seg = ClipSegmentNearPlane(a, b, pose);
		if (!seg.visible) { flush(); continue; }
		const OverlayProjection pa = ProjectPoint(seg.a, pose), pb = ProjectPoint(seg.b, pose);
		if (!pa.visible || !pb.visible) { flush(); continue; }
		const bool aUnclipped = seg.a.x == a.x && seg.a.y == a.y && seg.a.z == a.z;
		const bool bUnclipped = seg.b.x == b.x && seg.b.y == b.y && seg.b.z == b.z;
		if (run.empty() || !runEndsUnclipped || !aUnclipped) {
			flush();
			run.push_back({ pa.pixelX, pa.pixelY });
		}
		run.push_back({ pb.pixelX, pb.pixelY });
		runEndsUnclipped = bUnclipped;
	}
	flush();
}

// A marker's index as billboard digits. A digit with any corner at or behind the near plane is
// skipped whole -- CS2 clips on the GPU, but a half-drawn glyph reads worse than none.
void PushKeyframeDigits(std::vector<OverlayTexVertex>& out, int index, const OverlayVec3& at,
	const OverlayCameraPose& pose) {
	OverlayDigitQuad quads[3];
	const int n = KeyframeDigitQuads(index, at, pose.basis.right, pose.basis.up, kKeyframeIndexSize, quads, 3);
	for (int d = 0; d < n; ++d) {
		OverlayProjection p[4];
		bool visible = true;
		for (int c = 0; c < 4 && visible; ++c) {
			p[c] = ProjectPoint(quads[d].corner[c], pose);
			visible = p[c].visible;
		}
		if (!visible) continue;
		const OverlayDigitQuad& q = quads[d];
		const OverlayTexVertex lt{ (float)p[0].pixelX, (float)p[0].pixelY, (float)q.u0, (float)q.v0 };
		const OverlayTexVertex lb{ (float)p[1].pixelX, (float)p[1].pixelY, (float)q.u0, (float)q.v1 };
		const OverlayTexVertex rt{ (float)p[2].pixelX, (float)p[2].pixelY, (float)q.u1, (float)q.v0 };
		const OverlayTexVertex rb{ (float)p[3].pixelX, (float)p[3].pixelY, (float)q.u1, (float)q.v1 };
		out.push_back(lt); out.push_back(lb); out.push_back(rt);
		out.push_back(rt); out.push_back(lb); out.push_back(rb);
	}
}

// Uploads one batch and draws it with `blend`. Each batch is its own WRITE_DISCARD upload drawn
// from vertex 0, so no batch depends on how SV_VertexID treats a start offset.
void DrawBatch(const std::vector<OverlayVertex>& verts, ID3D11BlendState* blend) {
	if (verts.empty()) return;
	UINT count = (UINT)(std::min)((size_t)kMaxVertices, verts.size());
	if (count < verts.size()) ++g_truncated;
	count -= count % 3;
	if (count == 0) return;

	ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
	g_deferred->VSSetShaderResources(0, 1, nullSrv);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(g_deferred->Map(g_vertexData, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
	std::memcpy(mapped.pData, verts.data(), count * sizeof(OverlayVertex));
	g_deferred->Unmap(g_vertexData, 0);
	g_deferred->VSSetShaderResources(0, 1, &g_vertexDataSrv);

	const float blendFactor[4] = { 0, 0, 0, 0 };
	g_deferred->OMSetBlendState(blend, blendFactor, 0xffffffff);
	g_deferred->Draw(count, 0);
}

// The digits: CS2's drawtexture pass -- straight alpha blend, texels under 0.004 alpha discarded in
// the pixel shader. Leaves the stroke shaders bound again for the batches after it.
void DrawDigitBatch(const std::vector<OverlayTexVertex>& verts) {
	if (verts.empty() || !TexReady()) return;
	UINT count = (UINT)(std::min)((size_t)kMaxTexVertices, verts.size());
	if (count < verts.size()) ++g_truncated;
	count -= count % 3;
	if (count == 0) return;

	ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
	g_deferred->VSSetShaderResources(0, 1, nullSrv);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(g_deferred->Map(g_texVertexData, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
	std::memcpy(mapped.pData, verts.data(), count * sizeof(OverlayTexVertex));
	g_deferred->Unmap(g_texVertexData, 0);

	g_deferred->VSSetShader(g_texVs, nullptr, 0);
	g_deferred->PSSetShader(g_texPs, nullptr, 0);
	g_deferred->VSSetShaderResources(0, 1, &g_texVertexDataSrv);
	g_deferred->PSSetShaderResources(0, 1, &g_fontSrv);
	g_deferred->PSSetSamplers(0, 1, &g_sampler);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	g_deferred->OMSetBlendState(g_blend, blendFactor, 0xffffffff);
	g_deferred->Draw(count, 0);

	g_deferred->VSSetShaderResources(0, 1, nullSrv);
	g_deferred->PSSetShaderResources(0, 1, nullSrv);
	g_deferred->VSSetShader(g_vs, nullptr, 0);
	g_deferred->PSSetShader(g_ps, nullptr, 0);
	g_lastDigitVertexCount = (int)count;
}

void CreateDigitPipeline() {
	g_device->CreateVertexShader(kDeadlockOverlayTexVs, kDeadlockOverlayTexVsSize, nullptr, &g_texVs);
	g_device->CreatePixelShader(kDeadlockOverlayTexPs, kDeadlockOverlayTexPsSize, nullptr, &g_texPs);
	{
		D3D11_BUFFER_DESC vb = {};
		vb.ByteWidth = sizeof(OverlayTexVertex) * kMaxTexVertices;
		vb.Usage = D3D11_USAGE_DYNAMIC;
		vb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		vb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		vb.StructureByteStride = sizeof(OverlayTexVertex);
		if (SUCCEEDED(g_device->CreateBuffer(&vb, nullptr, &g_texVertexData)) && g_texVertexData) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_UNKNOWN;
			sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			sd.Buffer.FirstElement = 0;
			sd.Buffer.NumElements = kMaxTexVertices;
			g_device->CreateShaderResourceView(g_texVertexData, &sd, &g_texVertexDataSrv);
		}
	}
	{
		// hexfont is grayscale, embedded as luminance + alpha (DeadlockOverlayFont.h); expanded here.
		const unsigned texels = kDeadlockOverlayFontWidth * kDeadlockOverlayFontHeight;
		std::vector<unsigned char> rgba(texels * 4);
		for (unsigned i = 0; i < texels; ++i) {
			const unsigned char l = kDeadlockOverlayFontLA[i * 2], a = kDeadlockOverlayFontLA[i * 2 + 1];
			rgba[i * 4] = l; rgba[i * 4 + 1] = l; rgba[i * 4 + 2] = l; rgba[i * 4 + 3] = a;
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = kDeadlockOverlayFontWidth;
		td.Height = kDeadlockOverlayFontHeight;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const D3D11_SUBRESOURCE_DATA init{ rgba.data(), kDeadlockOverlayFontWidth * 4, 0 };
		if (SUCCEEDED(g_device->CreateTexture2D(&td, &init, &g_font)) && g_font)
			g_device->CreateShaderResourceView(g_font, nullptr, &g_fontSrv);
	}
	{
		// CampathDrawer's sampler: linear, clamped.
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxAnisotropy = 1;
		sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		sd.MinLOD = -D3D11_FLOAT32_MAX;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		g_device->CreateSamplerState(&sd, &g_sampler);
	}
}

} // namespace

bool OverlayDrawer_Ready() {
	return g_device && g_deferred && g_vs && g_ps && g_constants && g_vertexData && g_vertexDataSrv
		&& g_rasterizer && g_depthStencil && g_blend && g_blendAdditive;
}

void OverlayDrawer_SetEnabled(bool enabled) { g_enabled = enabled; }
bool OverlayDrawer_Enabled() { return g_enabled; }

void OverlayDrawer_BeginDevice(ID3D11Device* device) {
	OverlayDrawer_EndDevice();
	if (!device) return;

	device->AddRef();
	g_device = device;
	g_device->CreateDeferredContext(0, &g_deferred);

	g_device->CreateVertexShader(kDeadlockOverlayVs, kDeadlockOverlayVsSize, nullptr, &g_vs);
	g_device->CreatePixelShader(kDeadlockOverlayPs, kDeadlockOverlayPsSize, nullptr, &g_ps);

	{
		D3D11_BUFFER_DESC cb = {};
		cb.ByteWidth = sizeof(VsConstants);
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		g_device->CreateBuffer(&cb, nullptr, &g_constants);
	}
	{
		// A STRUCTURED buffer read by SV_VertexID in the vertex shader, not a vertex buffer:
		// Deadlock's software render command list retains vertex-buffer bindings made by callbacks,
		// and its internal list is 16-bit indexed.
		D3D11_BUFFER_DESC vb = {};
		vb.ByteWidth = sizeof(OverlayVertex) * kMaxVertices;
		vb.Usage = D3D11_USAGE_DYNAMIC;
		vb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		vb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		vb.StructureByteStride = sizeof(OverlayVertex);
		if (SUCCEEDED(g_device->CreateBuffer(&vb, nullptr, &g_vertexData)) && g_vertexData) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_UNKNOWN;
			sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			sd.Buffer.FirstElement = 0;
			sd.Buffer.NumElements = kMaxVertices;
			g_device->CreateShaderResourceView(g_vertexData, &sd, &g_vertexDataSrv);
		}
	}
	{
		// No culling: an extruded quad's winding depends on which way the line runs on screen.
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		g_device->CreateRasterizerState(&rd, &g_rasterizer);
	}
	{
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		g_device->CreateDepthStencilState(&dd, &g_depthStencil);
	}
	{
		// The trajectory and digits: straight alpha blend, like CS2's m_BlendState.
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		g_device->CreateBlendState(&bd, &g_blend);
	}
	{
		// The glow passes: additive, CS2's m_BlendStateAdditive (SrcAlpha, One).
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		g_device->CreateBlendState(&bd, &g_blendAdditive);
	}

	CreateDigitPipeline();

	advancedfx::Message("AfxHookDeadlock: overlay drawer device %s, key-number digits %s.\n",
	                    OverlayDrawer_Ready() ? "ready" : "INCOMPLETE", TexReady() ? "ready" : "UNAVAILABLE");
}

void OverlayDrawer_EndDevice() {
	if (g_sampler) { g_sampler->Release(); g_sampler = nullptr; }
	if (g_fontSrv) { g_fontSrv->Release(); g_fontSrv = nullptr; }
	if (g_font) { g_font->Release(); g_font = nullptr; }
	if (g_texVertexDataSrv) { g_texVertexDataSrv->Release(); g_texVertexDataSrv = nullptr; }
	if (g_texVertexData) { g_texVertexData->Release(); g_texVertexData = nullptr; }
	if (g_texPs) { g_texPs->Release(); g_texPs = nullptr; }
	if (g_texVs) { g_texVs->Release(); g_texVs = nullptr; }
	if (g_blendAdditive) { g_blendAdditive->Release(); g_blendAdditive = nullptr; }
	if (g_blend) { g_blend->Release(); g_blend = nullptr; }
	if (g_depthStencil) { g_depthStencil->Release(); g_depthStencil = nullptr; }
	if (g_rasterizer) { g_rasterizer->Release(); g_rasterizer = nullptr; }
	if (g_vertexDataSrv) { g_vertexDataSrv->Release(); g_vertexDataSrv = nullptr; }
	if (g_vertexData) { g_vertexData->Release(); g_vertexData = nullptr; }
	if (g_constants) { g_constants->Release(); g_constants = nullptr; }
	if (g_ps) { g_ps->Release(); g_ps = nullptr; }
	if (g_vs) { g_vs->Release(); g_vs = nullptr; }
	if (g_deferred) { g_deferred->Release(); g_deferred = nullptr; }
	if (g_device) { g_device->Release(); g_device = nullptr; }
}

void OverlayDrawer_SetHeroes(const OverlayHeroBox* boxes, int count) {
	if (count < 0 || !boxes) count = 0;
	if (count > kMaxHeroes) count = kMaxHeroes;
	std::lock_guard<std::mutex> lock(g_snapshotMutex);
	g_snapshotHeroes.assign(boxes, boxes + count);
}

void OverlayDrawer_SetPath(const OverlayPathPoint* points, int count) {
	if (count < 0 || !points) count = 0;
	if (count > kMaxPathPoints) count = kMaxPathPoints;
	std::lock_guard<std::mutex> lock(g_snapshotMutex);
	g_snapshotPath.assign(points, points + count);
}

void OverlayDrawer_SetMarkers(const OverlayMarker* markers, int count) {
	if (count < 0 || !markers) count = 0;
	if (count > kMaxMarkers) count = kMaxMarkers;
	std::lock_guard<std::mutex> lock(g_snapshotMutex);
	g_snapshotMarkers.assign(markers, markers + count);
}

void OverlayDrawer_SetPathStyle(bool freeze) {
	std::lock_guard<std::mutex> lock(g_snapshotMutex);
	g_snapshotFreeze = freeze;
}

void OverlayDrawer_RunPreUi() {
	if (!g_enabled || !OverlayDrawer_Ready()) return;

	std::vector<OverlayHeroBox> heroes;
	std::vector<OverlayPathPoint> path;
	std::vector<OverlayMarker> markers;
	bool freeze = false;
	{
		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		heroes = g_snapshotHeroes;
		path = g_snapshotPath;
		markers = g_snapshotMarkers;
		freeze = g_snapshotFreeze;
	}
	if (heroes.empty() && path.size() < 2 && markers.empty()) return;

	double origin[3], angles[3], fov;
	CameraRuntime_GetLastCamera(origin, angles, fov);
	if (!(fov > 1.0 && fov < 179.0)) { ++g_skips; return; }

	ID3D11DeviceContext* immediate = nullptr;
	g_device->GetImmediateContext(&immediate);
	if (!immediate) { ++g_skips; return; }

	ID3D11RenderTargetView* target = nullptr;
	immediate->OMGetRenderTargets(1, &target, nullptr);
	if (!target) { immediate->Release(); ++g_skips; return; }

	ID3D11Resource* resource = nullptr;
	target->GetResource(&resource);
	ID3D11Texture2D* backbuffer = nullptr;
	if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backbuffer);

	D3D11_TEXTURE2D_DESC bb = {};
	if (backbuffer) backbuffer->GetDesc(&bb);

	if (backbuffer && bb.Width > 0 && bb.Height > 0) {
		// The world renders FULL-SCREEN (DeadlockViewportBlit.h), so the pose was projected for the
		// whole backbuffer and the pixel widths are backbuffer pixels -- CS2's screen-width units.
		const OverlayCameraPose pose = MakeCameraPose(
			{ origin[0], origin[1], origin[2] }, angles[0], angles[1], angles[2],
			fov, (int)bb.Width, (int)bb.Height);
		const OverlayVec3 viewer{ origin[0], origin[1], origin[2] };

		std::vector<OverlayTexVertex> digitVerts;
		std::vector<OverlayVertex> alphaVerts, glowVerts;

		if (TexReady()) {
			for (size_t m = 0; m < markers.size(); ++m)
				PushKeyframeDigits(digitVerts, (int)m, { markers[m].x, markers[m].y, markers[m].z }, pose);
		}

		if (path.size() >= 2)
			PushTrajectory(alphaVerts, path, pose, kGizmoTrajectoryPixelWidth, freeze ? kTrajFreeze : kTrajLive);

		// Each marker's ten camera edges, computed once and stroked once per glow pass.
		std::vector<OverlaySegment3> gizmo(markers.size() * 10);
		for (size_t m = 0; m < markers.size(); ++m) {
			const OverlayVec3 at{ markers[m].x, markers[m].y, markers[m].z };
			CameraGizmoSegments(at, markers[m].pitch, markers[m].yaw, markers[m].roll, markers[m].fov,
				(int)bb.Width, (int)bb.Height, MarkerScreenScale(at, viewer), &gizmo[m * 10]);
		}
		for (const GlowPass& pass : kMarkerGlow) {
			for (size_t m = 0; m < markers.size(); ++m) {
				Rgba c = markers[m].hovered ? kMarkerAimed : (freeze ? kMarkerFreeze : kMarkerLive);
				c.a = pass.alpha / 255.0f;
				for (int e = 0; e < 10; ++e)
					PushWorldLine(glowVerts, gizmo[m * 10 + e].a, gizmo[m * 10 + e].b, pose,
						kGizmoLinePixelWidth * pass.widthMul, c);
			}
		}

		// The follow target: CS2's twelve-edge box, bottom face, top face, uprights.
		static const int kBoxEdges[12][2] = {
			{0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
		for (const GlowPass& pass : kBoxGlow) {
			Rgba c = kTargetBox;
			c.a = pass.alpha / 255.0f;
			for (const OverlayHeroBox& h : heroes) {
				const double x = h.feetX, y = h.feetY, z = h.feetZ, s = h.halfWidth, t = h.feetZ + h.height;
				const OverlayVec3 corners[8] = {
					{ x - s, y - s, z }, { x + s, y - s, z }, { x + s, y + s, z }, { x - s, y + s, z },
					{ x - s, y - s, t }, { x + s, y - s, t }, { x + s, y + s, t }, { x - s, y + s, t } };
				for (const auto& edge : kBoxEdges)
					PushWorldLine(glowVerts, corners[edge[0]], corners[edge[1]], pose,
						kGizmoLinePixelWidth * pass.widthMul, c);
			}
		}

		if (!digitVerts.empty() || !alphaVerts.empty() || !glowVerts.empty()) {
			VsConstants constants{ (float)bb.Width, (float)bb.Height, 0.0f, 0.0f };
			D3D11_MAPPED_SUBRESOURCE mappedCb;
			if (SUCCEEDED(g_deferred->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb))) {
				std::memcpy(mappedCb.pData, &constants, sizeof(constants));
				g_deferred->Unmap(g_constants, 0);
			}

			ID3D11RenderTargetView* rtvs[1] = { target };
			g_deferred->OMSetRenderTargets(1, rtvs, nullptr);
			D3D11_VIEWPORT vp = {};
			vp.Width = (float)bb.Width;
			vp.Height = (float)bb.Height;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			g_deferred->RSSetViewports(1, &vp);
			g_deferred->IASetInputLayout(nullptr);
			g_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g_deferred->VSSetShader(g_vs, nullptr, 0);
			g_deferred->VSSetConstantBuffers(0, 1, &g_constants);
			g_deferred->PSSetShader(g_ps, nullptr, 0);
			g_deferred->RSSetState(g_rasterizer);
			g_deferred->OMSetDepthStencilState(g_depthStencil, 0);

			// CS2's order: the key numbers, the trajectory, then the additive glow over both.
			DrawDigitBatch(digitVerts);
			DrawBatch(alphaVerts, g_blend);
			DrawBatch(glowVerts, g_blendAdditive);

			ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
			g_deferred->VSSetShaderResources(0, 1, nullSrv);

			ID3D11CommandList* commands = nullptr;
			g_deferred->FinishCommandList(FALSE, &commands);
			if (commands) {
				immediate->ExecuteCommandList(commands, TRUE);
				commands->Release();
			}

			++g_draws;
			g_lastVertexCount = (int)(alphaVerts.size() + glowVerts.size());
			if (Filmmaker::MvmDebugLog_Active() && (g_draws == 1 || 0 == (g_draws % 600)))
				Filmmaker::MvmDebugLog_Linef("deadlock.overlay",
					"run count=%llu path=%zu markers=%zu targets=%zu vertices=%d digitVertices=%zu truncated=%llu backbuffer=%ux%u",
					g_draws, path.size(), markers.size(), heroes.size(), g_lastVertexCount, digitVerts.size(),
					g_truncated, bb.Width, bb.Height);
		}
	} else {
		++g_skips;
	}

	if (backbuffer) backbuffer->Release();
	if (resource) resource->Release();
	target->Release();
	immediate->Release();
}

void OverlayDrawer_Report() {
	size_t targets = 0, pathPts = 0, markerCount = 0;
	bool freeze = false;
	{
		std::lock_guard<std::mutex> lock(g_snapshotMutex);
		targets = g_snapshotHeroes.size();
		pathPts = g_snapshotPath.size();
		markerCount = g_snapshotMarkers.size();
		freeze = g_snapshotFreeze;
	}
	advancedfx::Message(
		"  overlay:   %s  digits=%s  enabled=%s  published: path=%zu pts markers=%zu targets=%zu style=%s"
		"  draws=%llu skipped=%llu truncated=%llu  last vertices=%d digit vertices=%d\n",
		OverlayDrawer_Ready() ? "ready" : "NOT ready", TexReady() ? "ready" : "unavailable",
		g_enabled ? "yes" : "no", pathPts, markerCount, targets, freeze ? "freeze" : "live",
		g_draws, g_skips, g_truncated, g_lastVertexCount, g_lastDigitVertexCount);
}

} // namespace Deadlock
