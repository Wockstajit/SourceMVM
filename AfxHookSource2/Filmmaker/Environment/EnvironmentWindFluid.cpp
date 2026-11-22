#define NOMINMAX
#include "EnvironmentWindFluid.h"
#include "EnvironmentWindFluidMath.h"
#include "EnvironmentWindFluidShaders.h"
#include "EnvironmentGroundFogMath.h" // GroundFogTick: the demo clock split into fluid steps
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentWeatherCover.h"
#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <atomic>
#include <future>
#include <map>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
constexpr int kJacobiIterations = 20; // the pressure carries over between steps as a warm start
constexpr int kBodies = 12;           // BODIES in the shader, Frame::kMaxActors
constexpr float kBodyRadius = 44.f;   // how far round a player the air is dragged along
constexpr unsigned kIdleFrames = 300; // frames with gusts off before the fluid is released
constexpr int kObstacleRows = 8;      // rows of the building mask worked out a frame
// After a restart (a new grid or a seek) the air is run this far ahead at once, so the gust fronts
// are already blowing on the first frame, paused or not. Starting at rest, a paused demo or a
// scrubbed frame showed no gusts at all: the air only filled while the demo played.
constexpr float kPrewarmSeconds = 2.f;
constexpr int kPrewarmSteps = 40;

struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct StepConstants {
    float stepInfo[4], windInfo[4], gridInfo[4], bodyInfo[4];
    float bodies[kBodies][4];
    float bodyVelocity[kBodies][4];
    float overlayInfo[4], cameraInfo[4];
};
static_assert(sizeof(StepConstants) % 16 == 0, "D3D constant alignment");
enum Entry { Curl, Force, Advect, Divergence, Jacobi, Project }; // the order of WindFluidShaderEntries

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> shaders[WindFluidShaderEntryCount];
    ComPtr<ID3D11VertexShader> overlayVS;
    ComPtr<ID3D11PixelShader> overlayPS;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    Surface fluid[2], pressure[2], scalar;
    ComPtr<ID3D11Texture2D> obstacleTexture;
    ComPtr<ID3D11ShaderResourceView> obstacleSrv;
    int current = 0, currentPressure = 0;
    bool ready = false, fluidValid = false;
    WindGrid grid;
    std::string gridKey;
    float low = 0, high = 0; // the map's heights, for the building mask
    // The building mask in the making: the top of each cell's open sky, a few rows a frame.
    std::vector<float> tops;
    int topsRow = 0;
    bool obstaclesDone = false;
    GroundFogClock clock;
    std::map<int, Vec3> pawns; // where each player was at the last step
    // The billow noise the gust volume is torn by (GustNoiseVolume), built on a worker when the
    // device starts so turning gusts on never waits for it.
    std::future<std::vector<uint8_t>> noiseJob;
    ComPtr<ID3D11ShaderResourceView> noiseSrv;
    ComPtr<ID3D11SamplerState> noiseSampler;
    StepConstants k{};
    unsigned idle = 0, steps = 0, restarts = 0, solidCells = 0;
} state;
std::atomic<bool> g_overlay{false};
std::atomic<float> g_overlaySpan{1600.f};

