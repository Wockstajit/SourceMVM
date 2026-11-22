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
float g_sharedRefraction = 1, g_sharedBlur = 0.5f;
bool g_sharedOn = false;

LensRain::Sim g_sim;
bool g_havePose = false;
double g_prevOrigin[3] = {}, g_prevAngles[3] = {};
double g_prevTime = 0;
bool g_prevWasDemo = false;
std::chrono::steady_clock::time_point g_prevWall;
bool g_clearRequested = false;
bool g_wasOn = false; // switching on fills the lens with a few seconds of rain at once

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
	F4 look;  // x refraction, y blur (0..1), zw unused
	struct Water {
		F4 drop;   // centre xy (frame heights), radius, seed
		F4 motion; // velocity xy, kind, opacity
		F4 trail;  // recent displacement xy (frame heights)
	};
	Water drops[kMaxDrops];
};
static_assert(sizeof(Constants) % 16 == 0, "D3D constant buffers are 16-byte aligned");
static_assert(sizeof(Constants) <= 65536, "a constant buffer holds at most 64 KB");

const char kShader[] = R"HLSL(
struct Water {
    float4 drop;
    float4 motion;
    float4 trail;
};

cbuffer RainConstants : register(b0) {
    float4 info;   // x aspect (w/h), y frame height, z mip levels of the scene copy
    float4 look;   // x refraction, y blur (0..1)
    Water drops[384];
};
SamplerState linearClamp : register(s0);
Texture2D<float4> scene : register(t0);

struct VsOut {
    float4 pos : SV_Position;
    float2 local : LOCAL; // distance from the center, in radii
    nointerpolation float4 drop : DROP;   // centre uv, radius (heights), seed
    nointerpolation float4 motion : MOTION; // unit direction of motion xy, stretch, opacity
    nointerpolation float2 shape : SHAPE; // kind and trail length in radii
};

VsOut VS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };
    const float4 a = drops[iid].drop;
    const float4 b = drops[iid].motion;
    const float2 trail = drops[iid].trail.xy;
    const float run = length(trail);
    const float kind = b.z; // 0 bead, 1 rivulet, 2 splash
    const float aspect = info.x;
    const float speed = length(b.xy);
    const float2 dir = run > 0.004 ? trail / run : speed > 1e-5 ? b.xy / speed : float2(0, 1);
    const float stretch = kind == 2.0 ? 1.0 : 1.0 + 0.46 * saturate(speed / max(a.z * 20.0, 1e-3));
    const float trailRadii = run / a.z;
    const float2 c = corners[vid];
    const float2 side = float2(-dir.y, dir.x);
    const float along = c.y > 0 ? 1.55 : -max(stretch * 1.55, trailRadii);
    const float across = c.x * 1.55;
    const float2 offset = (dir * along + side * across) * a.z;
    const float2 screen = a.xy + offset;
    VsOut o;
    o.pos = float4(screen.x / aspect * 2.0 - 1.0, 1.0 - screen.y * 2.0, 0, 1);
    o.local = float2(across, along);
    o.drop = float4(a.x / aspect, a.y, a.z, a.w);
    o.motion = float4(dir, stretch, b.w);
    o.shape = float2(kind, trailRadii);
    return o;
}

// The drop's outline as a distance: 1 on the edge, 0 at the middle. Water on glass is never a
// circle: the edge is lumpy (three harmonics with per-drop amplitudes and phases), and about one
// drop in five is two drops run together. `seed` is a per-drop constant (0..1).
float DropShape(float2 q, float seed) {
    const float a = atan2(q.y, q.x);
    const float s1 = frac(seed * 13.17), s2 = frac(seed * 7.73), s3 = frac(seed * 3.31), s4 = frac(seed * 29.9);
    const float lump = 1.0 + 0.10 * s1 * sin(2.0 * a + s2 * 6.2831) + 0.07 * s2 * sin(3.0 * a + s3 * 6.2831)
                     + 0.04 * s3 * sin(5.0 * a + s4 * 6.2831);
    const bool pair = seed > 0.8;
    const float2 c0 = pair ? float2(-0.18 * (s4 * 2.0 - 1.0), -0.12) : float2(0, 0);
    float d = length(q - c0) / ((pair ? 0.72 : 1.0) * lump);
    if (pair) {
        // A smaller drop that has run into this one: a smooth union of the two outlines.
        const float2 c1 = float2(0.42 * (s4 * 2.0 - 1.0), 0.38);
        const float d1 = length(q - c1) / (0.48 + 0.12 * s1);
        const float k = 0.22;
        const float hmix = saturate(0.5 + 0.5 * (d1 - d) / k);
        d = lerp(d1, d, hmix) - k * hmix * (1.0 - hmix);
    }
    return d;
}

