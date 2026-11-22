#define NOMINMAX
#include "EventFx.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/CameraPath.h"
#include "../Movie/ParticleFxInternal.h"
#include "../Movie/SceneKeys.h"
#include "../Movie/SceneKeyframeCatalog.h"
#include "../Movie/SceneKeyframeTrackMath.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Render/RenderPrivateTextureScope.h"
#include "../../MirvTime.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"
#include "../../../Shared/AfxConsole.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace Filmmaker {
using Microsoft::WRL::ComPtr;
namespace {

std::mutex g_mutex;
bool g_masterEnabled = false;
std::vector<EventFx::EventPreset> g_presets = EventFx::DefaultPresets();
int g_nextPresetId = 6;
int g_selectedPresetId = 1;
std::vector<EventFx::ActiveCue> g_activeCues;
uint32_t g_cueSeed = 1;

// Deduplication for double server/client event delivery.
struct DedupStamp {
	int trigger = -1;
	int tick = -1;
	ULONGLONG wallMs = 0;
};
std::vector<DedupStamp> g_recentTriggers;
int g_lastDemoTick = -1;
double g_lastDemoTime = -1.0;

std::atomic<bool> g_hasActiveCues{ false };

double CurrentClock() {
	double t = 0.0;
	if (!g_MirvTime.GetCurrentDemoTime(t)) t = g_MirvTime.curtime_get();
	return std::isfinite(t) ? t : 0.0;
}

EventFx::EventPreset* FindPresetLocked(int id) {
	for (auto& p : g_presets)
		if (p.id == id) return &p;
	return nullptr;
}

void FirePresetLocked(const EventFx::EventPreset& p, double nowTime, bool hasPos, float x, float y, float z) {
	EventFx::ActiveCue cue;
	cue.preset = p;
	cue.triggerTime = nowTime;
	cue.seed = ++g_cueSeed;
	if (g_activeCues.size() >= 24) g_activeCues.erase(g_activeCues.begin());
	g_activeCues.push_back(cue);
	g_hasActiveCues.store(true, std::memory_order_release);

	// Optional native CS2 world light burst (flashbang or warm HE style).
	if (p.lightBurst) {
		float lx = x, ly = y, lz = z;
		if (!hasPos) {
			double org[3] = {}, ang[3] = {}, fov = 90.0;
			CameraBridge_GetCurrentCamera(org, ang, fov);
			const float yaw = (float)ang[1] * 3.14159265f / 180.0f;
			lx = (float)org[0] + std::cos(yaw) * 96.0f;
			ly = (float)org[1] + std::sin(yaw) * 96.0f;
			lz = (float)org[2];
		}
		fx::NativeLightFx_OnDetonate(p.lightFlashStyle, (int)(cue.seed & 0xFFFF), lx, ly, lz);
	}
}

void TriggerKindLocked(EventFx::TriggerKind kind, bool hasPos, float x, float y, float z) {
	if (!g_masterEnabled) return;
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	const ULONGLONG wall = GetTickCount64();
	for (const auto& r : g_recentTriggers) {
		if (r.trigger == (int)kind && (r.tick == tick || (wall - r.wallMs) < 120))
			return;
	}
	if (g_recentTriggers.size() >= 32) g_recentTriggers.erase(g_recentTriggers.begin());
	g_recentTriggers.push_back({ (int)kind, tick, wall });

	const double now = CurrentClock();
	for (const auto& p : g_presets) {
		if (p.enabled && p.trigger == kind)
			FirePresetLocked(p, now, hasPos, x, y, z);
	}
}

// ---- Lightweight D3D11 Post-Process Pulse Pass ----
constexpr char kPostShader[] = R"HLSL(
cbuffer EventPulseCB : register(b0) {
    float4 pulseA; // exposureScale, tintStrength, chromatic, vignette
    float4 pulseB; // tintR, tintG, tintB, 0
};
Texture2D<float4> sceneColor : register(t0);
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

float4 PS(VsOut i) : SV_Target {
    float2 c = i.uv - 0.5;
    float r = length(c);
    float ch = pulseA.z * 0.012 * (0.4 + r * 1.4);
    float2 dir = normalize(c + 1e-5) * ch;
    float3 col;
    col.r = sceneColor.SampleLevel(linearClamp, saturate(i.uv + dir), 0).r;
    col.g = sceneColor.SampleLevel(linearClamp, i.uv, 0).g;
    col.b = sceneColor.SampleLevel(linearClamp, saturate(i.uv - dir), 0).b;

    // Soft-knee exposure boost so bright flashes bloom gracefully without harsh clipping.
    col *= pulseA.x;
    col = col / (1.0 + max(col - 0.85, 0.0) * 0.45);
    col = lerp(col, pulseB.rgb, saturate(pulseA.y));
    float vig = 1.0 - smoothstep(0.32, 0.88, r) * saturate(pulseA.w) * 0.85;
    col *= vig;
    return float4(saturate(col), 1.0);
}
)HLSL";