bool MakeSurface(DXGI_FORMAT format, Surface &out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(state.grid.width);
    d.Height = UINT(state.grid.height);
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(state.device->CreateTexture2D(&d, nullptr, &out.texture)) &&
           SUCCEEDED(state.device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv)) &&
           SUCCEEDED(state.device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.uav));
}
void ReleaseFluid() {
    for (auto &s : state.fluid) s = Surface{};
    for (auto &s : state.pressure) s = Surface{};
    state.scalar = Surface{};
    state.obstacleTexture.Reset();
    state.obstacleSrv.Reset();
    state.fluidValid = false;
    state.grid = WindGrid{};
    state.gridKey.clear();
    state.clock = GroundFogClock{};
    state.pawns.clear();
    state.tops.clear();
    state.topsRow = 0;
    state.obstaclesDone = false;
    state.solidCells = 0;
}
// The grid belongs to the map: it waits for the map's collision data and changes only with it.
bool PrepareGrid(const Frame &f) {
    const auto &mesh = f.occlusion;
    if (!mesh) return false;
    const std::string key = mesh->contentKey + "|" + std::to_string(mesh->positions.size());
    if (state.fluid[0].texture && key == state.gridKey) return true;
    ReleaseFluid();
    const WindGrid grid = WindGridForMap(mesh->low, mesh->high);
    if (!grid.Valid()) return false;
    state.grid = grid;
    state.low = mesh->low.z;
    state.high = mesh->high.z;
    RenderPrivateTextureScope privateTextures;
    const bool ok = MakeSurface(DXGI_FORMAT_R16G16B16A16_FLOAT, state.fluid[0]) && MakeSurface(DXGI_FORMAT_R16G16B16A16_FLOAT, state.fluid[1]) &&
                    MakeSurface(DXGI_FORMAT_R32_FLOAT, state.pressure[0]) && MakeSurface(DXGI_FORMAT_R32_FLOAT, state.pressure[1]) &&
                    MakeSurface(DXGI_FORMAT_R32_FLOAT, state.scalar);
    if (!ok) {
        MvmDebugLog_LinefAlways("environment.wind", "could not allocate the fluid surfaces (%dx%d)", grid.width, grid.height);
        ReleaseFluid();
        return false;
    }
    state.gridKey = key;
    MvmDebugLog_LinefAlways("environment.wind", "grid cells=%dx%d cell=%.1f origin=(%.0f %.0f)", grid.width, grid.height, grid.cell, grid.originX,
                            grid.originY);
    return true;
}
// THE BUILDINGS, from the weather cover volume (how open to the sky each point of the map is): each
// cell's column is read from the top down to where the sky is shut off, a few rows a frame so no
// frame pays for the whole map, and WindObstacles turns those tops into the mask. Until the cover
// is baked and the mask is done the wind blows through everything.
void PumpObstacles(const Frame &f) {
    if (state.obstaclesDone || !state.grid.Valid()) return;
    const auto &grid = state.grid;
    if (state.tops.empty()) {
        if (!WeatherCoverReady()) return;
        state.tops.assign(size_t(grid.width) * grid.height, state.low);
        state.topsRow = 0;
    }
    for (int n = 0; n < kObstacleRows && state.topsRow < grid.height; ++n, ++state.topsRow)
        for (int x = 0; x < grid.width; ++x)
            state.tops[size_t(state.topsRow) * grid.width + x] = WeatherCoverTop(grid.CentreX(x), grid.CentreY(state.topsRow), state.low, state.high);
    if (state.topsRow < grid.height) return;
    const std::vector<uint8_t> solid = WindAirMap(state.tops, grid.width, grid.height, grid.cell);
    state.tops.clear();
    state.tops.shrink_to_fit();
    state.obstaclesDone = true;
    for (uint8_t s : solid) state.solidCells += s > 127 ? 1 : 0;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(grid.width);
    d.Height = UINT(grid.height);
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{solid.data(), UINT(grid.width), 0};
    RenderPrivateTextureScope privateTextures;
    if (FAILED(state.device->CreateTexture2D(&d, &data, &state.obstacleTexture)) ||
        FAILED(state.device->CreateShaderResourceView(state.obstacleTexture.Get(), nullptr, &state.obstacleSrv))) {
        state.obstacleTexture.Reset();
        state.obstacleSrv.Reset();
    }
    MvmDebugLog_LinefAlways("environment.wind", "buildings: %u of %d cells solid, mask %s", state.solidCells, grid.width * grid.height,
                            state.obstacleSrv ? "bound" : "FAILED");
}
void Dispatch(ID3D11DeviceContext *ctx, Entry entry, ID3D11ShaderResourceView *in0, ID3D11ShaderResourceView *in1, ID3D11ShaderResourceView *in2,
              ID3D11UnorderedAccessView *out0, ID3D11UnorderedAccessView *out1) {
    ID3D11ShaderResourceView *views[4] = {in0, in1, in2, state.obstacleSrv.Get()};
    ID3D11UnorderedAccessView *outs[2] = {out0, out1};
    ctx->CSSetShader(state.shaders[entry].Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 4, views);
    ctx->CSSetUnorderedAccessViews(0, 2, outs, nullptr);
    ctx->Dispatch((UINT(state.grid.width) + 7) / 8, (UINT(state.grid.height) + 7) / 8, 1);
    // Unbind, so the next pass may read what this one wrote.
    ID3D11ShaderResourceView *noViews[4] = {};
    ID3D11UnorderedAccessView *noOuts[2] = {};
    ctx->CSSetShaderResources(0, 4, noViews);
    ctx->CSSetUnorderedAccessViews(0, 2, noOuts, nullptr);
}
bool Compile(const char *entry, const char *profile, ComPtr<ID3DBlob> &code) {
    ComPtr<ID3DBlob> error;
    if (SUCCEEDED(CompileEnvironmentShader(WindFluidShader, sizeof(WindFluidShader) - 1, "EnvironmentWindFluid", nullptr, entry, profile,
                                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &error)))
        return true;
    MvmDebugLog_LinefAlways("environment.wind", "compile=%s error=%s", entry, error ? static_cast<const char *>(error->GetBufferPointer()) : "compiler unavailable");
    return false;
}
} // namespace

