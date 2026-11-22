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
	F4 info;  // x aspect (w/h), y drops, z mip levels of the scene copy, w unused
	F4 look;  // x refraction, y blur (0..1), zw unused
	F4 drops[kMaxDrops * 2];
};
static_assert(sizeof(Constants) % 16 == 0, "D3D constant buffers are 16-byte aligned");
static_assert(sizeof(Constants) <= 65536, "a constant buffer holds at most 64 KB");

const char kShader[] = R"HLSL(
cbuffer RainConstants : register(b0) {
    float4 info;   // x aspect (w/h), y drops, z mip levels of the scene copy
    float4 look;   // x refraction, y blur (0..1)
    float4 drops[768]; // per drop: [0] centre xy (screen: x 0..aspect, y 0..1 down), radius, seed; [1] velocity xy (heights/s), age, unused
};
SamplerState linearClamp : register(s0);
Texture2D<float4> scene : register(t0);

struct VsOut {
    float4 pos : SV_Position;
    float2 local : LOCAL;      // -1..1 across the drop, stretched along its motion
    nointerpolation float4 drop : DROP;   // centre uv, radius (heights), seed
    nointerpolation float4 motion : MOTION; // unit direction of motion xy, stretch, age
};

VsOut VS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };
    const float4 a = drops[iid * 2];
    const float4 b = drops[iid * 2 + 1];
    const float aspect = info.x;
    const float speed = length(b.xy);
    const float2 dir = speed > 1e-5 ? b.xy / speed : float2(0, 1);
    // A running drop is drawn longer along its path: a bead with a tail.
    const float stretch = 1.0 + min(speed / max(a.z * 20.0, 1e-3), 1.0);
    const float2 c = corners[vid];
    // Along the motion the quad reaches `stretch` radii behind the drop and one ahead.
    const float2 side = float2(-dir.y, dir.x);
    const float along = c.y > 0 ? 1.0 : -stretch;
    const float2 offset = (dir * along + side * c.x) * a.z * 1.15; // a little margin for the soft rim
    const float2 screen = a.xy + offset;
    VsOut o;
    o.pos = float4(screen.x / aspect * 2.0 - 1.0, 1.0 - screen.y * 2.0, 0, 1);
    o.local = float2(c.x, along) * 1.15;
    o.drop = float4(a.x / aspect, a.y, a.z, a.w);
    o.motion = float4(dir, stretch, b.z);
    return o;
}

float4 PS(VsOut i) : SV_Target {
    // The drop's outline: a circle ahead of its centre, an egg that tapers behind it while it runs.
    float2 q = i.local;
    float behind = max(-q.y, 0.0);
    const float stretch = i.motion.z;
    q.y = q.y < 0 ? q.y / stretch : q.y;
    // Tapers toward the tail; the wobble keeps two drops from being the same circle.
    const float wob = 0.06 * sin(atan2(q.y, q.x) * 3.0 + i.drop.w * 6.2831);
    q.x *= 1.0 + 1.4 * saturate(behind / stretch); // a thin trail, not a cone
    const float d = length(q) / (1.0 + wob);
    const float blur = look.y;
    const float edge = lerp(0.06, 0.32, blur);
    const float alpha = 1.0 - smoothstep(1.0 - edge, 1.0 + edge * 0.5, d);
    if (alpha <= 0.001) discard;

    // A dome of water: its slope bends the view. A real drop shows the scene behind it shrunk and
    // flipped, as a tiny wide-angle lens does, so the sample is taken mirrored through the centre.
    const float dd = saturate(d);
    const float h = sqrt(max(1.0 - dd * dd, 0.0));
    const float aspect = info.x;
    float2 n2 = q * (1.0 / max(d, 1e-3)) * dd;      // outward, 0 at the middle, 1 at the rim
    const float r = i.drop.z;
    const float reach = r * (2.2 + 2.6 * look.x);   // how far around the drop it gathers from
    // Back to screen axes: local x runs along side = (-dir.y, dir.x), local y along dir.
    const float2 dir = i.motion.xy;
    float2 rot = n2.x * float2(-dir.y, dir.x) + n2.y * dir;
    float2 uv = i.drop.xy - float2(rot.x / aspect, rot.y) * reach * lerp(1.0, dd, 0.35);
    // On the glass the drops are far out of focus, and so is the world seen through them.
    const float levels = info.z;
    const float mip = clamp(log2(max(r * 1080.0, 1.0)) * (0.35 + blur * 0.6) + blur * 1.5, 0.0, levels - 1.0);
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
    return float4(col, alpha * lerp(1.0, 0.9, blur));
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
		if ((s_log++ % 60) == 0)
			MvmDebugLog_LinefAlways("lens.rain", "drops=%zu dt=%.4f demo=%d v(right=%.0f up=%.0f fwd=%.0f) turn(yaw=%.0f pitch=%.0f) gravity=(%.2f %.2f) amount=%.0f slide=%.2f",
				g_sim.Drops().size(), dt, demo ? 1 : 0, m.right, m.up, m.forward, m.yawRate, m.pitchRate, m.gravity[0], m.gravity[1], p.amount, p.slide);
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

	Constants k{};
	const int n = (int)(std::min)(drops.size(), (size_t)kMaxDrops);
	k.info = { aspect, (float)n, (float)g.levels, 0 };
	k.look = { refraction, blur, 0, 0 };
	for (int i = 0; i < n; ++i) {
		const LensRain::Drop& d = drops[i];
		k.drops[i * 2] = { d.x, d.y, d.r, d.seed };
		k.drops[i * 2 + 1] = { d.vx, d.vy, d.age, 0 };
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
