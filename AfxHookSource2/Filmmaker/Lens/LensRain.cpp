#define NOMINMAX
#include "LensRain.h"
#include "LensRainMath.h"
#include "LensFx.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Movie/CameraBridge.h"
#include "../../MirvTime.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <future>
#include <mutex>
#include <vector>

namespace Filmmaker {

using Microsoft::WRL::ComPtr;

namespace {

// ================================================================================================
// Main thread: the drops
// ================================================================================================

std::mutex g_mutex;
std::vector<LensRain::Drop> g_shared; // the latest drops, for the render thread
float g_sharedAspect = 16.0f / 9.0f;
float g_sharedSize = 0.7f, g_sharedTrails = 0.3f, g_sharedRefraction = 0.75f, g_sharedBlur = 0.48f;
float g_sharedVisibility = 1.0f;

LensRain::Sim g_sim;
bool g_havePose = false;
double g_prevOrigin[3] = {}, g_prevAngles[3] = {};
double g_prevTime = 0;
bool g_prevWasDemo = false;
bool g_clearRequested = false;
bool g_wasOn = false;

double WallSeconds() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================================================
// Render thread
// ================================================================================================

constexpr int kMaxDrops = LensRain::kMaxDrops;

struct F4 { float x = 0, y = 0, z = 0, w = 0; };
// Must match RainConstants below.
struct Constants {
	F4 info; // aspect, frame height, mip count
	F4 look; // refraction, softness, water-only preview, visibility
	struct Water {
		F4 drop;   // center in frame heights, radius, seed
		F4 bounds; // left, top, right, bottom in frame heights
		F4 style;  // kind, opacity, first knot, knot count
	};
	Water water[kMaxDrops * 2];
	F4 points[kMaxDrops * LensRain::kRunoffPoints]; // x, y, coverage, distance
};
static_assert(kMaxDrops * 2 == 128 && kMaxDrops * LensRain::kRunoffPoints == 1600, "match RainConstants in HLSL");
static_assert(sizeof(Constants) % 16 == 0 && sizeof(Constants) <= 65536, "D3D constant buffer size");

const char kShader[] = R"HLSL(
struct Water {
    float4 drop;
    float4 bounds;
    float4 style;
};

cbuffer RainConstants : register(b0) {
    float4 info; // aspect, frame height, mip count
    float4 look; // refraction, softness, water-only preview, visibility
    Water water[128];
    float4 points[1600];
};
SamplerState linearClamp : register(s0);
Texture2D<float4> scene : register(t0);

struct VsOut {
    float4 pos : SV_Position;
    nointerpolation float4 drop : DROP;
    nointerpolation float4 style : STYLE;
};

VsOut VS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1),
                                      float2(0,1), float2(1,0), float2(1,1) };
    const Water w = water[iid];
    const float2 screen = lerp(w.bounds.xy, w.bounds.zw, corners[vid]) / float2(info.x, 1);
    VsOut o;
    o.pos = float4(screen.x * 2 - 1, 1 - screen.y * 2, 0, 1);
    o.drop = w.drop;
    o.style = w.style;
    return o;
}

float BlurMip(float footprint) {
    return clamp(log2(max(footprint * info.y, 1.0)), 0.0, info.z - 1.0);
}

// Sample the curved centerline once per pixel so joined segments cannot double-blend.
float2 TrailProfile(float2 position, VsOut i, float radius, out float wet) {
    float best = 1e10;
    float2 offset = float2(1e4, 1e4);
    float2 side = float2(1, 0);
    float distance = 0;
    wet = 0;
    const uint first = (uint)i.style.z, count = (uint)i.style.w;
    float4 a = points[first];
    [loop] for (uint j = 1; j < count; ++j) {
        const float4 b = points[first + j];
        const float2 v = b.xy - a.xy;
        const float vv = dot(v, v);
        if (vv > 1e-10) {
            const float t = saturate(dot(position - a.xy, v) / vv);
            const float2 delta = position - lerp(a.xy, b.xy, t);
            const float d2 = dot(delta, delta);
            if (d2 < best) {
                best = d2;
                offset = delta;
                side = float2(-v.y, v.x) * rsqrt(vv);
                wet = lerp(a.z, b.z, t);
                distance = lerp(a.w, b.w, t);
            }
        }
        a = b;
    }
    // Vary the edge of the deposited water in place, using distance from its first landing.
    const float phase = i.drop.w * 6.2831;
    const float ripple = sin(distance * 153.0 + phase);
    const float fine = sin(distance * 398.0 + phase * 1.7);
    offset -= side * radius * (0.18 * ripple + 0.09 * fine);
    const float width = 0.78 + 0.17 * ripple + 0.13 * fine;
    wet *= 0.93 + 0.07 * fine;
    return offset / (radius * width);
}