constexpr int kNoiseSize = 64;
void PumpNoise() {
    if (state.noiseSrv || !state.noiseJob.valid() || state.noiseJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::vector<uint8_t> voxels;
    try { voxels = state.noiseJob.get(); } catch (...) { voxels.clear(); }
    if (voxels.size() != size_t(kNoiseSize) * kNoiseSize * kNoiseSize || !state.device) return;
    D3D11_TEXTURE3D_DESC d{};
    d.Width = d.Height = d.Depth = UINT(kNoiseSize);
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{voxels.data(), UINT(kNoiseSize), UINT(kNoiseSize * kNoiseSize)};
    ComPtr<ID3D11Texture3D> texture;
    RenderPrivateTextureScope privateTextures;
    if (FAILED(state.device->CreateTexture3D(&d, &data, &texture)) || FAILED(state.device->CreateShaderResourceView(texture.Get(), nullptr, &state.noiseSrv)))
        state.noiseSrv.Reset();
    MvmDebugLog_LinefAlways("environment.wind", "gust noise %d^3 %s", kNoiseSize, state.noiseSrv ? "uploaded" : "FAILED");
}

void EndWindFluid() {
    if (state.noiseJob.valid()) state.noiseJob.wait();
    state = State{};
}
void BeginWindFluid(ID3D11Device *device) {
    EndWindFluid();
    if (!device) return;
    state.device = device;
    for (int i = 0; i < WindFluidShaderEntryCount; ++i) {
        ComPtr<ID3DBlob> code;
        if (!Compile(WindFluidShaderEntries[i], "cs_5_0", code) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &state.shaders[i])))
            return;
    }
    {
        // The overlay is a test view: without it the wind still runs.
        ComPtr<ID3DBlob> vs, ps;
        if (Compile("OverlayVS", "vs_5_0", vs) && Compile("OverlayPS", "ps_5_0", ps)) {
            device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &state.overlayVS);
            device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &state.overlayPS);
        }
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(StepConstants);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    state.ready = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &state.constants)) && SUCCEEDED(device->CreateSamplerState(&sd, &state.sampler));
    D3D11_SAMPLER_DESC wrap = sd;
    wrap.AddressU = wrap.AddressV = wrap.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    device->CreateSamplerState(&wrap, &state.noiseSampler);
    state.noiseJob = std::async(std::launch::async, [] { return GustNoiseVolume(kNoiseSize); });
}

