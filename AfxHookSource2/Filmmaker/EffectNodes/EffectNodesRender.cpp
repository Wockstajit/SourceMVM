#define NOMINMAX
#include "EffectNodesRender.h"
#include "EffectNodes.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderPlayerMatte.h"
#include "../Render/RenderPrivateTextureScope.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace Filmmaker::EffectNodes {
using Microsoft::WRL::ComPtr;
namespace {

constexpr char kShader[] = R"HLSL(
cbuffer NodeConstants : register(b0) {
    row_major float4x4 invViewProj;
    float4 dims;         // target w, h; depth w, h
    float4 depthInfo;    // minDepth, maxDepth, reversed, hasWorldDepth
    float4 cameraTime;   // cam xyz, demoTime
    float4 camFwd;       // fwd xyz, hasMaskA
    float4 stepInfo;     // nodeType, hasMaskB, masterMix, 0
    float4 p0;           // params[0..3]
    float4 p1;           // params[4..7]
    int4   pmCounts;     // hasTarget, targetBoneCount, otherCount, 0
    float4 pmBounds;     // xyz, maxR
    float4 pmCapsule;    // feet xyz, height
    float4 pmBones[16];
    float4 pmOthers[16];
};

Texture2D<float4> colorIn : register(t0);
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float> worldDepth : register(t2);
Texture2D<float> maskA : register(t3);
Texture2D<float> maskB : register(t4);
Texture2D<float4> origColor : register(t5);
SamplerState linearClamp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut VS(uint id : SV_VertexID) {
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

bool Reversed() { return depthInfo.z > 0.5; }

float RawDepth(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.zw) - 1);
#if DEPTH_MS
    return sceneDepth.Load(p, 0);
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}

bool WorldAtUv(float2 uv, out float3 world) {
    world = cameraTime.xyz;
    int2 p = clamp(int2(uv * dims.zw), int2(0, 0), int2(dims.zw) - 1);
    float d = RawDepth(p);
    if (Reversed() ? d < 1e-7 : d > 0.9999999) return false;
    if (!Reversed() && d <= 0.100001) return false;
    float span = max(depthInfo.y - depthInfo.x, 1e-12);
    float n = (d - depthInfo.x) / span;
    if (n < 0.0 || n > 1.0) return false;
    float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, n, 1.0));
    if (abs(h.w) < 1e-10) return false;
    world = h.xyz / h.w;
    return true;
}

float CameraDistAtUv(float2 uv) {
    float3 w;
    if (!WorldAtUv(uv, w)) return 1e30;
    return dot(w - cameraTime.xyz, camFwd.xyz);
}

float WorldRefDistAtUv(float2 uv) {
    if (depthInfo.w < 0.5) return 1e30;
    int2 p = clamp(int2(uv * dims.xy), int2(0, 0), int2(dims.xy) - 1);
    float d = worldDepth.Load(int3(p, 0));
    if (Reversed() ? d < 1e-7 : d > 0.9999999) return 1e30;
    float span = max(depthInfo.y - depthInfo.x, 1e-12);
    float n = (d - depthInfo.x) / span;
    if (n < 0.0 || n > 1.0) return 1e30;
    float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, n, 1.0));
    if (abs(h.w) < 1e-10) return 1e30;
    return dot(h.xyz / h.w - cameraTime.xyz, camFwd.xyz);
}

float CapsuleSdf(float3 p, float3 feet, float height, float radius) {
    float pad = min(radius, height * 0.45);
    float3 a = feet + float3(0, 0, pad);
    float3 b = feet + float3(0, 0, max(pad, height - pad));
    float3 ab = b - a;
    float denom = dot(ab, ab);
    float t = denom > 1e-6 ? saturate(dot(p - a, ab) / denom) : 0.0;
    return length((a + ab * t) - p) - radius;
}

