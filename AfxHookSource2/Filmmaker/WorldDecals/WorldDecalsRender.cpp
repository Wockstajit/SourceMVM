#define NOMINMAX
#include "WorldDecalsRender.h"
#include "WorldDecals.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderPrivateTextureScope.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace Filmmaker::WorldDecals {
using Microsoft::WRL::ComPtr;
namespace {

constexpr int kMaxActors = 12;

constexpr char kShader[] = R"HLSL(
cbuffer DecalConstants : register(b0) {
    row_major float4x4 invViewProj;
    float4 dims;          // target w, h; depth w, h
    float4 depthInfo;     // minDepth, maxDepth, reversed, actorCount
    float4 cameraTime;    // cam xyz, demoTime
    float4 posKind;       // decal center xyz, kind (0=text,1=image,2=video,3=arrow,4=route)
    float4 rightW;        // right xyz, halfWidth
    float4 upH;           // up xyz, halfHeight
    float4 fwdD;          // forward xyz, halfDepth
    float4 color;         // rgba
    float4 flags;         // blendMode, normalFade, skipPlayers, hasTexture
    float4 shapeA;        // arrowHead/routeWidth, arrowShaft/routeDrawnLen, dashed, pulse/arrowCap
    float4 shapeB;        // routePointCount, 0, 0, 0
    float4 actors[12];    // live players' feet
    float4 routePts[32];  // xyz = waypoint, w = cumulative arc length at waypoint
};

#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t0);
#else
Texture2D<float> sceneDepth : register(t0);
#endif
Texture2D<float4> decalTex : register(t1);
Texture2D<float4> sceneColor : register(t2);
SamplerState linearClamp : register(s0);

float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

bool Reversed() { return depthInfo.z > 0.5; }

float RawDepth(int2 p) {
#if DEPTH_MS
    return sceneDepth.Load(p, 0);
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}

bool WorldAt(float2 pixel, out float3 world) {
    world = float3(0, 0, 0);
    int2 p = clamp(int2(pixel * dims.zw / dims.xy), int2(0, 0), int2(dims.zw) - 1);
    float d = RawDepth(p);
    if (Reversed() ? d < 1e-7 : d > 0.9999999) return false;
    if (!Reversed() && d <= 0.100001) return false;
    float span = max(depthInfo.y - depthInfo.x, 1e-12);
    float n = (d - depthInfo.x) / span;
    if (n < 0.0 || n > 1.0) return false;
    float4 h = mul(invViewProj, float4(pixel / dims.xy * float2(2, -2) + float2(-1, 1), n, 1));
    if (abs(h.w) < 1e-10) return false;
    world = h.xyz / h.w;
    return true;
}

float3 SurfaceNormal(float2 pixel, float3 c) {
    float3 r, l, d, u;
    bool hr = WorldAt(pixel + float2(1, 0), r), hl = WorldAt(pixel - float2(1, 0), l);
    bool hd = WorldAt(pixel + float2(0, 1), d), hu = WorldAt(pixel - float2(0, 1), u);
    if (!(hr || hl) || !(hd || hu)) return float3(0, 0, 1);
    float3 xr = r - c, xl = c - l, yd = d - c, yu = c - u;
    float3 dx = hr && (!hl || dot(xr, xr) < dot(xl, xl)) ? xr : xl;
    float3 dy = hd && (!hu || dot(yd, yd) < dot(yu, yu)) ? yd : yu;
    float3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-8) return float3(0, 0, 1);
    n /= len;
    return dot(n, cameraTime.xyz - c) < 0 ? -n : n;
}

bool HitPlayer(float3 w) {
    int count = int(depthInfo.w + 0.5);
    [loop] for (int i = 0; i < count; ++i) {
        float3 a = actors[i].xyz;
        if (length(w.xy - a.xy) < 20.0 && w.z > a.z + 2.0 && w.z < a.z + 78.0) return true;
    }
    return false;
}