struct PostCb {
	float pulseA[4];
	float pulseB[4];
};

struct PostBlobs {
	ComPtr<ID3DBlob> vs, ps;
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const PostBlobs> g_blobs;
std::atomic<bool> g_compileStarted{ false };

struct PostGpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> dsOff;
	ComPtr<ID3D11BlendState> blendOpaque;
	ComPtr<ID3D11Texture2D> colorCopy;
	ComPtr<ID3D11ShaderResourceView> colorSrv;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
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

void CompilePost() {
	auto b = std::make_shared<PostBlobs>();
	ComPtr<ID3DBlob> err;
	const size_t n = sizeof(kPostShader) - 1;
	b->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kPostShader, n, "EventFxPost", nullptr, "VS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->vs, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kPostShader, n, "EventFxPost", nullptr, "PS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->ps, &err));
	std::lock_guard<std::mutex> lock(g_blobLock);
	g_blobs = b;
}

bool EnsurePostShaders() {
	if (g_gpu.vs) return true;
	if (!g_gpu.device || g_gpu.failed) return false;
	std::shared_ptr<const PostBlobs> b;
	{ std::lock_guard<std::mutex> lock(g_blobLock); b = g_blobs; }
	if (!b) return false;
	if (!b->ok) { g_gpu.failed = true; return false; }

	RenderPrivateTextureScope priv;
	auto* d = g_gpu.device.Get();
	if (FAILED(d->CreateVertexShader(b->vs->GetBufferPointer(), b->vs->GetBufferSize(), nullptr, &g_gpu.vs))
		|| FAILED(d->CreatePixelShader(b->ps->GetBufferPointer(), b->ps->GetBufferSize(), nullptr, &g_gpu.ps))) {
		g_gpu.failed = true; return false;
	}
	D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(PostCb); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3* kv,
	SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool s_tried = false;
	static SaveKV3AsJSON_t s_fn = nullptr;
	if (!s_tried) {
		s_tried = true;
		HMODULE tier0 = GetModuleHandleA("tier0.dll");
		if (tier0)
			s_fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return s_fn;
}

bool EventDataToJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!fn) return false;
	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get())
		return false;
	return JsonParse(text.Get(), out) && out.type == JsonValue::Type::Object;
}

} // namespace

void EventFx_OnLevelInit() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_activeCues.clear();
	g_recentTriggers.clear();
	g_lastDemoTick = -1;
	g_lastDemoTime = -1.0;
	g_hasActiveCues.store(false, std::memory_order_release);
}