float SelectedPlayerWeight(float3 p) {
    if (pmCounts.x == 0) return 1.0;
    float3 bd = p - pmBounds.xyz;
    float maxR = pmBounds.w > 1.0 ? pmBounds.w : 96.0;
    if (dot(bd, bd) > maxR * maxR) return 0.0;
    float dTarget = 1e9;
    if (pmCounts.y > 0) {
        [loop] for (int i = 0; i < pmCounts.y; ++i) {
            float r = pmBones[i].w > 1.0 ? pmBones[i].w : 16.0;
            dTarget = min(dTarget, length(p - pmBones[i].xyz) - r);
        }
    } else {
        dTarget = CapsuleSdf(p, pmCapsule.xyz, pmCapsule.w > 1.0 ? pmCapsule.w : 74.0, 22.0);
    }
    if (dTarget > 32.0) return 0.0;
    if (pmCounts.z > 0) {
        float dOther = 1e9;
        [loop] for (int j = 0; j < pmCounts.z; ++j) {
            dOther = min(dOther, CapsuleSdf(p, pmOthers[j].xyz, pmOthers[j].w > 1.0 ? pmOthers[j].w : 74.0, 18.0));
        }
        return saturate((dOther - dTarget + 1.5) / 3.0);
    }
    return 1.0;
}

// ---- Mask Evaluation Pass (writes R8_UNORM) ----
float MaskPS(VsOut i) : SV_Target {
    int type = int(stepInfo.x + 0.5);
    float m = 0.0;
    if (type == 0) {
        // DepthRange: near=p0.x, far=p0.y, softness=p0.z, invert=p0.w
        float d = CameraDistAtUv(i.uv);
        float soft = max(p0.z, 1.0);
        float inNear = smoothstep(p0.x - soft, p0.x + soft, d);
        float inFar = 1.0 - smoothstep(p0.y - soft, p0.y + soft, d);
        m = (d < 1e29) ? (inNear * inFar) : 0.0;
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 1) {
        // SkyMask: cutoff=p0.x, softness=p0.y, invert=p0.z
        float d = CameraDistAtUv(i.uv);
        m = (d > 1e29) ? 1.0 : smoothstep(p0.x - max(p0.y, 1.0), p0.x + max(p0.y, 1.0), d);
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 2) {
        // PlayerMatte: selectedOnly=p0.x, feather=p0.y, invert=p0.z
        float3 w;
        if (WorldAtUv(i.uv, w)) {
            float here = dot(w - cameraTime.xyz, camFwd.xyz);
            float worldD = WorldRefDistAtUv(i.uv);
            float isPlayer = (depthInfo.w > 0.5)
                ? ((here < 1e29 && here < worldD - max(1.5, worldD * 0.004)) ? 1.0 : 0.0)
                : ((here < 900.0) ? SelectedPlayerWeight(w) : 0.0);
            if (isPlayer > 0.01 && p0.x > 0.5)
                isPlayer *= SelectedPlayerWeight(w);
            m = isPlayer;
        }
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 3) {
        // FogMask: start=p0.x, end=p0.y, density=p0.z, invert=p0.w
        float d = CameraDistAtUv(i.uv);
        float fog = (d > 1e29) ? 0.65 : saturate((d - p0.x) / max(p0.y - p0.x, 1.0));
        m = saturate(1.0 - exp(-fog * max(p0.z, 0.01) * 1.8));
        if (p0.w > 0.5) m = 1.0 - m;
    } else if (type == 4) {
        // LumaKey: threshold=p0.x, softness=p0.y, invert=p0.z
        float3 c = colorIn.SampleLevel(linearClamp, i.uv, 0).rgb;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        m = smoothstep(p0.x - max(p0.y, 0.01), p0.x + max(p0.y, 0.01), l);
        if (p0.z > 0.5) m = 1.0 - m;
    } else if (type == 5) {
        // MaskCombine: op=p0.x (0=union, 1=intersect, 2=subtract, 3=invert)
        float a = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, i.uv, 0) : 1.0;
        float b = stepInfo.y > 0.5 ? maskB.SampleLevel(linearClamp, i.uv, 0) : 0.0;
        int op = int(p0.x + 0.5);
        if (op == 0) m = max(a, b);
        else if (op == 1) m = min(a, b);
        else if (op == 2) m = saturate(a - b);
        else m = 1.0 - a;
    }
    return saturate(m);
}