float4 PS(VsOut i) : SV_Target {
    const bool splash = i.style.x == 2.0;
    const bool trail = i.style.x == 3.0;
    const bool preview = look.z > 0.5;
    const float blur = look.y;
    const float2 position = i.pos.xy / info.y;
    const float2 uv = position / float2(info.x, 1);
    const float opticalRadius = i.drop.z;
    const float radius = trail ? max((preview ? 1.8 : 1.15) / info.y, opticalRadius * 1.18)
                               : max((preview ? 2.4 : 1.15) / info.y, opticalRadius);
    float wet = 1.0;
    float2 q = (position - i.drop.xy) / radius;
    if (trail) {
        q = TrailProfile(position, i, radius, wet);
    } else if (splash) {
        q.x += 0.17 * sin(q.y * 2.4 + i.drop.w * 6.2831);
        q *= float2(1.12, 0.86);
    } else {
        q *= float2(1.0 + i.drop.w * 0.15, 1.05 - i.drop.w * 0.17);
    }

    const float d = length(q);
    const float pixel = min(0.8, 0.72 / (radius * info.y));
    const float inner = splash ? lerp(0.76, 0.36, blur) : 1.0 - max(pixel, blur * 0.27);
    const float alpha = wet * (1.0 - smoothstep(inner, 1.0 + pixel, d));
    if (alpha <= 0.001) discard;

    // The shape's slope bends the image. Rim color is a separate cue for the glass surface.
    const float displacement = trail ? 2.8 : (splash ? 0.65 : 3.05);
    const float2 slope = q * (1.0 - 0.24 * saturate(d * d));
    const float sampleRadius = trail || preview ? opticalRadius : radius;
    const float2 sampleUv = uv - slope * (sampleRadius * look.x * displacement) / float2(info.x, 1);
    const float mip = BlurMip(sampleRadius * blur * (trail ? 1.5 : (splash ? 0.93 : 0.70)));
    float3 color = scene.SampleLevel(linearClamp, sampleUv, mip).rgb;
    if (look.w > 0.0) {
        const float rim = smoothstep(0.30, 0.80, d) * (1.0 - smoothstep(0.98, 1.27, d));
        const float facing = dot(q, float2(-0.6, -0.8));
        const float3 coolShadow = float3(0.24, 0.17, 0.39);
        const float3 paleViolet = float3(0.76, 0.69, 0.97);
        const float3 tint = lerp(coolShadow, paleViolet, smoothstep(-0.53, 0.64, facing));
        const float edge = rim * look.w * lerp(0.27, 0.58, saturate(abs(facing)));
        color = lerp(color, tint, saturate(edge * (splash ? 0.32 : 1.0)));
    }
    const float coverage = saturate(opticalRadius / radius);
    const float resolutionFade = trail ? 1.0 : (preview ? lerp(0.72, 1.0, coverage) : coverage);
    const float opacity = saturate(alpha * i.style.y * resolutionFade);
    return float4(color, opacity);
}
)HLSL";

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx; // deferred
	bool ready = false;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> linear;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> noDepth;
	ComPtr<ID3D11BlendState> blend;
	// A mipmapped copy of the frame the drops look through.
	ComPtr<ID3D11Texture2D> copy;
	ComPtr<ID3D11ShaderResourceView> copySrv;
	UINT cw = 0, ch = 0, levels = 0;
	DXGI_FORMAT cFormat = DXGI_FORMAT_UNKNOWN, cTypeless = DXGI_FORMAT_UNKNOWN;
	unsigned frames = 0;
} g;

// Shader code is not tied to a device: compiled once in the background and kept.
struct Compiled { ComPtr<ID3DBlob> vs, ps; };
std::future<Compiled> g_compile;
Compiled g_code;
bool g_haveCode = false;
bool g_compileFailed = false;