float2 ScreenUv(float2 screen) {
    return screen / float2(info.x, 1.0);
}

float BlurMip(float footprint) {
    return clamp(log2(max(footprint * info.y, 1.0)), 0.0, info.z - 1.0);
}

float4 Splash(VsOut i) {
    // A thin, irregular patch of water throws the view under it out of focus.
    float2 q = i.local * float2(1.08, 0.90);
    q.x += 0.14 * sin(q.y * 4.2 + i.drop.w * 6.2831);
    const float d = DropShape(q, i.drop.w);
    const float blur = look.y;
    const float mask = 1.0 - smoothstep(lerp(0.68, 0.42, blur), 1.10, d);
    if (mask <= 0.001) discard;
    const float2 dir = i.motion.xy;
    const float2 side = float2(-dir.y, dir.x);
    const float r = i.drop.z;
    const float2 uv = ScreenUv(i.pos.xy / info.y + (side * q.x + dir * q.y) * (r * 0.16 * look.x * mask));
    const float mip = BlurMip(r * blur * 0.60);
    const float2 step = ScreenUv(float2(1, 1) * (r * blur * 0.15));
    float3 color = scene.SampleLevel(linearClamp, uv, mip).rgb * 0.4;
    color += 0.15 * (scene.SampleLevel(linearClamp, uv + float2(step.x, 0), mip).rgb
                   + scene.SampleLevel(linearClamp, uv - float2(step.x, 0), mip).rgb
                   + scene.SampleLevel(linearClamp, uv + float2(0, step.y), mip).rgb
                   + scene.SampleLevel(linearClamp, uv - float2(0, step.y), mip).rgb);
    return float4(color, mask * i.motion.w * 0.96);
}

float4 Rivulet(VsOut i) {
    const float r = i.drop.z;
    const float run = i.shape.y;
    const float t = -i.local.y / run;
    const float phase = i.drop.w * 6.2831;
    // Width and curvature vary independently as the bead leaves water behind it.
    const float bend = sin(phase + 8.0 * t) * 0.50 + sin(phase * 2.0 + 23.0 * t) * 0.19;
    const float center = smoothstep(0.0, 0.25, t) * bend;
    const float width = (0.34 + 0.14 * sin(phase + 36.0 * t)) * lerp(1.25, 0.56, t);
    const float x = (i.local.x - center) / width;
    const float blur = look.y;
    const float mask = (1.0 - smoothstep(lerp(0.82, 0.50, blur), 1.23, abs(x)))
        * (1.0 - smoothstep(0.76, 1.0, t)) * smoothstep(0.004, 0.018, run * r);
    if (mask <= 0.001) return 0;
    const float2 side = float2(-i.motion.y, i.motion.x);
    const float2 uv = ScreenUv(i.pos.xy / info.y - side * clamp(x, -1.0, 1.0) * r * (0.3 + look.x * 0.70));
    const float mip = BlurMip(r * blur * 0.85);
    const float3 color = scene.SampleLevel(linearClamp, uv, mip).rgb;
    const float rim = smoothstep(0.42, 0.91, abs(x));
    const float3 edge = x < 0 ? lerp(color, 1.0, 0.27) : color * 0.62;
    return float4(lerp(color, edge, rim), mask * i.motion.w * 0.96);
}