void EventFx_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!event) return;
	const char* name = event->GetName();
	if (!name || !*name) return;

	bool headshot = false;
	bool hasPos = false;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	const bool isDeath = (0 == std::strcmp(name, "player_death"));
	const bool isGrenade = (0 == std::strcmp(name, "flashbang_detonate")
		|| 0 == std::strcmp(name, "hegrenade_detonate")
		|| 0 == std::strcmp(name, "smokegrenade_detonate")
		|| 0 == std::strcmp(name, "inferno_startburn"));
	const bool isBomb = (0 == std::strcmp(name, "bomb_exploded"));
	if (!isDeath && !isGrenade && !isBomb) return;

	if (isDeath || isGrenade) {
		JsonValue root;
		if (EventDataToJson(event, root)) {
			if (isDeath) {
				if (const JsonValue* v = root.Find("headshot"))
					headshot = v->AsBool(false) || v->AsInt(0) != 0;
			} else if (isGrenade) {
				const JsonValue* jx = root.Find("x");
				const JsonValue* jy = root.Find("y");
				const JsonValue* jz = root.Find("z");
				if (jx && jy && jz) {
					x = (float)jx->AsNumber(0.0);
					y = (float)jy->AsNumber(0.0);
					z = (float)jz->AsNumber(0.0);
					hasPos = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && (x != 0.0f || y != 0.0f);
				}
			}
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_masterEnabled) return;
	if (isDeath) {
		TriggerKindLocked(EventFx::TriggerKind::Kill, false, 0, 0, 0);
		if (headshot) TriggerKindLocked(EventFx::TriggerKind::Headshot, false, 0, 0, 0);
	} else if (0 == std::strcmp(name, "flashbang_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::FlashbangPop, hasPos, x, y, z);
	} else if (0 == std::strcmp(name, "hegrenade_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::HeGrenadePop, hasPos, x, y, z);
	} else if (0 == std::strcmp(name, "smokegrenade_detonate")) {
		TriggerKindLocked(EventFx::TriggerKind::SmokePop, hasPos, x, y, z);
	} else if (0 == std::strcmp(name, "inferno_startburn")) {
		TriggerKindLocked(EventFx::TriggerKind::MolotovPop, hasPos, x, y, z);
	} else if (isBomb) {
		TriggerKindLocked(EventFx::TriggerKind::BombExplode, false, 0, 0, 0);
	}
}

void EventFx_RunMainThreadFrame() {
	int tick = 0;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	const double now = CurrentClock();

	std::lock_guard<std::mutex> lock(g_mutex);
	// Clear active cues on backward seek so scrubbed playback stays clean.
	if (g_lastDemoTime >= 0.0 && now < g_lastDemoTime - 0.15) {
		g_activeCues.clear();
		g_recentTriggers.clear();
	}
	g_lastDemoTime = now;

	// Detect timeline marker crossings when moving forward in demo time.
	if (g_masterEnabled && haveTick && g_lastDemoTick >= 0 && tick > g_lastDemoTick && (tick - g_lastDemoTick) <= 64) {
		const auto& markers = CameraPathRef().Markers();
		for (const auto& m : markers) {
			if (m.tick > g_lastDemoTick && m.tick <= tick) {
				TriggerKindLocked(EventFx::TriggerKind::TimelineMarker, false, 0, 0, 0);
				break;
			}
		}
	}
	if (haveTick) g_lastDemoTick = tick;

	// Prune finished cues.
	const double wallNow = (double)GetTickCount64() * 0.001;
	g_activeCues.erase(std::remove_if(g_activeCues.begin(), g_activeCues.end(),
		[now, wallNow](const EventFx::ActiveCue& c) {
			const double clock = c.useWallClock ? wallNow : now;
			const double age = clock - c.triggerTime;
			return age < -0.5 || age > (double)(c.preset.attackSec + c.preset.decaySec + 0.05f);
		}), g_activeCues.end());
	g_hasActiveCues.store(!g_activeCues.empty(), std::memory_order_release);
}

bool EventFx_ApplyCameraImpulse(double curTime, float& pitch, float& yaw, float& roll, float& fov) {
	if (!g_hasActiveCues.load(std::memory_order_acquire)) return false;
	std::vector<EventFx::ActiveCue> copy;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		copy = g_activeCues;
	}
	const double wallNow = (double)GetTickCount64() * 0.001;
	const auto ev = EventFx::EvaluateActiveCues(copy, curTime, wallNow);
	if (!ev.active) return false;
	pitch += ev.pitchDelta;
	yaw += ev.yawDelta;
	roll += ev.rollDelta;
	fov = std::clamp(fov + ev.fovDelta, 10.0f, 165.0f);
	return true;
}

void EventFx_BeginDevice(ID3D11Device* device) {
	EventFx_EndDevice();
	if (!device) return;
	g_gpu.device = device;
	g_gpu.failed = false;
	device->CreateDeferredContext(0, &g_gpu.ctx);
	if (!g_compileStarted.exchange(true)) {
		std::thread(CompilePost).detach();
	}
}

void EventFx_EndDevice() {
	g_gpu = {};
}

bool EventFx_RenderPost(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, double demoTime) {
	if (!g_hasActiveCues.load(std::memory_order_acquire) || !immediate || !target || !g_gpu.ctx || !EnsurePostShaders())
		return false;

	std::vector<EventFx::ActiveCue> copy;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		copy = g_activeCues;
	}
	const double wallNow = (double)GetTickCount64() * 0.001;
	const auto ev = EventFx::EvaluateActiveCues(copy, demoTime, wallNow);
	if (!ev.active) return false;
	if (std::fabs(ev.exposureEv) < 1e-3f && ev.tintStrength < 1e-3f && ev.chromatic < 1e-3f && ev.vignette < 1e-3f)
		return false;

	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return false;
	D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);
	const DXGI_FORMAT typed = TypedColorFormat(rtvd.Format != DXGI_FORMAT_UNKNOWN ? rtvd.Format : d.Format);

	if (!g_gpu.colorCopy || g_gpu.w != d.Width || g_gpu.h != d.Height || g_gpu.fmt != typed) {
		RenderPrivateTextureScope priv;
		g_gpu.colorCopy.Reset(); g_gpu.colorSrv.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.MipLevels = 1; td.ArraySize = 1; td.Format = typed; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
		td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.colorCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.colorCopy.Get(), nullptr, &g_gpu.colorSrv)))
			return false;
		g_gpu.w = d.Width; g_gpu.h = d.Height; g_gpu.fmt = typed;
	}

	auto* c = g_gpu.ctx.Get();
	if (d.SampleDesc.Count > 1) c->ResolveSubresource(g_gpu.colorCopy.Get(), 0, src.Get(), 0, typed);
	else c->CopyResource(g_gpu.colorCopy.Get(), src.Get());

	PostCb cb{};
	cb.pulseA[0] = std::pow(2.0f, ev.exposureEv);
	cb.pulseA[1] = ev.tintStrength;
	cb.pulseA[2] = ev.chromatic;
	cb.pulseA[3] = ev.vignette;
	cb.pulseB[0] = ev.tint[0]; cb.pulseB[1] = ev.tint[1]; cb.pulseB[2] = ev.tint[2];
	c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &cb, 0, 0);

	D3D11_VIEWPORT vp{}; vp.Width = (float)d.Width; vp.Height = (float)d.Height; vp.MaxDepth = 1.0f;
	c->RSSetViewports(1, &vp);
	c->RSSetState(g_gpu.raster.Get());
	c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
	const float bf[4] = {};
	c->OMSetBlendState(g_gpu.blendOpaque.Get(), bf, 0xffffffff);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
	c->PSSetShader(g_gpu.ps.Get(), nullptr, 0);
	ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
	c->PSSetConstantBuffers(0, 1, cbs);
	ID3D11SamplerState* smps[1] = { g_gpu.sampler.Get() };
	c->PSSetSamplers(0, 1, smps);
	ID3D11ShaderResourceView* srvs[1] = { g_gpu.colorSrv.Get() };
	c->PSSetShaderResources(0, 1, srvs);
	ID3D11RenderTargetView* rts[1] = { target };
	c->OMSetRenderTargets(1, rts, nullptr);
	c->Draw(3, 0);
	ID3D11ShaderResourceView* nullSrv[1] = {};
	c->PSSetShaderResources(0, 1, nullSrv);
	ID3D11RenderTargetView* nullRt[1] = {};
	c->OMSetRenderTargets(1, nullRt, nullptr);

	ComPtr<ID3D11CommandList> cl;
	if (FAILED(c->FinishCommandList(FALSE, &cl)) || !cl) return false;
	{ GPU_PASS_SCOPE(immediate, "event fx pulse"); immediate->ExecuteCommandList(cl.Get(), TRUE); }
	return true;
}