// ---- Effect Processing Pass ----
float4 EffectPS(VsOut i) : SV_Target {
    int type = int(stepInfo.x + 0.5);
    float2 texel = 1.0 / max(dims.xy, float2(1.0, 1.0));
    float4 src = colorIn.SampleLevel(linearClamp, i.uv, 0);
    float m = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, i.uv, 0) : 1.0;
    float3 outRgb = src.rgb;

    if (type == 6) {
        // ColorGrade: sat=p0.x, contrast=p0.y, exp=p0.z, temp=p0.w, tint=p1.xyz, mix=p1.w
        float3 c = src.rgb * exp2(p0.z);
        c = (c - 0.5) * p0.y + 0.5;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = lerp(float3(l, l, l), c, p0.x);
        c.r *= (1.0 + p0.w * 0.25);
        c.b *= (1.0 - p0.w * 0.25);
        c *= p1.xyz;
        outRgb = lerp(src.rgb, saturate(c), saturate(m * p1.w));
    } else if (type == 7) {
        // Glow: thresh=p0.x, intensity=p0.y, radius=p0.z, tint=p0.w/p1.xy, mix=p1.z
        float thresh = p0.x;
        float intensity = p0.y;
        float rad = max(p0.z, 1.0);
        float3 tint = float3(p0.w, p1.x, p1.y);
        float3 accum = float3(0, 0, 0);
        float wsum = 0.0;
        [unroll] for (int r = 1; r <= 2; ++r) {
            float rr = rad * (float(r) * 0.55);
            [unroll] for (int a = 0; a < 8; ++a) {
                float ang = (float(a) + 0.5 * float(r)) * 0.78539816;
                float2 off = float2(cos(ang), sin(ang)) * rr * texel;
                float2 suv = saturate(i.uv + off);
                float3 sc = colorIn.SampleLevel(linearClamp, suv, 0).rgb;
                float sm = camFwd.w > 0.5 ? maskA.SampleLevel(linearClamp, suv, 0) : 1.0;
                float sl = dot(sc, float3(0.2126, 0.7152, 0.0722));
                float bright = smoothstep(thresh, thresh + 0.35, sl);
                float wt = (r == 1) ? 1.0 : 0.6;
                accum += sc * bright * sm * wt;
                wsum += wt;
            }
        }
        float3 bloom = (accum / max(wsum, 1e-4)) * tint * intensity;
        outRgb = saturate(src.rgb + bloom * saturate(p1.z));
    } else if (type == 8) {
        // Outline: thickness=p0.x, threshold=p0.y, color=p0.z/p0.w/p1.x, opacity=p1.y
        float th = max(p0.x, 1.0);
        float edge = 0.0;
        if (camFwd.w > 0.5) {
            float c0 = m;
            float n0 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( th,  0) * texel), 0);
            float n1 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2(-th,  0) * texel), 0);
            float n2 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( 0,  th) * texel), 0);
            float n3 = maskA.SampleLevel(linearClamp, saturate(i.uv + float2( 0, -th) * texel), 0);
            float grad = max(max(abs(n0 - c0), abs(n1 - c0)), max(abs(n2 - c0), abs(n3 - c0)));
            edge = smoothstep(p0.y * 0.5, max(p0.y, 0.05), grad);
        } else {
            float dc = CameraDistAtUv(i.uv);
            float d0 = CameraDistAtUv(saturate(i.uv + float2(th, 0) * texel));
            float d1 = CameraDistAtUv(saturate(i.uv + float2(0, th) * texel));
            float rel = (abs(d0 - dc) + abs(d1 - dc)) / max(dc, 20.0);
            edge = smoothstep(p0.y * 0.25, max(p0.y * 0.6, 0.05), rel);
        }
        float3 col = float3(p0.z, p0.w, p1.x);
        outRgb = lerp(src.rgb, col, saturate(edge * p1.y));
    } else if (type == 9) {
        // Distortion: mode=p0.x (0=ripple,1=heathaze,2=radial,3=chromatic), strength=p0.y, scale=p0.z, speed=p0.w, chroma=p1.x, mix=p1.y
        int dmode = int(p0.x + 0.5);
        float str = p0.y * 0.025 * m;
        float sc = max(p0.z, 1.0);
        float t = cameraTime.w * p0.w;
        float2 off = float2(0, 0);
        if (dmode == 0) {
            float2 c = i.uv - 0.5;
            float r = length(c);
            off = normalize(c + 1e-5) * sin(r * sc * 3.14 - t * 2.5) * str;
        } else if (dmode == 1) {
            off.x = sin(i.uv.y * sc * 2.2 + t * 2.0 + sin(i.uv.x * sc)) * str * 0.65;
            off.y = cos(i.uv.x * sc * 1.8 - t * 1.6) * str * 0.45;
        } else if (dmode == 2) {
            float2 c = i.uv - 0.5;
            off = c * dot(c, c) * str * 6.0;
        }
        float2 duv = saturate(i.uv + off);
        float ch = (dmode == 3 ? str * 1.4 : str * p1.x * 0.8);
        float2 dir = normalize((i.uv - 0.5) + 1e-5) * ch;
        float3 dcol;
        dcol.r = colorIn.SampleLevel(linearClamp, saturate(duv + dir), 0).r;
        dcol.g = colorIn.SampleLevel(linearClamp, duv, 0).g;
        dcol.b = colorIn.SampleLevel(linearClamp, saturate(duv - dir), 0).b;
        outRgb = lerp(src.rgb, dcol, saturate(m * p1.y));
    }
    return float4(outRgb, 1.0);
}