float4 Bead(VsOut i) {
    // Keep a rounded cap where the stretched bead meets its trail.
    float2 q = i.local;
    const float behind = max(-q.y, 0.0);
    const float stretch = i.motion.z;
    q.y = q.y < 0 ? q.y / stretch : q.y;
    q.x *= 1.0 + 0.45 * saturate(behind / stretch) * saturate(behind / stretch); // a soft, rounded tail
    const float seed = i.drop.w;
    const float d = DropShape(q, seed);
    const float blur = look.y;
    const float edge = lerp(0.06, 0.32, blur);
    const float alpha = 1.0 - smoothstep(1.0 - edge, 1.0 + edge * 0.5, d);
    if (alpha <= 0.001) return 0;

    // A dome of water over that outline: its slope (the gradient of the outline's distance) bends
    // the view, so a lumpy edge bends the picture lumpily, as a real drop does. A real drop shows
    // the scene behind it shrunk and flipped, as a tiny wide-angle lens does, so the sample is taken
    // mirrored through the centre.
    const float dd = saturate(d);
    const float h = sqrt(max(1.0 - dd * dd, 0.0));
    const float aspect = info.x;
    const float e = 0.03;
    const float2 grad = float2(DropShape(q + float2(e, 0), seed) - DropShape(q - float2(e, 0), seed),
                               DropShape(q + float2(0, e), seed) - DropShape(q - float2(0, e), seed)) / (2.0 * e);
    float2 n2 = (length(grad) > 1e-4 ? normalize(grad) : float2(0, 0)) * dd; // outward, 0 at the middle, 1 at the rim
    const float r = i.drop.z;
    const float reach = r * (2.2 + 2.6 * look.x);   // how far around the drop it gathers from
    // Back to screen axes: local x runs along side = (-dir.y, dir.x), local y along dir.
    const float2 dir = i.motion.xy;
    float2 rot = n2.x * float2(-dir.y, dir.x) + n2.y * dir;
    float2 uv = i.drop.xy - float2(rot.x / aspect, rot.y) * reach * lerp(1.0, dd, 0.35);
    // On the glass the drops are far out of focus, and so is the world seen through them.
    const float mip = BlurMip(r * blur * 1.65);
    float3 col = scene.SampleLevel(linearClamp, saturate(uv), mip).rgb;
    // Water absorbs at grazing angles and reflects the darker surroundings: a dark rim.
    const float rim = pow(1.0 - h, 1.4);
    col *= lerp(1.0, 0.22, rim);
    // The edge where the water meets the glass reads as a thin dark line, which is most of what
    // makes a small drop visible at all.
    col *= 1.0 - 0.3 * smoothstep(0.72, 0.96, dd);
    // A highlight on the upper left of the dome from the light above, softened as the drop blurs.
    const float3 nrm = normalize(float3(rot * 1.15, h + 0.25));
    const float3 L = normalize(float3(-0.45, -0.6, 0.65));
    const float spec = pow(saturate(dot(nrm, L)), lerp(28.0, 14.0, blur)) * lerp(1.15, 0.6, blur);
    // The light the drop focuses: a soft glint on the side away from the highlight.
    const float glint = pow(saturate(dot(nrm, float3(-L.x, -L.y, L.z))), 10.0) * lerp(0.35, 0.18, blur);
    col += spec + glint;
    // Water lets a touch less light through than air.
    col *= 0.97;
    return float4(col, alpha * lerp(1.0, 0.9, blur) * i.motion.w);
}