ComPtr<ID3DBlob> CompileStage(const char* entry, const char* profile) {
	ComPtr<ID3DBlob> code, errors;
	bool cached = false;
	const HRESULT hr = Environment::CompileEnvironmentShader(kShader, sizeof(kShader) - 1, "LensRain", nullptr, entry, profile,
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &errors, &cached);
	if (FAILED(hr)) {
		MvmDebugLog_LinefAlways("lens.rain", "shader %s FAILED hr=%08x %s", entry, (unsigned)hr,
			errors ? (const char*)errors->GetBufferPointer() : "");
		return nullptr;
	}
	return code;
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

bool CreateObjects(const Compiled& c) {
	if (!c.vs || !c.ps) return false;
	if (FAILED(g.device->CreateVertexShader(c.vs->GetBufferPointer(), c.vs->GetBufferSize(), nullptr, &g.vs))) return false;
	if (FAILED(g.device->CreatePixelShader(c.ps->GetBufferPointer(), c.ps->GetBufferSize(), nullptr, &g.ps))) return false;
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
	auto& rt = bl.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
	rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
	rt.DestBlendAlpha = D3D11_BLEND_ONE;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	if (FAILED(g.device->CreateBlendState(&bl, &g.blend))) return false;
	if (FAILED(g.device->CreateDeferredContext(0, &g.ctx))) return false;
	return true;
}

bool PumpDevice() {
	if (g.ready) return true;
	if (!g.device || g_compileFailed) return false;
	if (!g_haveCode) {
		if (!g_compile.valid())
			g_compile = std::async(std::launch::async, [] { Compiled c; c.vs = CompileStage("VS", "vs_5_0"); c.ps = CompileStage("PS", "ps_5_0"); return c; });
		if (g_compile.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
		g_code = g_compile.get();
		g_haveCode = true;
		if (!g_code.vs || !g_code.ps) { g_compileFailed = true; return false; }
	}
	if (!CreateObjects(g_code)) {
		g_compileFailed = true;
		MvmDebugLog_LinefAlways("lens.rain", "could not create the render objects");
		return false;
	}
	g.ready = true;
	MvmDebugLog_LinefAlways("lens.rain", "device ready");
	return true;
}

// The scene copy: the target's size and format, with a full mip chain for the out-of-focus look.
bool EnsureCopy(const D3D11_TEXTURE2D_DESC& td, DXGI_FORMAT typed) {
	if (g.copy && g.cw == td.Width && g.ch == td.Height && g.cFormat == typed && g.cTypeless == td.Format) return true;
	g.copy.Reset(); g.copySrv.Reset();
	UINT levels = 1;
	for (UINT s = (std::max)(td.Width, td.Height); s > 1; s >>= 1) ++levels;
	levels = (std::min)(levels, 9u);
	D3D11_TEXTURE2D_DESC d{};
	d.Width = td.Width; d.Height = td.Height; d.MipLevels = levels; d.ArraySize = 1;
	d.Format = td.Format; // copies need the same format family; views name the typed one
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.copy))) return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = typed;
	sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sv.Texture2D.MipLevels = levels;
	if (FAILED(g.device->CreateShaderResourceView(g.copy.Get(), &sv, &g.copySrv))) { g.copy.Reset(); return false; }
	g.cw = td.Width; g.ch = td.Height; g.levels = levels; g.cFormat = typed; g.cTypeless = td.Format;
	return true;
}

} // namespace

// ================================================================================================
// Main thread
// ================================================================================================

void LensRain_Clear() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_clearRequested = true;
}