WindFluidFrameInfo PrepareWindFluid(ID3D11DeviceContext *ctx, const Frame &f, const Climate &climate, bool precipitating) {
    WindFluidFrameInfo info;
    if (!state.ready || !ctx || climate[CL_enabled] <= 0 || climate[CL_gusts] <= 0 || !precipitating) {
        if (state.ready && ++state.idle == kIdleFrames) {
            ReleaseFluid();
            MvmDebugLog_LinefAlways("environment.wind", "released the fluid after %u frames off", kIdleFrames);
        }
        return info;
    }
    state.idle = 0;
    PumpNoise();
    if (!PrepareGrid(f)) return info;
    PumpObstacles(f);
    const auto &grid = state.grid;
    auto &k = state.k;
    k.gridInfo[0] = grid.originX;
    k.gridInfo[1] = grid.originY;
    k.gridInfo[2] = float(grid.width);
    k.gridInfo[3] = float(grid.height);
    k.stepInfo[1] = grid.cell;
    k.stepInfo[2] = float(std::fmod(f.time, 10000.0));
    k.cameraInfo[0] = f.camera.x;
    k.cameraInfo[1] = f.camera.y;
    k.bodyInfo[1] = state.obstacleSrv ? 1.f : 0.f;
    const auto &w = f.scene.weather;
    k.windInfo[0] = w.windX;
    k.windInfo[1] = w.windY;
    k.windInfo[2] = 220 + 1.5f * std::sqrt(w.windX * w.windX + w.windY * w.windY);

    const GroundFogAdvance advance = GroundFogTick(state.clock, f.time);
    ID3D11Buffer *buffer = state.constants.Get();
    ID3D11SamplerState *sampler = state.sampler.Get();
    GPU_PASS_SCOPE(ctx, "composite: wind fluid");
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    ctx->CSSetSamplers(0, 1, &sampler);
    // One step of the air: swirl, the wind's push, carry, then keep it from piling up.
    auto stepFluid = [&] {
        auto swap = [&] { state.current = 1 - state.current; };
        auto in = [&] { return state.fluid[state.current].srv.Get(); };
        auto out = [&] { return state.fluid[1 - state.current].uav.Get(); };
        Dispatch(ctx, Curl, in(), nullptr, nullptr, nullptr, state.scalar.uav.Get());
        Dispatch(ctx, Force, in(), state.scalar.srv.Get(), nullptr, out(), nullptr);
        swap();
        Dispatch(ctx, Advect, in(), nullptr, nullptr, out(), nullptr);
        swap();
        Dispatch(ctx, Divergence, in(), nullptr, nullptr, nullptr, state.scalar.uav.Get());
        for (int j = 0; j < kJacobiIterations; ++j) {
            const int next = 1 - state.currentPressure;
            Dispatch(ctx, Jacobi, nullptr, state.scalar.srv.Get(), state.pressure[state.currentPressure].srv.Get(), nullptr, state.pressure[next].uav.Get());
            state.currentPressure = next;
        }
        Dispatch(ctx, Project, in(), nullptr, state.pressure[state.currentPressure].srv.Get(), out(), nullptr);
        swap();
        ++state.steps;
    };
    if (!state.fluidValid || advance.reset) {
        // A new grid or a seek: the air starts at rest and is run kPrewarmSeconds ahead at once, up
        // to this frame's time, with no players in it.
        const float zero[4] = {0, 0, 0, 0};
        for (auto &s : state.fluid) ctx->ClearUnorderedAccessViewFloat(s.uav.Get(), zero);
        for (auto &p : state.pressure) ctx->ClearUnorderedAccessViewFloat(p.uav.Get(), zero);
        state.fluidValid = true;
        state.pawns.clear();
        ++state.restarts;
        const float dt = kPrewarmSeconds / kPrewarmSteps;
        k.bodyInfo[0] = 0;
        k.stepInfo[0] = dt;
        k.stepInfo[3] = 3.5f;
        k.windInfo[3] = 1.f - std::exp(-dt / 1.1f);
        k.bodyInfo[2] = std::exp(-dt / 3.f);
        for (int s = 0; s < kPrewarmSteps; ++s) {
            k.stepInfo[2] = float(std::fmod(f.time - kPrewarmSeconds + (s + 1) * dt, 10000.0));
            ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
            stepFluid();
        }
        k.stepInfo[2] = float(std::fmod(f.time, 10000.0));
        MvmDebugLog_LinefAlways("environment.wind", "restart time=%.4f prewarm=%d steps over %.1f s", f.time, kPrewarmSteps, kPrewarmSeconds);
    }
    info.originX = grid.originX;
    info.originY = grid.originY;
    info.perUnitX = 1.f / (float(grid.width) * grid.cell);
    info.perUnitY = 1.f / (float(grid.height) * grid.cell);
    if (advance.dt <= 0 || advance.steps <= 0) return info;

    // The players, and how fast each moves: the air round them is dragged along.
    std::map<int, Vec3> seen;
    int bodies = 0;
    for (const auto &a : f.actors) {
        seen[a.entity] = a.origin;
        const auto before = state.pawns.find(a.entity);
        if (before == state.pawns.end() || bodies >= kBodies) continue;
        const float vx = (a.origin.x - before->second.x) / advance.dt, vy = (a.origin.y - before->second.y) / advance.dt;
        if (vx * vx + vy * vy > 900.f * 900.f) continue; // a respawn or a teleport, not a run
        k.bodies[bodies][0] = a.origin.x;
        k.bodies[bodies][1] = a.origin.y;
        k.bodies[bodies][2] = (std::max)(kBodyRadius, grid.cell * 1.5f);
        k.bodyVelocity[bodies][0] = vx;
        k.bodyVelocity[bodies][1] = vy;
        ++bodies;
    }
    state.pawns.swap(seen);
    const float dt = advance.dt / float(advance.steps);
    k.stepInfo[0] = dt;
    k.stepInfo[3] = 3.5f;
    k.windInfo[3] = 1.f - std::exp(-dt / 1.1f);
    k.bodyInfo[0] = float(bodies);
    k.bodyInfo[2] = std::exp(-dt / 3.f);
    ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
    for (int s = 0; s < advance.steps; ++s) stepFluid();
    if (MvmDebugLog_Active()) {
        static unsigned logged = 0;
        if ((++logged % 120) == 1)
            MvmDebugLog_Linef("environment.wind", "time=%.4f dt=%.4f steps=%d bodies=%d restarts=%u grid=%dx%d cell=%.1f buildings=%d solid=%u wind=(%.0f %.0f) gusts=%.2f",
                              f.time, advance.dt, advance.steps, bodies, state.restarts, grid.width, grid.height, grid.cell, int(state.obstaclesDone), state.solidCells,
                              w.windX, w.windY, climate[CL_gusts]);
    }
    return info;
}

void BindWindFluid(ID3D11DeviceContext *ctx) {
    if (!ctx) return;
    ID3D11ShaderResourceView *view = state.fluidValid ? state.fluid[state.current].srv.Get() : nullptr;
    ID3D11SamplerState *sampler = state.sampler.Get();
    ctx->PSSetShaderResources(42, 1, &view);
    ctx->VSSetShaderResources(42, 1, &view);
    ctx->PSSetSamplers(8, 1, &sampler);
    ctx->VSSetSamplers(8, 1, &sampler);
    ID3D11ShaderResourceView *noise = state.noiseSrv.Get();
    ID3D11SamplerState *noiseSampler = state.noiseSampler.Get();
    ctx->PSSetShaderResources(49, 1, &noise);
    ctx->VSSetShaderResources(49, 1, &noise);
    ctx->PSSetSamplers(10, 1, &noiseSampler);
    ctx->VSSetSamplers(10, 1, &noiseSampler);
}
std::string WindFluidStatus() {
    if (!state.ready) return "wind: shaders unavailable";
    if (!state.fluidValid) return "wind: off";
    char text[200];
    std::snprintf(text, sizeof(text), "wind: %dx%d cells of %.0f u, %u steps, buildings %s (%u cells)", state.grid.width, state.grid.height, state.grid.cell,
                  state.steps, state.obstaclesDone ? "mapped" : "pending", state.solidCells);
    return text;
}
void SetWindFluidOverlay(bool on, float span) {
    g_overlay.store(on);
    if (on && span >= 0) g_overlaySpan.store(span);
}
bool WindFluidOverlay() { return g_overlay.load(); }
void DrawWindFluidOverlay(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target, unsigned width, unsigned height) {
    if (!g_overlay.load() || !ctx || !target || !state.fluidValid || !state.overlayVS || !state.overlayPS || !state.grid.Valid()) return;
    const float side = float(height) * .42f, span = g_overlaySpan.load();
    const float mapWidth = float(state.grid.width) * state.grid.cell, mapHeight = float(state.grid.height) * state.grid.cell;
    const float aspect = span > 0 ? 1.f : mapWidth / mapHeight;
    const float w = aspect >= 1 ? side : side * aspect, h = aspect >= 1 ? side / aspect : side;
    D3D11_VIEWPORT viewport{float(height) * .017f, float(height) * .3f + (side - h) * .5f, w, h, 0, 1};
    auto &k = state.k;
    k.overlayInfo[0] = span > 0 ? k.cameraInfo[0] : state.grid.originX + mapWidth * .5f;
    k.overlayInfo[1] = span > 0 ? k.cameraInfo[1] : state.grid.originY + mapHeight * .5f;
    k.overlayInfo[2] = span > 0 ? span : mapWidth;
    k.overlayInfo[3] = span > 0 ? span : mapHeight;
    ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
    ID3D11Buffer *buffer = state.constants.Get();
    ID3D11SamplerState *sampler = state.sampler.Get();
    ID3D11ShaderResourceView *views[4] = {state.fluid[state.current].srv.Get(), nullptr, nullptr, state.obstacleSrv.Get()};
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(state.overlayVS.Get(), nullptr, 0);
    ctx->PSSetShader(state.overlayPS.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &buffer);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->PSSetShaderResources(0, 4, views);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[4] = {};
    ctx->PSSetShaderResources(0, 4, none);
}
} // namespace Filmmaker::Environment
