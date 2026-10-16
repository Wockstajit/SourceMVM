#define NOMINMAX
#include "RelightLightEditor.h"
#include "RelightLightEditorMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Config/PracticeSessionFlag.h" // a practice session writes nothing
#include <cstdlib>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
struct Shown {
    std::string key;
    Vec3 position{};
    bool on = true;        // shines this frame
    bool userOff = false;  // switched off in the editor
};
std::mutex g_lock;
std::atomic<bool> g_active{false};
std::string g_map;                 // map whose choices are loaded
std::set<std::string> g_off;       // keys switched off on g_map
std::vector<Shown> g_shown;        // the frame's lights, for the markers
std::vector<LightMarker> g_markers;// last drawn markers, for F
float g_width = 0, g_height = 0;
std::string g_aimed;               // key under the crosshair last frame

std::filesystem::path ChoicesPath(const std::string& map) {
    // %APPDATA%/HLAE, the same root as GetHlaeRoamingAppDataFolderW (not linked into the unit tests).
    const wchar_t* roaming = _wgetenv(L"APPDATA");
    return std::filesystem::path(roaming ? roaming : L".") / L"HLAE" / L"source_mvm" / L"relight" / L"lights" /
           (std::filesystem::path(map).filename().wstring() + L".txt");
}
void LoadLocked(const std::string& map) {
    if (map == g_map) return;
    g_map = map;
    g_off.clear();
    if (map.empty() || PracticeSession_Active()) return;
    std::ifstream in(ChoicesPath(map));
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g_off.insert(line);
    }
}
void SaveLocked() {
    if (g_map.empty() || PracticeSession_Active()) return;
    std::error_code error;
    const auto path = ChoicesPath(g_map);
    if (g_off.empty()) { std::filesystem::remove(path, error); return; }
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& key : g_off) out << key << '\n';
}

// ---- markers ----
constexpr char kMarkerShader[] = R"HLSL(
struct Marker { float2 center; float2 radius; float4 color; float ring; float3 pad; };
StructuredBuffer<Marker> markers:register(t0);
struct Varying { float4 position:SV_Position; float2 local:TEXCOORD0; nointerpolation uint index:TEXCOORD1; };
Varying VS(uint id:SV_VertexID) {
    const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };
    Marker m = markers[id / 6]; float2 c = corners[id % 6];
    Varying o; o.position = float4(m.center + c * m.radius, 0, 1); o.local = c; o.index = id / 6; return o;
}
float4 PS(Varying i):SV_Target {
    Marker m = markers[i.index]; float r = length(i.local);
    float w = fwidth(r) * 1.5;
    float disc = 1 - smoothstep(.55 - w, .55, r);                       // filled bulb
    float outline = smoothstep(.55 - w, .55, r) * (1 - smoothstep(.68, .68 + w, r)); // dark edge
    float ring = m.ring * (smoothstep(.8 - w, .8, r) * (1 - smoothstep(.95, .95 + w, r)));
    float3 rgb = m.color.rgb * disc + float3(.05, .05, .05) * outline + float3(1, 1, 1) * ring;
    float a = max(max(disc * m.color.a, outline * .8 * m.color.a), ring);
    if (a <= 0) discard;
    return float4(rgb / max(disc + outline + ring, 1e-3), a);
}
)HLSL";
struct GpuMarker { float cx, cy, rx, ry, r, g, b, a, ring, pad[3]; };
constexpr unsigned MarkerLimit = 4096;
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> commands;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    bool failed = false;
} g_gpu;
bool GpuReady(ID3D11Device* device) {
    if (g_gpu.device.Get() != device) { g_gpu = Gpu{}; g_gpu.device = device; }
    if (g_gpu.vs) return true;
    if (g_gpu.failed) return false;
    g_gpu.failed = true;
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(Environment::CompileEnvironmentShader(kMarkerShader, sizeof(kMarkerShader) - 1, "RelightLightMarkers", nullptr, "VS",
                                                     "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &vs, &errors)) ||
        FAILED(Environment::CompileEnvironmentShader(kMarkerShader, sizeof(kMarkerShader) - 1, "RelightLightMarkers", nullptr, "PS",
                                                     "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &ps, &errors))) {
        MvmDebugLog_LinefAlways("relight.lighteditor", "shader failed: %s",
                                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compiler unavailable");
        return false;
    }
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = sizeof(GpuMarker) * MarkerLimit;
    b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    b.StructureByteStride = sizeof(GpuMarker);
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_gpu.vs)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_gpu.ps)) ||
        FAILED(device->CreateBuffer(&b, nullptr, &g_gpu.buffer)) ||
        FAILED(device->CreateShaderResourceView(g_gpu.buffer.Get(), nullptr, &g_gpu.view)) ||
        FAILED(device->CreateBlendState(&bd, &g_gpu.blend)) || FAILED(device->CreateRasterizerState(&rd, &g_gpu.raster)) ||
        FAILED(device->CreateDepthStencilState(&dd, &g_gpu.depth)) || FAILED(device->CreateDeferredContext(0, &g_gpu.commands))) {
        g_gpu.vs.Reset();
        return false;
    }
    g_gpu.failed = false;
    return true;
}
} // namespace

void SetLightEditor(bool on) {
    g_active = on;
    if (!on) { std::lock_guard<std::mutex> lock(g_lock); g_markers.clear(); g_aimed.clear(); }
}
bool LightEditorActive() { return g_active; }