// ---- Final Output Mix Pass ----
float4 OutputPS(VsOut i) : SV_Target {
    float3 a = origColor.SampleLevel(linearClamp, i.uv, 0).rgb;
    float3 b = colorIn.SampleLevel(linearClamp, i.uv, 0).rgb;
    return float4(lerp(a, b, saturate(stepInfo.z)), 1.0);
}
)HLSL";

struct Constants {
	float invViewProj[16];
	float dims[4];
	float depthInfo[4];
	float cameraTime[4];
	float camFwd[4];
	float stepInfo[4];
	float p0[4];
	float p1[4];
	int pmCounts[4];
	float pmBounds[4];
	float pmCapsule[4];
	float pmBones[16][4];
	float pmOthers[16][4];
};
static_assert(sizeof(Constants) % 16 == 0, "cb alignment");

struct Blobs {
	ComPtr<ID3DBlob> vs, maskPS[2], effectPS[2], outputPS;
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const Blobs> g_blobs;
std::atomic<bool> g_compileStarted{ false };
std::thread g_compiler;

struct RtSurface {
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11RenderTargetView> rtv;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	void Reset() { rtv.Reset(); srv.Reset(); tex.Reset(); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }
	bool Ensure(ID3D11Device* dev, UINT width, UINT height, DXGI_FORMAT format) {
		if (tex && w == width && h == height && fmt == format) return true;
		Reset();
		RenderPrivateTextureScope priv;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))
			|| FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))
			|| FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
			Reset();
			return false;
		}
		w = width; h = height; fmt = format;
		return true;
	}
};

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> maskPS[2], effectPS[2], outputPS;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> dsOff;
	ComPtr<ID3D11BlendState> blendOpaque;

	RtSurface orig, ping[2], masks[kMaskSlots];
	ComPtr<ID3D11Texture2D> depthCopy;
	ComPtr<ID3D11ShaderResourceView> depthSrv;
	D3D11_TEXTURE2D_DESC depthDesc{};
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
	b->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->vs, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "MaskPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->maskPS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s1, "MaskPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->maskPS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "EffectPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->effectPS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s1, "EffectPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->effectPS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "EffectNodes", s0, "OutputPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->outputPS, &err));
	if (!b->ok)
		MvmDebugLog_LinefAlways("effectnodes", "shader compile failed: %s",
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
	const bool ok = SUCCEEDED(d->CreateVertexShader(b->vs->GetBufferPointer(), b->vs->GetBufferSize(), nullptr, &g_gpu.vs))
		&& SUCCEEDED(d->CreatePixelShader(b->maskPS[0]->GetBufferPointer(), b->maskPS[0]->GetBufferSize(), nullptr, &g_gpu.maskPS[0]))
		&& SUCCEEDED(d->CreatePixelShader(b->maskPS[1]->GetBufferPointer(), b->maskPS[1]->GetBufferSize(), nullptr, &g_gpu.maskPS[1]))
		&& SUCCEEDED(d->CreatePixelShader(b->effectPS[0]->GetBufferPointer(), b->effectPS[0]->GetBufferSize(), nullptr, &g_gpu.effectPS[0]))
		&& SUCCEEDED(d->CreatePixelShader(b->effectPS[1]->GetBufferPointer(), b->effectPS[1]->GetBufferSize(), nullptr, &g_gpu.effectPS[1]))
		&& SUCCEEDED(d->CreatePixelShader(b->outputPS->GetBufferPointer(), b->outputPS->GetBufferSize(), nullptr, &g_gpu.outputPS));
	if (!ok) { g_gpu.failed = true; return false; }

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
	ID3D11DepthStencilView* depth, ID3D11ShaderResourceView* worldDepthSrv,
	const float* worldToClip, const float* projection,
	const float camera[3], const float forward[3],
	float depthMin, float depthMax, double demoTime) {
	if (!Active() || !immediate || !target || !depth || !worldToClip || !g_gpu.ctx || !EnsureShaders())
		return false;

	const CompiledPlan plan = SnapshotPlan();
	if (!plan.enabled || plan.steps.empty()) return false;

	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return false;
	D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);
	const DXGI_FORMAT colorFmt = TypedColorFormat(rtvd.Format != DXGI_FORMAT_UNKNOWN ? rtvd.Format : sd.Format);
	const UINT W = sd.Width, H = sd.Height;

	if (!g_gpu.orig.Ensure(g_gpu.device.Get(), W, H, colorFmt)
		|| !g_gpu.ping[0].Ensure(g_gpu.device.Get(), W, H, colorFmt)
		|| !g_gpu.ping[1].Ensure(g_gpu.device.Get(), W, H, colorFmt))
		return false;
	for (int m = 0; m < kMaskSlots; ++m)
		if (!g_gpu.masks[m].Ensure(g_gpu.device.Get(), W, H, DXGI_FORMAT_R8_UNORM)) return false;

	auto* c = g_gpu.ctx.Get();
	bool isMsaa = false;
	UINT dw = W, dh = H;
	if (!EnsureDepthCopy(c, depth, isMsaa, dw, dh)) {
		ComPtr<ID3D11CommandList> drop; c->FinishCommandList(FALSE, &drop);
		return false;
	}

	if (sd.SampleDesc.Count > 1) {
		c->ResolveSubresource(g_gpu.orig.tex.Get(), 0, src.Get(), 0, colorFmt);
	} else {
		c->CopyResource(g_gpu.orig.tex.Get(), src.Get());
	}
	c->CopyResource(g_gpu.ping[0].tex.Get(), g_gpu.orig.tex.Get());

	Environment::Matrix vp{}, inv{};
	std::memcpy(vp.data(), worldToClip, sizeof(float) * 16);
	Environment::Inverse(vp, inv);
	const bool reversed = projection && projection[10] < 0.0f;
	const auto pm = RenderPlayerMatte_Snapshot();

	D3D11_VIEWPORT vpRect{}; vpRect.Width = (float)W; vpRect.Height = (float)H; vpRect.MaxDepth = 1.0f;
	c->RSSetViewports(1, &vpRect);
	c->RSSetState(g_gpu.raster.Get());
	c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
	const float bf[4] = {};
	c->OMSetBlendState(g_gpu.blendOpaque.Get(), bf, 0xffffffff);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
	ID3D11SamplerState* smps[1] = { g_gpu.sampler.Get() };
	c->PSSetSamplers(0, 1, smps);

	const int msIdx = isMsaa ? 1 : 0;
	int curPing = 0;

	Constants baseCb{};
	std::memcpy(baseCb.invViewProj, inv.data(), sizeof(float) * 16);
	baseCb.dims[0] = (float)W; baseCb.dims[1] = (float)H;
	baseCb.dims[2] = (float)dw; baseCb.dims[3] = (float)dh;
	baseCb.depthInfo[0] = depthMin; baseCb.depthInfo[1] = depthMax;
	baseCb.depthInfo[2] = reversed ? 1.0f : 0.0f;
	baseCb.depthInfo[3] = worldDepthSrv ? 1.0f : 0.0f;
	baseCb.cameraTime[0] = camera ? camera[0] : 0.0f;
	baseCb.cameraTime[1] = camera ? camera[1] : 0.0f;
	baseCb.cameraTime[2] = camera ? camera[2] : 0.0f;
	baseCb.cameraTime[3] = (float)demoTime;
	baseCb.camFwd[0] = forward ? forward[0] : 1.0f;
	baseCb.camFwd[1] = forward ? forward[1] : 0.0f;
	baseCb.camFwd[2] = forward ? forward[2] : 0.0f;
	baseCb.pmCounts[0] = pm.hasTarget ? 1 : 0;
	baseCb.pmCounts[1] = pm.targetBoneCount;
	baseCb.pmCounts[2] = pm.otherCount;
	for (int k = 0; k < 4; ++k) {
		baseCb.pmBounds[k] = pm.targetBoundsCenter[k];
		baseCb.pmCapsule[k] = pm.targetCapsule[k];
	}
	for (int i = 0; i < 16; ++i) {
		for (int k = 0; k < 4; ++k) {
			baseCb.pmBones[i][k] = pm.targetBones[i][k];
			baseCb.pmOthers[i][k] = pm.otherCapsules[i][k];
		}
	}

	ID3D11ShaderResourceView* nullSrvs[6] = {};
	ID3D11RenderTargetView* nullRt[1] = {};

	for (const CompiledStep& st : plan.steps) {
		Constants cb = baseCb;
		cb.stepInfo[0] = (float)(int)st.type;
		cb.camFwd[3] = st.maskSlotA >= 0 ? 1.0f : 0.0f;
		cb.stepInfo[1] = st.maskSlotB >= 0 ? 1.0f : 0.0f;
		cb.stepInfo[2] = plan.masterMix;
		for (int k = 0; k < 4; ++k) {
			cb.p0[k] = st.p[k];
			cb.p1[k] = st.p[k + 4];
		}
		c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &cb, 0, 0);
		ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
		c->PSSetConstantBuffers(0, 1, cbs);

		ID3D11ShaderResourceView* srvs[6] = {
			g_gpu.ping[curPing].srv.Get(),
			g_gpu.depthSrv.Get(),
			worldDepthSrv,
			st.maskSlotA >= 0 ? g_gpu.masks[st.maskSlotA].srv.Get() : nullptr,
			st.maskSlotB >= 0 ? g_gpu.masks[st.maskSlotB].srv.Get() : nullptr,
			g_gpu.orig.srv.Get()
		};

		if (IsMaskNode(st.type) && st.outMaskSlot >= 0 && st.outMaskSlot < kMaskSlots) {
			ID3D11RenderTargetView* rts[1] = { g_gpu.masks[st.outMaskSlot].rtv.Get() };
			c->OMSetRenderTargets(1, rts, nullptr);
			c->PSSetShader(g_gpu.maskPS[msIdx].Get(), nullptr, 0);
			c->PSSetShaderResources(0, 6, srvs);
			c->Draw(3, 0);
			c->PSSetShaderResources(0, 6, nullSrvs);
			c->OMSetRenderTargets(1, nullRt, nullptr);
		} else if (IsEffectNode(st.type)) {
			const int nextPing = 1 - curPing;
			ID3D11RenderTargetView* rts[1] = { g_gpu.ping[nextPing].rtv.Get() };
			c->OMSetRenderTargets(1, rts, nullptr);
			c->PSSetShader(g_gpu.effectPS[msIdx].Get(), nullptr, 0);
			c->PSSetShaderResources(0, 6, srvs);
			c->Draw(3, 0);
			c->PSSetShaderResources(0, 6, nullSrvs);
			c->OMSetRenderTargets(1, nullRt, nullptr);
			curPing = nextPing;
		}
	}

	// Final composite into the scene render target with masterMix.
	Constants finalCb = baseCb;
	finalCb.stepInfo[2] = plan.masterMix;
	c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &finalCb, 0, 0);
	ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
	c->PSSetConstantBuffers(0, 1, cbs);
	ID3D11RenderTargetView* outRt[1] = { target };
	c->OMSetRenderTargets(1, outRt, nullptr);
	c->PSSetShader(g_gpu.outputPS.Get(), nullptr, 0);
	ID3D11ShaderResourceView* outSrvs[6] = {
		g_gpu.ping[curPing].srv.Get(), nullptr, nullptr, nullptr, nullptr, g_gpu.orig.srv.Get()
	};
	c->PSSetShaderResources(0, 6, outSrvs);
	c->Draw(3, 0);
	c->PSSetShaderResources(0, 6, nullSrvs);
	c->OMSetRenderTargets(1, nullRt, nullptr);

	ComPtr<ID3D11CommandList> cl;
	if (FAILED(c->FinishCommandList(FALSE, &cl)) || !cl) return false;
	{ GPU_PASS_SCOPE(immediate, "effect nodes"); immediate->ExecuteCommandList(cl.Get(), TRUE); }
	return true;
}

} // namespace Filmmaker::EffectNodes