float4 EvaluateArrow(float2 uv) {
    // Arrow points upward (-V in UV space: v=0 tip, v=1 tail).
    float headLen = clamp(shapeA.x, 0.12, 0.75);
    float shaftHalf = clamp(shapeA.y * 0.5, 0.03, 0.35);
    float x = abs(uv.x - 0.5);
    float y = uv.y;
    float mask = 0.0;
    if (y < headLen) {
        float halfW = (y / max(headLen, 1e-4)) * 0.45;
        mask = smoothstep(halfW + 0.015, halfW - 0.015, x);
    } else {
        mask = smoothstep(shaftHalf + 0.015, shaftHalf - 0.015, x);
        if (shapeA.z > 0.5) {
            float dash = frac(y * 8.0 - cameraTime.w * shapeA.w * 0.35);
            mask *= smoothstep(0.0, 0.08, dash) * smoothstep(0.65, 0.57, dash);
        }
    }
    float pulse = shapeA.w > 0.05 ? (0.75 + 0.25 * sin(y * 12.0 + cameraTime.w * shapeA.w)) : 1.0;
    return float4(color.rgb * pulse, mask * color.a);
}

float4 EvaluateRoute(float3 w, float3 normal) {
    int ptCount = int(shapeB.x + 0.5);
    float drawnLen = shapeA.y;
    float halfW = max(shapeA.x * 0.5, 1.5);
    if (ptCount < 2 || drawnLen <= 0.5) return float4(0, 0, 0, 0);

    float bestDist = 1e9;
    float bestArc = 0.0;
    float3 tipPos = routePts[0].xyz;
    float3 tipDir = float3(1, 0, 0);

    [loop] for (int i = 1; i < ptCount; ++i) {
        float prevArc = routePts[i - 1].w;
        if (prevArc >= drawnLen) break;
        float3 a = routePts[i - 1].xyz;
        float3 b = routePts[i].xyz;
        float segLen = max(routePts[i].w - prevArc, 1e-4);
        float used = min(segLen, drawnLen - prevArc);
        float3 bClip = lerp(a, b, used / segLen);
        float3 ab = bClip - a;
        float ab2 = dot(ab, ab);
        float t = ab2 > 1e-5 ? saturate(dot(w - a, ab) / ab2) : 0.0;
        float3 c = a + ab * t;
        // Weight Z generously so painted ground routes hug steps and slight slopes cleanly.
        float3 diff = w - c;
        float d = length(float3(diff.xy, diff.z * 0.55));
        if (d < bestDist) {
            bestDist = d;
            bestArc = prevArc + used * t;
        }
        if (prevArc + segLen >= drawnLen - 1e-3 || i == ptCount - 1) {
            tipPos = bClip;
            tipDir = normalize(ab + float3(1e-5, 0, 0));
        }
    }

    float lineMask = smoothstep(halfW + 2.0, max(halfW - 1.5, 0.1), bestDist);
    if (shapeA.z > 0.5) {
        float dash = frac(bestArc / max(halfW * 4.5, 12.0));
        lineMask *= smoothstep(0.0, 0.08, dash) * smoothstep(0.68, 0.60, dash);
    }

    // Optional directional arrowhead at the animated leading tip of the route!
    float arrowMask = 0.0;
    if (shapeA.w > 0.5 && drawnLen > halfW * 1.5) {
        float3 rel = w - tipPos;
        float along = dot(rel, tipDir);
        float3 sideVec = normalize(cross(float3(0, 0, 1), tipDir) + float3(0, 1e-5, 0));
        float across = abs(dot(rel, sideVec));
        float zOff = abs(rel.z);
        float arrowLen = halfW * 3.4;
        if (along <= 2.0 && along >= -arrowLen && zOff < 28.0) {
            float fracBack = saturate(-along / arrowLen);
            float wAt = fracBack * halfW * 2.2;
            arrowMask = smoothstep(wAt + 1.5, max(wAt - 1.2, 0.0), across) * smoothstep(28.0, 14.0, zOff);
        }
    }

    float upFade = smoothstep(0.25, 0.55, abs(normal.z));
    float mask = max(lineMask, arrowMask) * upFade;
    // Slight bright leading-edge pulse as the route draws itself!
    float headGlow = smoothstep(halfW * 4.0, 0.0, abs(drawnLen - bestArc));
    float3 rgb = color.rgb * (1.0 + headGlow * 0.45);
    return float4(rgb, mask * color.a);
}

