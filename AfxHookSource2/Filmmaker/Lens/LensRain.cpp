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
	F4 info;  // x aspect (w/h), y frame height, z mip levels of the scene copy
	F4 look;  // x refraction, y blur (0..1), z water-only preview
	struct Water {
		F4 drop;  // normalized centre xy, radius, seed
		F4 shape; // runoff xy (frame heights), kind, opacity
	};
	Water drops[kMaxDrops];
};
static_assert(kMaxDrops == 64, "match RainConstants in HLSL");
static_assert(sizeof(Constants) % 16 == 0, "D3D constant buffers are 16-byte aligned");
static_assert(sizeof(Constants) <= 65536, "a constant buffer holds at most 64 KB");

const char kShader[] = R"HLSL(
struct Water {
    float4 drop;
    float4 shape;
};

cbuffer RainConstants : register(b0) {
    float4 info; // aspect, frame height, mip count
    float4 look; // refraction, softness, water-only preview
    Water drops[64];
};
SamplerState linearClamp : register(s0);
Texture2D<float4> scene : register(t0);

struct VsOut {
    float4 pos : SV_Position;
    float2 local : LOCAL;
    nointerpolation float4 shape : SHAPE; // radius, runoff in radii, seed, kind
    nointerpolation float4 motion : MOTION; // direction xy, coverage, optical radius
};

VsOut VS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1),
                                      float2(-1,1), float2(1,-1), float2(1,1) };
    const float4 drop = drops[iid].drop;
    const float4 water = drops[iid].shape;
    const float run = length(water.xy);
    const float2 dir = run > 1e-5 ? water.xy / run : float2(0, 1);
    const bool preview = look.z > 0.5;
    const float radius = max((preview ? 1.6 : 0.9) / info.y, drop.z);
    const float runRadii = run / radius;
    const float coverage = saturate(drop.z / radius);
    const float2 corner = corners[vid];
    const float2 local = float2(corner.x * 2.05, corner.y > 0 ? 2.05 : -2.05 - runRadii);
    const float2 offset = (dir * local.y + float2(-dir.y, dir.x) * local.x) * radius;
    const float2 screen = drop.xy + offset / float2(info.x, 1);
    VsOut o;
    o.pos = float4(screen.x * 2 - 1, 1 - screen.y * 2, 0, 1);
    o.local = local;
    o.shape = float4(radius, runRadii, drop.w, water.z);
    const float opacity = water.w * (preview ? sqrt(coverage) : coverage * coverage);
    o.motion = float4(dir, opacity, drop.z);
    return o;
}

float BlurMip(float footprint) {
    return clamp(log2(max(footprint * info.y, 1.0)), 0.0, info.z - 1.0);
}

float4 PS(VsOut i) : SV_Target {
    const float r = i.shape.x, run = i.shape.y, seed = i.shape.z;
    const bool strip = i.shape.w == 1.0;
    const bool splash = i.shape.w == 2.0;
    const float2 viewSize = info.y * float2(info.x, 1);
    const float2 uv = i.pos.xy / viewSize;
    const float2 dir = i.motion.xy;
    const float2 side = float2(-dir.y, dir.x);
    const float blur = look.y;
    float2 q = i.local;
    if (strip) {
        // The entire strip has a shallow, rounded profile. Its ends share the
        // same width and refraction as the body, so it never grows a separate head.
        const float along = clamp(q.y, -run, 0.0);
        const float t = -along / max(run, 0.01);
        q.x -= sin(t * 3.14159) * (seed - 0.5) * 0.78;
        q.y -= along;
    } else if (splash) {
        q.x += 0.17 * sin(q.y * 2.4 + seed * 6.2831);
        q *= float2(1.12, 0.86);
    } else {
        q *= float2(1.0 + seed * 0.15, 1.05 - seed * 0.17);
    }

    const float d = length(q);
    const float pixel = min(0.8, 0.72 / (r * info.y));
    const float inner = splash ? lerp(0.76, 0.36, blur) : 1.0 - max(pixel, blur * 0.27);
    const float alpha = 1.0 - smoothstep(inner, 1.0 + pixel, d);
    if (alpha <= 0.001) discard;

    const float2 normal = side * q.x + dir * q.y;
    const bool preview = look.z > 0.5;
    const float opticalRadius = preview ? i.motion.w : r;
    const float displacement = splash ? 0.16 : 0.87;
    const float2 sampleUv = uv - normal * (opticalRadius * look.x * displacement) / float2(info.x, 1);
    const float mip = BlurMip(opticalRadius * blur * (splash ? 0.93 : 0.70));
    float3 color = scene.SampleLevel(linearClamp, sampleUv, mip).rgb;
    if (!splash) {
        // Keep the rim subdued and tied to scene color so subpixel drops don't flash.
        const float rim = smoothstep(0.38, 1.0, d);
        color *= 1.0 + 0.17 * rim * clamp(dot(normal, float2(-0.6, -0.8)), -1.0, 1.0);
    }
    const float opacity = preview ? 1.0 : (splash ? 0.83 : 0.95);
    return float4(color, alpha * i.motion.z * opacity);
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
				if (std::hypot(d.trailX, d.trailY) * trails >= 0.009f) ++strips;
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
	float size, trails, refraction, blur;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
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

	// Draw diffuse water first so beads and streaks can sit over it.
	std::stable_partition(drops.begin(), drops.end(), [](const LensRain::Drop& d) { return d.kind == LensRain::Kind::Splash; });
	Constants k{};
	const int n = (int)(std::min)(drops.size(), (size_t)kMaxDrops);
	k.info = { aspect, (float)td.Height, (float)g.levels, 0 };
	k.look = { refraction, blur, preview ? 1.0f : 0.0f, 0 };
	for (int i = 0; i < n; ++i) {
		const LensRain::Drop& d = drops[i];
		k.drops[i] = {
			{ d.x, d.y, LensRain::VisualRadius(d, size), d.seed },
			{ d.trailX * trails, d.trailY * trails, static_cast<float>(d.kind), LensRain::Opacity(d) },
		};
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
	c->DrawInstanced(6, (UINT)n, 0, 0);
	ComPtr<ID3D11CommandList> list;
	if (FAILED(c->FinishCommandList(FALSE, &list))) return false;
	{ GPU_PASS_SCOPE(immediate, "lens: rain"); immediate->ExecuteCommandList(list.Get(), TRUE); }

	if (MvmDebugLog_Active() && (g.frames++ % 120) == 0)
		MvmDebugLog_LinefAlways("lens.rain", "drew %d drop(s) on %ux%u samples=%u fmt=%d mips=%u refraction=%.2f blur=%.2f",
			n, td.Width, td.Height, td.SampleDesc.Count, (int)typed, g.levels, refraction, blur);
	return true;
}

} // namespace Filmmaker