void LensRain_RunMainThreadFrame(float rainIntensity) {
	const Lens::Values L = LensFx_GetKeyed();
	const bool enabled = L[Lens::RainOn] > 0.5f;
	const bool on = enabled && rainIntensity > 0;
	const bool transition = g_wasOn != on;
	g_wasOn = on;
	bool clear;
	float aspect;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		clear = g_clearRequested;
		g_clearRequested = false;
		aspect = g_sharedAspect;
		if (!on) g_shared.clear();
	}
	if (!on || clear) {
		if (transition || clear) g_sim.Reset();
		g_havePose = false;
		if (!on) {
			if (transition && MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("lens.rain", "inactive: toggle=%d intensity=%.3f", enabled, rainIntensity);
			return;
		}
	}

	double origin[3] = {}, angles[3] = {}, fov = 90;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	double demoTime = 0;
	const bool demo = g_MirvTime.GetCurrentDemoTime(demoTime);
	const double now = demo ? demoTime : WallSeconds();
	const double dt = g_havePose && demo == g_prevWasDemo ? now - g_prevTime : 0;
	const bool seek = (g_havePose && demo != g_prevWasDemo) || dt < 0 || dt > 0.25;
	const auto still = LensRain::MotionFrom(origin, angles, origin, angles, 0);
	const auto motion = g_havePose && !seek
		? LensRain::MotionFrom(g_prevOrigin, g_prevAngles, origin, angles, dt)
		: still;

	LensRain::Params p;
	p.intensity = rainIntensity;
	p.amount = L[Lens::RainAmount];
	p.size = L[Lens::RainSize];
	p.slide = L[Lens::RainSlide];
	p.impact = L[Lens::RainImpact];
	const float trails = L[Lens::RainTrails];
	// Start from a deterministic lens after a seek, including the seek to a render's
	// first frame. The same wet look is available while the demo is paused.
	if (seek) g_sim.Reset();
	if ((transition || seek) && !clear) {
		for (int i = 0; i < 120; ++i) g_sim.Step(1.0f / 60.0f, aspect, still, p);
	}
	if (g_havePose && !seek) g_sim.Step((float)dt, aspect, motion, p);

	for (int i = 0; i < 3; ++i) { g_prevOrigin[i] = origin[i]; g_prevAngles[i] = angles[i]; }
	g_prevTime = now;
	g_prevWasDemo = demo;
	g_havePose = true;

	if (MvmDebugLog_Active()) {
		static unsigned s_log = 0;
		if (transition || seek || (++s_log % 60) == 0) {
			unsigned strips = 0;
			float maxSpeed = 0;
			for (const auto& d : g_sim.Drops()) {
				if (LensRain::VisibleRunoff(d, trails).count > 1) ++strips;
				maxSpeed = (std::max)(maxSpeed, std::hypot(d.vx, d.vy));
			}
			MvmDebugLog_LinefAlways("lens.rain", "intensity=%.3f drops=%zu strips=%u fastest=%.3f dt=%.4f seek=%d amount=%.0f size=%.2f slide=%.2f",
				rainIntensity, g_sim.Drops().size(), strips, maxSpeed, dt, seek, p.amount, p.size, p.slide);
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	g_shared = g_sim.Drops();
	g_sharedSize = p.size;
	g_sharedTrails = trails;
	g_sharedRefraction = L[Lens::RainRefraction];
	g_sharedBlur = L[Lens::RainBlur];
	g_sharedVisibility = L[Lens::RainVisibility];
}

bool LensRain_Active() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return !g_shared.empty();
}

// ================================================================================================
// Render thread
// ================================================================================================

void LensRain_BeginDevice(ID3D11Device* device) {
	if (device && g.device.Get() == device) return;
	LensRain_EndDevice();
	if (!device) return;
	g.device = device;
}

void LensRain_EndDevice() {
	g = Gpu{}; // the compiled code (g_code, g_compile) outlives the device
}

bool LensRain_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, bool preview) {
	std::vector<LensRain::Drop> drops;
	float size, trails, refraction, blur, visibility;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		visibility = g_sharedVisibility;
		if (g_shared.empty() && !preview) return false;
		drops = g_shared;
		size = g_sharedSize;
		trails = g_sharedTrails;
		refraction = g_sharedRefraction;
		blur = g_sharedBlur;
	}
	if (!immediate || !target) return false;
	constexpr float black[4] = { 0, 0, 0, 1 };
	if (preview && drops.empty()) {
		immediate->ClearRenderTargetView(target, black);
		return true;
	}
	if (!PumpDevice()) return false;

	ComPtr<ID3D11Resource> res;
	target->GetResource(&res);
	ComPtr<ID3D11Texture2D> tex;
	if (!res || FAILED(res.As(&tex))) return false;
	D3D11_TEXTURE2D_DESC td{};
	tex->GetDesc(&td);
	if (td.ArraySize != 1 || td.Width < 16 || td.Height < 16) return false;
	D3D11_RENDER_TARGET_VIEW_DESC rv{};
	target->GetDesc(&rv);
	const DXGI_FORMAT typed = rv.Format != DXGI_FORMAT_UNKNOWN ? rv.Format : TypedFrom(td.Format);
	if (!EnsureCopy(td, typed)) return false;
	const float aspect = (float)td.Width / (float)td.Height;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_sharedAspect = aspect;
	}

	Constants k{};
	k.info = { aspect, (float)td.Height, (float)g.levels, 0 };
	k.look = { refraction, blur, preview ? 1.0f : 0.0f, visibility };
	UINT n = 0, nextPoint = 0;
	const float pixelHeight = 1.0f / (float)td.Height;
	auto addDrop = [&](const LensRain::Drop& d) {
		const float opacity = LensRain::Opacity(d);
		if (opacity <= 0.001f) return;
		const bool splash = d.kind == LensRain::Kind::Splash;
		const float radius = LensRain::VisualRadius(d, size);
		const float pad = 2.05f * (std::max)(radius, (preview ? 2.4f : 1.15f) * pixelHeight);
		const float x = d.x * aspect, y = d.y;
		k.water[n++] = {{x, y, radius, d.seed}, {x - pad, y - pad, x + pad, y + pad},
			{splash ? 2.f : 0.f, opacity * (preview ? 1.0f : (splash ? 0.83f : 0.95f)), 0, 0}};
	};
	for (const auto& d : drops) {
		if (d.kind == LensRain::Kind::Splash) addDrop(d);
	}
	// Draw the deposited water below the living drop heads, including paths whose heads have drained.
	for (const auto& d : drops) {
		const auto wet = LensRain::VisibleRunoff(d, trails);
		if (wet.count < 2) continue;
		const float radius = LensRain::VisualRadius(d, size);
		const float pad = 2.05f * (std::max)(1.18f * radius, (preview ? 1.8f : 1.15f) * pixelHeight);
		const UINT first = nextPoint;
		F4 bounds{wet.points[0].x * aspect, wet.points[0].y, wet.points[0].x * aspect, wet.points[0].y};
		for (int i = 0; i < wet.count; ++i) {
			const auto& p = wet.points[i];
			const float x = p.x * aspect;
			bounds.x = (std::min)(bounds.x, x); bounds.y = (std::min)(bounds.y, p.y);
			bounds.z = (std::max)(bounds.z, x); bounds.w = (std::max)(bounds.w, p.y);
			k.points[nextPoint++] = {x, p.y, p.coverage, p.distance};
		}
		bounds.x -= pad; bounds.y -= pad; bounds.z += pad; bounds.w += pad;
		k.water[n++] = {{d.x * aspect, d.y, radius, d.seed}, bounds, {3.f, 1.f, (float)first, (float)wet.count}};
	}
	for (const auto& d : drops) {
		if (d.kind != LensRain::Kind::Splash) addDrop(d);
	}
	if (!n) {
		if (preview) immediate->ClearRenderTargetView(target, black);
		return preview;
	}

	ID3D11DeviceContext* c = g.ctx.Get();
	c->ClearState();
	// Save the live scene before clearing the backdrop so the preview still refracts it.
	if (td.SampleDesc.Count > 1) c->ResolveSubresource(g.copy.Get(), 0, tex.Get(), 0, typed);
	else c->CopySubresourceRegion(g.copy.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
	c->GenerateMips(g.copySrv.Get());
	if (preview) c->ClearRenderTargetView(target, black);
	c->UpdateSubresource(g.cb.Get(), 0, nullptr, &k, 0, 0);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g.vs.Get(), nullptr, 0);
	c->PSSetShader(g.ps.Get(), nullptr, 0);
	ID3D11Buffer* cbs[] = { g.cb.Get() };
	c->VSSetConstantBuffers(0, 1, cbs);
	c->PSSetConstantBuffers(0, 1, cbs);
	ID3D11SamplerState* samplers[] = { g.linear.Get() };
	c->PSSetSamplers(0, 1, samplers);
	ID3D11ShaderResourceView* srvs[] = { g.copySrv.Get() };
	c->PSSetShaderResources(0, 1, srvs);
	c->RSSetState(g.raster.Get());
	const D3D11_VIEWPORT vp{ 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
	c->RSSetViewports(1, &vp);
	c->OMSetDepthStencilState(g.noDepth.Get(), 0);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	c->OMSetBlendState(g.blend.Get(), blendFactor, 0xffffffff);
	c->OMSetRenderTargets(1, &target, nullptr);
	c->DrawInstanced(6, n, 0, 0);
	ComPtr<ID3D11CommandList> list;
	if (FAILED(c->FinishCommandList(FALSE, &list))) return false;
	{ GPU_PASS_SCOPE(immediate, "lens: rain"); immediate->ExecuteCommandList(list.Get(), TRUE); }

	if (MvmDebugLog_Active() && (g.frames++ % 120) == 0)
		MvmDebugLog_LinefAlways("lens.rain", "drew %u lens water shapes on %ux%u samples=%u fmt=%d mips=%u refraction=%.2f blur=%.2f",
			n, td.Width, td.Height, td.SampleDesc.Count, (int)typed, g.levels, refraction, blur);
	return true;
}

} // namespace Filmmaker