float4 PS(float4 pos : SV_Position) : SV_Target {
    float3 w;
    if (!WorldAt(pos.xy, w)) discard;
    if (flags.z > 0.5 && HitPlayer(w)) discard;

    int kind = int(posKind.w + 0.5);
    float3 n = SurfaceNormal(pos.xy, w);
    float4 sampleCol = float4(0, 0, 0, 0);

    if (kind == 4) {
        sampleCol = EvaluateRoute(w, n);
        if (sampleCol.a <= 0.004) discard;
    } else {
        float3 rel = w - posKind.xyz;
        float lx = dot(rel, rightW.xyz);
        float ly = dot(rel, upH.xyz);
        float lz = dot(rel, fwdD.xyz);
        if (abs(lx) > rightW.w || abs(ly) > upH.w || abs(lz) > fwdD.w) discard;

        float2 uv = float2(lx / (2.0 * rightW.w) + 0.5, 0.5 - ly / (2.0 * upH.w));
        // Fade gracefully on grazing angles relative to projection direction.
        float facing = abs(dot(n, fwdD.xyz));
        float angleFade = smoothstep(flags.y * 0.7, min(flags.y + 0.25, 1.0), facing);
        float depthFade = 1.0 - smoothstep(0.82, 1.0, abs(lz) / max(fwdD.w, 1e-4));

        if (kind == 3) {
            sampleCol = EvaluateArrow(uv);
        } else {
            if (flags.w < 0.5) discard;
            float4 tex = decalTex.SampleLevel(linearClamp, uv, 0);
            sampleCol = float4(tex.rgb * color.rgb, tex.a * color.a);
        }
        sampleCol.a *= angleFade * depthFade;
        if (sampleCol.a <= 0.004) discard;
    }

    int blendMode = int(flags.x + 0.5);
    int2 px = clamp(int2(pos.xy), int2(0, 0), int2(dims.xy) - 1);
    float3 bg = sceneColor.Load(int3(px, 0)).rgb;

    if (blendMode == 1) {
        // Additive glow/projection
        return float4(bg + sampleCol.rgb * sampleCol.a, 1.0);
    } else if (blendMode == 2) {
        // Multiply (stained/painted ink)
        return float4(lerp(bg, bg * sampleCol.rgb, sampleCol.a), 1.0);
    } else if (blendMode == 3) {
        // LitPaint: adopt local wall luminance so posters/lines sit naturally in shadow & light!
        float luma = clamp(dot(bg, float3(0.299, 0.587, 0.114)) * 1.45 + 0.18, 0.18, 1.35);
        return float4(lerp(bg, sampleCol.rgb * luma, sampleCol.a), 1.0);
    }
    return float4(lerp(bg, sampleCol.rgb, sampleCol.a), 1.0);
}
)HLSL";

struct Constants {
	float invViewProj[16];
	float dims[4];
	float depthInfo[4];
	float cameraTime[4];
	float posKind[4];
	float rightW[4];
	float upH[4];
	float fwdD[4];
	float color[4];
	float flags[4];
	float shapeA[4];
	float shapeB[4];
	float actors[kMaxActors][4];
	float routePts[kMaxRoutePoints][4];
};
static_assert(sizeof(Constants) % 16 == 0, "cb alignment");

struct Blobs {
	ComPtr<ID3DBlob> vs, ps[2];
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const Blobs> g_blobs;
std::atomic<bool> g_compileStarted{ false };
std::thread g_compiler;

struct CachedTex {
	uint64_t revision = 0;
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11ShaderResourceView> srv;
};

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps[2];
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> dsOff;
	ComPtr<ID3D11BlendState> blendOpaque;