void ApplyLightChoices(Scene& scene) {
    std::lock_guard<std::mutex> lock(g_lock);
    LoadLocked(scene.map);
    const bool editing = g_active;
    if (editing) g_shown.clear();
    for (auto& light : scene.lights) {
        const auto key = LightEditorKey(light);
        const bool off = g_off.count(key) != 0;
        if (off) light.enabled = false;
        if (editing) g_shown.push_back({key, light.position, light.enabled, off});
    }
}
void ResetLightChoices() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_off.clear();
    SaveLocked();
}
void ReloadLightChoices() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_map.clear();
    g_off.clear();
}
unsigned LightsSwitchedOff() {
    std::lock_guard<std::mutex> lock(g_lock);
    return unsigned(g_off.size());
}

bool LightEditorKey(int vkey, bool down) {
    if (!g_active) return false;
    constexpr int kF = 0x46, kEscape = 0x1B;
    if (vkey == kEscape) {
        if (down) SetLightEditor(false);
        return true;
    }
    if (vkey != kF) return false;
    // One switch per physical press: auto-repeat and the duplicate key-down the game's input
    // layer delivers for one press (measured ~250 ms apart) are ignored.
    static bool held = false;
    static ULONGLONG last = 0;
    if (!down) { held = false; return true; }
    const ULONGLONG now = GetTickCount64();
    if (held || now - last < 400) return true;
    held = true;
    last = now;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_aimed.empty()) return true;
    const bool nowOff = g_off.insert(g_aimed).second;
    if (!nowOff) g_off.erase(g_aimed);
    SaveLocked();
    MvmDebugLog_LinefAlways("relight.lighteditor", "map=%s light=%s %s off=%zu", g_map.c_str(), g_aimed.c_str(),
                            nowOff ? "switched off" : "switched on", g_off.size());
    return true;
}

void DrawLightMarkers(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, const float* worldToClip, Vec3 camera,
                      float width, float height) {
    if (!g_active || !immediate || !target || !(width > 0) || !(height > 0)) return;
    ComPtr<ID3D11Device> device;
    immediate->GetDevice(&device);
    if (!GpuReady(device.Get())) return;
    std::vector<GpuMarker> gpu;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_markers.clear();
        for (const auto& s : g_shown) {
            LightMarker m;
            m.key = s.key;
            const Vec3 d = s.position - camera;
            m.distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            m.visible = ProjectToScreen(worldToClip, s.position, width, height, m.x, m.y);
            g_markers.push_back(m);
        }
        g_width = width; g_height = height;
        const int aimed = AimedLightMarker(g_markers, width, height);
        g_aimed = aimed >= 0 ? g_markers[size_t(aimed)].key : std::string();
        for (size_t i = 0; i < g_markers.size() && gpu.size() < MarkerLimit; ++i) {
            const auto& m = g_markers[i];
            if (!m.visible) continue;
            const auto& s = g_shown[i];
            // Constant size on screen, a little smaller with distance so far lights do not clutter.
            const float pixels = std::clamp(22.f - m.distance / 200.f, 10.f, 22.f) * (int(i) == aimed ? 1.6f : 1.f);
            GpuMarker g{};
            g.cx = m.x / width * 2 - 1; g.cy = 1 - m.y / height * 2;
            g.rx = pixels / width * 2; g.ry = pixels / height * 2;
            if (s.userOff) { g.r = .85f; g.g = .25f; g.b = .2f; g.a = .45f; }   // switched off here
            else if (!s.on) { g.r = .6f; g.g = .6f; g.b = .6f; g.a = .35f; }   // off for another reason (fill light)
            else { g.r = 1.f; g.g = .85f; g.b = .35f; g.a = .95f; }            // shining
            g.ring = int(i) == aimed ? 1.f : 0.f;
            // Far lights recede: full strength within LightMarkerNear, faint by LightMarkerFar, so a
            // room's own lamps stand out from the dots of every room behind it. The aimed one stays clear.
            if (int(i) != aimed) g.a *= LightMarkerOpacity(m.distance);
            gpu.push_back(g);
        }
    }
    if (gpu.empty()) return;
    auto ctx = g_gpu.commands.Get();
    ctx->ClearState();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_gpu.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, gpu.data(), gpu.size() * sizeof(GpuMarker));
    ctx->Unmap(g_gpu.buffer.Get(), 0);
    const D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(g_gpu.raster.Get());
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->OMSetBlendState(g_gpu.blend.Get(), nullptr, ~0u);
    ctx->OMSetDepthStencilState(g_gpu.depth.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g_gpu.ps.Get(), nullptr, 0);
    auto view = g_gpu.view.Get();
    ctx->VSSetShaderResources(0, 1, &view);
    ctx->PSSetShaderResources(0, 1, &view);
    ctx->Draw(UINT(gpu.size() * 6), 0);
    ctx->ClearState();
    ComPtr<ID3D11CommandList> list;
    if (SUCCEEDED(ctx->FinishCommandList(FALSE, &list))) immediate->ExecuteCommandList(list.Get(), TRUE);
}

void EndLightEditor() {
    g_active = false;
    g_gpu = Gpu{};
    std::lock_guard<std::mutex> lock(g_lock);
    g_markers.clear(); g_shown.clear(); g_aimed.clear();
}
} // namespace Filmmaker::Relight