float4 PS(VsOut i) : SV_Target {
    if (i.shape.x == 2.0) return Splash(i);
    float4 bead = 0, ribbon = 0;
    if (i.local.y >= -i.motion.z * 1.55) bead = Bead(i);
    if (i.shape.x == 1.0 && i.local.y < 0 && -i.local.y < i.shape.y)
        ribbon = Rivulet(i);
    const float behind = ribbon.a * (1.0 - bead.a);
    const float alpha = bead.a + behind;
    if (alpha <= 0.001) discard;
    return float4((bead.rgb * bead.a + ribbon.rgb * behind) / alpha, alpha);
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

void LensRain_RunMainThreadFrame() {
	const Lens::Values L = LensFx_GetKeyed();
	const bool on = L[Lens::RainOn] > 0.5f;
	bool clear = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		clear = g_clearRequested;
		g_clearRequested = false;
	}
	const bool prewarm = on && !g_wasOn;
	g_wasOn = on;
	if (!on || clear) {
		g_sim.Reset();
		g_havePose = false;
		if (!on) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_shared.clear();
			g_sharedOn = false;
			return;
		}
	}

	double origin[3] = {}, angles[3] = {}, fov = 90;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	// Time: the demo's own clock while one plays (a paused demo freezes the drops, and a render steps
	// them exactly one output frame at a time), the wall clock elsewhere.
	double demoTime = 0;
	const bool demo = g_MirvTime.GetCurrentDemoTime(demoTime);
	const double now = demo ? demoTime : WallSeconds();
	double dt = 0;
	if (g_havePose && demo == g_prevWasDemo) dt = now - g_prevTime;
	// A seek (time runs backwards, or jumps): keep the drops, take no motion from the jump.
	const bool jump = dt < 0 || dt > 0.25;
	LensRain::Motion m;
	if (g_havePose && !jump)
		m = LensRain::MotionFrom(g_prevOrigin, g_prevAngles, origin, angles, dt);
	else
		m = LensRain::MotionFrom(origin, angles, origin, angles, 0);
	if (jump) dt = 0;

	LensRain::Params p;
	p.amount = L[Lens::RainAmount];
	p.size = L[Lens::RainSize];
	p.slide = L[Lens::RainSlide];
	p.trails = L[Lens::RainTrails];
	p.impact = L[Lens::RainImpact];
	float aspect;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		aspect = g_sharedAspect;
	}
	// Switched on: three seconds of rain land at once on a still camera, so a paused shot shows the
	// drops right away (and a render that switches it on gets the same lens every time).
	if (prewarm) {
		LensRain::Motion still;
		still.gravity[0] = m.gravity[0]; still.gravity[1] = m.gravity[1];
		for (int i = 0; i < 180; ++i) g_sim.Step(1.0f / 60.0f, aspect, still, p);
	}
	g_sim.Step((float)dt, aspect, m, p);

	for (int i = 0; i < 3; ++i) { g_prevOrigin[i] = origin[i]; g_prevAngles[i] = angles[i]; }
	g_prevTime = now;
	g_prevWasDemo = demo;
	g_havePose = true;

	if (MvmDebugLog_Active()) {
		static unsigned s_log = 0;
		if ((s_log++ % 60) == 0) {
			unsigned splashes = 0, strips = 0;
			for (const auto& d : g_sim.Drops()) {
				if (d.kind == LensRain::Kind::Splash) ++splashes;
				if (std::hypot(d.trailX, d.trailY) >= 0.018f) ++strips;
			}
			MvmDebugLog_LinefAlways("lens.rain", "drops=%zu splashes=%u strips=%u dt=%.4f demo=%d v(right=%.0f up=%.0f fwd=%.0f) turn(yaw=%.0f pitch=%.0f) gravity=(%.2f %.2f) amount=%.0f slide=%.2f",
				g_sim.Drops().size(), splashes, strips, dt, demo ? 1 : 0, m.right, m.up, m.forward, m.yawRate, m.pitchRate, m.gravity[0], m.gravity[1], p.amount, p.slide);
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	g_shared = g_sim.Drops();
	g_sharedOn = true;
	g_sharedRefraction = L[Lens::RainRefraction];
	g_sharedBlur = L[Lens::RainBlur];
}

bool LensRain_Active() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_sharedOn && !g_shared.empty();
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

bool LensRain_Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target) {
	std::vector<LensRain::Drop> drops;
	float refraction, blur;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_sharedOn || g_shared.empty()) return false;
		drops = g_shared;
		refraction = g_sharedRefraction;
		blur = g_sharedBlur;
	}
	if (!immediate || !target || !PumpDevice()) return false;

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
	k.look = { refraction, blur, 0, 0 };
	for (int i = 0; i < n; ++i) {
		const LensRain::Drop& d = drops[i];
		// Drawn with its splat and drain: the size and opacity of its point in life.
		float size, alpha;
		LensRain::Lifecycle(d, size, alpha);
		k.drops[i] = {
			{ d.x, d.y, LensRain::VisualRadius(d) * size, d.seed },
			{ d.vx, d.vy, static_cast<float>(d.kind), alpha },
			{ d.trailX, d.trailY, 0, 0 },
		};
	}

	ID3D11DeviceContext* c = g.ctx.Get();
	c->ClearState();
	// The world the drops look through, out of focus: a copy with its mips.
	if (td.SampleDesc.Count > 1) c->ResolveSubresource(g.copy.Get(), 0, tex.Get(), 0, typed);
	else c->CopySubresourceRegion(g.copy.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
	c->GenerateMips(g.copySrv.Get());
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