	ComPtr<ID3D11Texture2D> colorCopy, depthCopy;
	ComPtr<ID3D11ShaderResourceView> colorSrv, depthSrv;
	UINT colorW = 0, colorH = 0;
	DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
	D3D11_TEXTURE2D_DESC depthDesc{};
	std::map<int, CachedTex> textures;
	bool failed = false;
} g_gpu;

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT DepthTypelessFormat(DXGI_FORMAT f) {
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

void CompileAll() {
	auto b = std::make_shared<Blobs>();
	ComPtr<ID3DBlob> err;
	const D3D_SHADER_MACRO s0[] = { { "DEPTH_MS", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO s1[] = { { "DEPTH_MS", "1" }, { nullptr, nullptr } };
	const size_t n = sizeof(kShader) - 1;
	b->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldDecals", s0, "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->vs, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldDecals", s0, "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->ps[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldDecals", s1, "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->ps[1], &err));
	if (!b->ok)
		MvmDebugLog_LinefAlways("worlddecals", "shader compile failed: %s",
			err ? static_cast<const char*>(err->GetBufferPointer()) : "compiler unavailable");
	std::lock_guard<std::mutex> lock(g_blobLock);
	g_blobs = b;
}

bool EnsureShaders() {
	if (g_gpu.vs) return true;
	if (!g_gpu.device || g_gpu.failed) return false;
	std::shared_ptr<const Blobs> b;
	{ std::lock_guard<std::mutex> lock(g_blobLock); b = g_blobs; }
	if (!b) return false;
	if (!b->ok) { g_gpu.failed = true; return false; }

	RenderPrivateTextureScope priv;
	auto* d = g_gpu.device.Get();
	if (FAILED(d->CreateVertexShader(b->vs->GetBufferPointer(), b->vs->GetBufferSize(), nullptr, &g_gpu.vs))
		|| FAILED(d->CreatePixelShader(b->ps[0]->GetBufferPointer(), b->ps[0]->GetBufferSize(), nullptr, &g_gpu.ps[0]))
		|| FAILED(d->CreatePixelShader(b->ps[1]->GetBufferPointer(), b->ps[1]->GetBufferSize(), nullptr, &g_gpu.ps[1]))) {
		g_gpu.failed = true;
		return false;
	}
	D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(Constants); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d->CreateBuffer(&cbd, nullptr, &g_gpu.cb);
	D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	d->CreateSamplerState(&sd, &g_gpu.sampler);
	D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	d->CreateRasterizerState(&rd, &g_gpu.raster);
	D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	d->CreateDepthStencilState(&dd, &g_gpu.dsOff);
	D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	d->CreateBlendState(&bd, &g_gpu.blendOpaque);
	return g_gpu.cb && g_gpu.sampler && g_gpu.raster && g_gpu.dsOff && g_gpu.blendOpaque;
}

ID3D11ShaderResourceView* EnsureDecalTexture(int id, const std::shared_ptr<const TextureBitmap>& bmp) {
	if (!bmp || !bmp->width || !bmp->height || bmp->rgba.size() < (size_t)bmp->width * bmp->height * 4)
		return nullptr;
	auto& slot = g_gpu.textures[id];
	if (slot.srv && slot.revision == bmp->revision) return slot.srv.Get();
	RenderPrivateTextureScope priv;
	slot.tex.Reset(); slot.srv.Reset();
	D3D11_TEXTURE2D_DESC td{};
	td.Width = bmp->width; td.Height = bmp->height; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{};
	init.pSysMem = bmp->rgba.data();
	init.SysMemPitch = bmp->width * 4;
	if (FAILED(g_gpu.device->CreateTexture2D(&td, &init, &slot.tex))
		|| FAILED(g_gpu.device->CreateShaderResourceView(slot.tex.Get(), nullptr, &slot.srv))) {
		slot.tex.Reset(); slot.srv.Reset();
		return nullptr;
	}
	slot.revision = bmp->revision;
	return slot.srv.Get();
}

bool EnsureColorCopy(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, DXGI_FORMAT viewFmt) {
	D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
	const DXGI_FORMAT typed = TypedColorFormat(viewFmt != DXGI_FORMAT_UNKNOWN ? viewFmt : d.Format);
	if (!g_gpu.colorCopy || g_gpu.colorW != d.Width || g_gpu.colorH != d.Height || g_gpu.colorFmt != typed) {
		RenderPrivateTextureScope priv;
		g_gpu.colorCopy.Reset(); g_gpu.colorSrv.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.MipLevels = 1; td.ArraySize = 1; td.Format = typed; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
		td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.colorCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.colorCopy.Get(), nullptr, &g_gpu.colorSrv)))
			return false;
		g_gpu.colorW = d.Width; g_gpu.colorH = d.Height; g_gpu.colorFmt = typed;
	}
	if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_gpu.colorCopy.Get(), 0, src, 0, typed);
	else ctx->CopyResource(g_gpu.colorCopy.Get(), src);
	return true;
}

bool EnsureDepthCopy(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, bool& isMsaa, UINT& w, UINT& h) {
	if (!dsv) return false;
	ComPtr<ID3D11Resource> res; dsv->GetResource(&res);
	ComPtr<ID3D11Texture2D> tex; if (!res || FAILED(res.As(&tex))) return false;
	D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
	const DXGI_FORMAT typeless = DepthTypelessFormat(d.Format);
	const DXGI_FORMAT srvFmt = DepthSrvFormat(d.Format);
	if (typeless == DXGI_FORMAT_UNKNOWN || srvFmt == DXGI_FORMAT_UNKNOWN) return false;
	isMsaa = d.SampleDesc.Count > 1;
	w = d.Width; h = d.Height;
	if (!g_gpu.depthCopy || g_gpu.depthDesc.Width != d.Width || g_gpu.depthDesc.Height != d.Height
		|| g_gpu.depthDesc.Format != typeless || g_gpu.depthDesc.SampleDesc.Count != d.SampleDesc.Count) {
		RenderPrivateTextureScope priv;
		g_gpu.depthCopy.Reset(); g_gpu.depthSrv.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.Format = typeless; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = srvFmt;
		sd.ViewDimension = isMsaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
		if (!isMsaa) sd.Texture2D.MipLevels = 1;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.depthCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.depthCopy.Get(), &sd, &g_gpu.depthSrv)))
			return false;
		g_gpu.depthDesc = td;
	}
	ctx->CopyResource(g_gpu.depthCopy.Get(), tex.Get());
	return true;
}

} // namespace

void BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;
	g_gpu.device = device;
	g_gpu.failed = false;
	device->CreateDeferredContext(0, &g_gpu.ctx);
	if (!g_compileStarted.exchange(true)) {
		g_compiler = std::thread(CompileAll);
		g_compiler.detach();
	}
}

void EndDevice() {
	g_gpu = {};
}

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, const float* projection,
	const float camera[3], float depthMin, float depthMax,
	const float (*actors)[4], int actorCount, double demoTime, bool recording) {
	if (!AnyEnabled() || !immediate || !target || !depth || !worldToClip || !g_gpu.ctx || !EnsureShaders())
		return false;

	const auto items = RenderSnapshot();
	if (items.empty()) return false;

	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return false;
	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);

	bool isMsaa = false;
	UINT dw = 0, dh = 0;
	if (!EnsureDepthCopy(g_gpu.ctx.Get(), depth, isMsaa, dw, dh)) {
		ComPtr<ID3D11CommandList> drop; g_gpu.ctx->FinishCommandList(FALSE, &drop);
		return false;
	}

	Environment::Matrix vp{}, inv{};
	std::memcpy(vp.data(), worldToClip, sizeof(float) * 16);
	Environment::Inverse(vp, inv);
	const bool reversed = projection && projection[10] < 0.0f;

	auto* c = g_gpu.ctx.Get();
	D3D11_TEXTURE2D_DESC srcDesc{}; src->GetDesc(&srcDesc);
	D3D11_VIEWPORT vpRect{};
	vpRect.Width = (float)srcDesc.Width; vpRect.Height = (float)srcDesc.Height; vpRect.MaxDepth = 1.0f;
	c->RSSetViewports(1, &vpRect);
	c->RSSetState(g_gpu.raster.Get());
	c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
	const float bf[4] = {};
	c->OMSetBlendState(g_gpu.blendOpaque.Get(), bf, 0xffffffff);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
	c->PSSetShader(g_gpu.ps[isMsaa ? 1 : 0].Get(), nullptr, 0);
	ID3D11SamplerState* smps[1] = { g_gpu.sampler.Get() };
	c->PSSetSamplers(0, 1, smps);

	int drawnCount = 0;
	for (const auto& item : items) {
		const Decal& d = item.decal;
		if (!d.enabled || (recording && !d.render)) continue;
		ID3D11ShaderResourceView* texSrv = nullptr;
		if (d.kind == DecalKind::Text || d.kind == DecalKind::Image || d.kind == DecalKind::Video) {
			texSrv = EnsureDecalTexture(d.id, item.texture);
			if (!texSrv) continue;
		}
		if (!EnsureColorCopy(c, src.Get(), rtvd.Format)) continue;

		Vec3 fwd, right, up;
		BasisFromAngles(d.angles, fwd, right, up);
		Constants cb{};
		std::memcpy(cb.invViewProj, inv.data(), sizeof(float) * 16);
		cb.dims[0] = (float)srcDesc.Width; cb.dims[1] = (float)srcDesc.Height;
		cb.dims[2] = (float)dw; cb.dims[3] = (float)dh;
		const int actN = std::clamp(actorCount, 0, kMaxActors);
		cb.depthInfo[0] = depthMin; cb.depthInfo[1] = depthMax;
		cb.depthInfo[2] = reversed ? 1.0f : 0.0f; cb.depthInfo[3] = (float)actN;
		cb.cameraTime[0] = camera ? camera[0] : 0.0f;
		cb.cameraTime[1] = camera ? camera[1] : 0.0f;
		cb.cameraTime[2] = camera ? camera[2] : 0.0f;
		cb.cameraTime[3] = (float)demoTime;
		cb.posKind[0] = d.position[0]; cb.posKind[1] = d.position[1]; cb.posKind[2] = d.position[2];
		cb.posKind[3] = (float)(int)d.kind;
		cb.rightW[0] = right.x; cb.rightW[1] = right.y; cb.rightW[2] = right.z; cb.rightW[3] = std::max(d.width * 0.5f, 1.0f);
		cb.upH[0] = up.x; cb.upH[1] = up.y; cb.upH[2] = up.z; cb.upH[3] = std::max(d.height * 0.5f, 1.0f);
		cb.fwdD[0] = fwd.x; cb.fwdD[1] = fwd.y; cb.fwdD[2] = fwd.z; cb.fwdD[3] = std::max(d.depth * 0.5f, 1.0f);
		for (int k = 0; k < 4; ++k) cb.color[k] = d.color[k];
		cb.flags[0] = (float)(int)d.blend;
		cb.flags[1] = d.normalFade;
		cb.flags[2] = d.skipPlayers ? 1.0f : 0.0f;
		cb.flags[3] = texSrv ? 1.0f : 0.0f;

		if (d.kind == DecalKind::Route) {
			const float progress = EvaluateRouteProgress(d, demoTime);
			const float totalLen = RouteTotalLength(d.routePoints);
			cb.shapeA[0] = d.routeWidth;
			cb.shapeA[1] = totalLen * progress;
			cb.shapeA[2] = d.routeDashed ? 1.0f : 0.0f;
			cb.shapeA[3] = d.routeArrowCap ? 1.0f : 0.0f;
			const int ptN = std::min((int)d.routePoints.size(), kMaxRoutePoints);
			cb.shapeB[0] = (float)ptN;
			float accum = 0.0f;
			for (int i = 0; i < ptN; ++i) {
				if (i > 0) accum += Length(Sub(d.routePoints[i], d.routePoints[i - 1]));
				cb.routePts[i][0] = d.routePoints[i].x;
				cb.routePts[i][1] = d.routePoints[i].y;
				cb.routePts[i][2] = d.routePoints[i].z;
				cb.routePts[i][3] = accum;
			}
		} else {
			cb.shapeA[0] = d.arrowHead;
			cb.shapeA[1] = d.arrowShaft;
			cb.shapeA[2] = d.arrowDashed ? 1.0f : 0.0f;
			cb.shapeA[3] = d.arrowPulse;
		}
		for (int i = 0; i < actN; ++i) {
			for (int k = 0; k < 4; ++k) cb.actors[i][k] = actors[i][k];
		}

		c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &cb, 0, 0);
		ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
		c->PSSetConstantBuffers(0, 1, cbs);
		ID3D11RenderTargetView* rts[1] = { target };
		c->OMSetRenderTargets(1, rts, nullptr);
		ID3D11ShaderResourceView* srvs[3] = { g_gpu.depthSrv.Get(), texSrv, g_gpu.colorSrv.Get() };
		c->PSSetShaderResources(0, 3, srvs);
		c->Draw(3, 0);
		ID3D11ShaderResourceView* nulls[3] = {};
		c->PSSetShaderResources(0, 3, nulls);
		ID3D11RenderTargetView* nullRt[1] = {};
		c->OMSetRenderTargets(1, nullRt, nullptr);
		++drawnCount;
	}

	ComPtr<ID3D11CommandList> cl;
	if (!drawnCount || FAILED(c->FinishCommandList(FALSE, &cl)) || !cl) return false;
	{ GPU_PASS_SCOPE(immediate, "world decals"); immediate->ExecuteCommandList(cl.Get(), TRUE); }
	return true;
}

} // namespace Filmmaker::WorldDecals