void EventFx_AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	out << ",\"eventFx\":{"
		<< "\"on\":" << (g_masterEnabled ? "true" : "false")
		<< ",\"selected\":" << g_selectedPresetId
		<< ",\"activeCount\":" << g_activeCues.size()
		<< ",\"presets\":[";
	for (size_t i = 0; i < g_presets.size(); ++i) {
		const auto& p = g_presets[i];
		if (i) out << ",";
		out << "{\"id\":" << p.id
			<< ",\"name\":\"" << JsonBuilder::Escape(p.name) << "\""
			<< ",\"trigger\":\"" << EventFx::TriggerToken(p.trigger) << "\""
			<< ",\"enabled\":" << (p.enabled ? "true" : "false")
			<< ",\"attack\":" << p.attackSec
			<< ",\"decay\":" << p.decaySec
			<< ",\"exposureEv\":" << p.exposureEv
			<< ",\"tint\":[" << p.tint[0] << "," << p.tint[1] << "," << p.tint[2] << "]"
			<< ",\"tintStrength\":" << p.tintStrength
			<< ",\"shakeAmp\":" << p.shakeAmp
			<< ",\"shakeRoll\":" << p.shakeRoll
			<< ",\"shakeFreq\":" << p.shakeFreq
			<< ",\"fovKick\":" << p.fovKick
			<< ",\"lightBurst\":" << (p.lightBurst ? "true" : "false")
			<< ",\"lightFlash\":" << (p.lightFlashStyle ? "true" : "false")
			<< ",\"chromatic\":" << p.chromatic
			<< ",\"vignette\":" << p.vignette
			<< "}";
	}
	out << "]}";
}

void EventFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s eventfx on|off|toggle|reset\n"
			"%s eventfx test <id> | select <id> | enable <id> <0|1> | add | remove <id>\n"
			"%s eventfx set <id> <field> <value>\n"
			"%s eventfx bake <id>  (stamps exposure envelope into Scene Keyframes at playhead)\n",
			cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(2);
	if (0 == _stricmp(sub, "on")) { std::lock_guard<std::mutex> l(g_mutex); g_masterEnabled = true; return; }
	if (0 == _stricmp(sub, "off")) { std::lock_guard<std::mutex> l(g_mutex); g_masterEnabled = false; g_activeCues.clear(); g_hasActiveCues.store(false); return; }
	if (0 == _stricmp(sub, "toggle")) { std::lock_guard<std::mutex> l(g_mutex); g_masterEnabled = !g_masterEnabled; return; }
	if (0 == _stricmp(sub, "reset")) {
		std::lock_guard<std::mutex> l(g_mutex);
		g_masterEnabled = false;
		g_presets = EventFx::DefaultPresets();
		g_nextPresetId = 6;
		g_selectedPresetId = 1;
		g_activeCues.clear();
		g_hasActiveCues.store(false);
		return;
	}
	if (0 == _stricmp(sub, "select") && argc >= 4) {
		std::lock_guard<std::mutex> l(g_mutex);
		g_selectedPresetId = std::atoi(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(sub, "test") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> l(g_mutex);
		if (auto* p = FindPresetLocked(id)) {
			// Use wall-clock time for Test Fire so the pulse plays smoothly even while paused.
			const double wallNow = (double)GetTickCount64() * 0.001;
			FirePresetLocked(*p, wallNow, false, 0, 0, 0);
			if (!g_activeCues.empty()) g_activeCues.back().useWallClock = true;
		}
		return;
	}
	if (0 == _stricmp(sub, "enable") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const bool on = std::atoi(args->ArgV(4)) != 0;
		std::lock_guard<std::mutex> l(g_mutex);
		if (auto* p = FindPresetLocked(id)) p->enabled = on;
		return;
	}
	if (0 == _stricmp(sub, "add")) {
		std::lock_guard<std::mutex> l(g_mutex);
		EventFx::EventPreset p;
		p.id = g_nextPresetId++;
		p.name = "Custom Cue " + std::to_string(p.id);
		p.trigger = EventFx::TriggerKind::Kill;
		g_selectedPresetId = p.id;
		g_presets.push_back(p);
		return;
	}
	if (0 == _stricmp(sub, "remove") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> l(g_mutex);
		g_presets.erase(std::remove_if(g_presets.begin(), g_presets.end(),
			[id](const EventFx::EventPreset& p) { return p.id == id; }), g_presets.end());
		if (g_selectedPresetId == id) g_selectedPresetId = g_presets.empty() ? -1 : g_presets.front().id;
		return;
	}
	if (0 == _stricmp(sub, "bake") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		EventFx::EventPreset copy;
		{
			std::lock_guard<std::mutex> l(g_mutex);
			auto* p = FindPresetLocked(id);
			if (!p) return;
			copy = *p;
		}
		int curTick = 0;
		g_MirvTime.GetCurrentDemoTick(curTick);
		const auto keys = EventFx::BuildBakedExposureKeys(copy, (double)curTick, 64.0);
		const auto* spec = SceneKeys::FindSpec(SceneKeys::Catalog(), "exposure");
		if (spec) {
			SceneKeys_Edit([&](SceneKeys::SceneKeyframeTrack& tr) {
				tr.data.armed.insert(spec->id);
				for (const auto& bk : keys) {
					SceneKeys::Value v;
					v.numbers = { std::clamp(bk.exposureScale, spec->min, spec->max) };
					tr.Put(*spec, bk.tick, v);
				}
			});
			advancedfx::Message("%s eventfx: baked '%s' exposure pulse (%zu scene keys from tick %d).\n",
				cmd, copy.name.c_str(), keys.size(), curTick);
		}
		return;
	}
	if (0 == _stricmp(sub, "set") && argc >= 6) {
		const int id = std::atoi(args->ArgV(3));
		const char* f = args->ArgV(4);
		const char* val = args->ArgV(5);
		const float fv = (float)std::atof(val);
		using EventFx::Clamp;
		std::lock_guard<std::mutex> l(g_mutex);
		auto* p = FindPresetLocked(id);
		if (!p) return;
		if (0 == _stricmp(f, "trigger")) EventFx::TriggerFromToken(val, p->trigger);
		else if (0 == _stricmp(f, "attack")) p->attackSec = Clamp(fv, 0.005f, 1.0f);
		else if (0 == _stricmp(f, "decay")) p->decaySec = Clamp(fv, 0.02f, 5.0f);
		else if (0 == _stricmp(f, "exposureEv")) p->exposureEv = Clamp(fv, -2.0f, 4.0f);
		else if (0 == _stricmp(f, "tintStrength")) p->tintStrength = Clamp(fv, 0.0f, 1.0f);
		else if (0 == _stricmp(f, "shakeAmp")) p->shakeAmp = Clamp(fv, 0.0f, 10.0f);
		else if (0 == _stricmp(f, "shakeRoll")) p->shakeRoll = Clamp(fv, 0.0f, 10.0f);
		else if (0 == _stricmp(f, "shakeFreq")) p->shakeFreq = Clamp(fv, 1.0f, 40.0f);
		else if (0 == _stricmp(f, "fovKick")) p->fovKick = Clamp(fv, -15.0f, 20.0f);
		else if (0 == _stricmp(f, "lightBurst")) p->lightBurst = fv > 0.5f;
		else if (0 == _stricmp(f, "lightFlash")) p->lightFlashStyle = fv > 0.5f;
		else if (0 == _stricmp(f, "chromatic")) p->chromatic = Clamp(fv, 0.0f, 1.0f);
		else if (0 == _stricmp(f, "vignette")) p->vignette = Clamp(fv, 0.0f, 1.0f);
		return;
	}
	advancedfx::Warning("%s eventfx: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker
