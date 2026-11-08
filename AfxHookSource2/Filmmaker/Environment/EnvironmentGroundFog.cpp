#define NOMINMAX
#include "EnvironmentGroundFog.h"
#include "EnvironmentGroundFogMath.h"
#include "EnvironmentGroundFogShaders.h"
#include "EnvironmentShaderCompiler.h"
#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
// Pressure solve iterations per step. The pressure carries over between steps as a warm start, so
// 20 sweeps leave the wake visibly divergence-free; more only cost time (each is one full-grid pass).
constexpr int kJacobiIterations = 20;
// A player's footprint in the fluid: about a body's half-width plus the legs' swing.
constexpr float kBodyRadius = 22.f;
// Release the fluid and floor map after this many frames with ground fog off.
constexpr unsigned kIdleFrames = 300;

struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct StepConstants {
    float stepInfo[4], windInfo[4], gridInfo[4], bodyInfo[4];
    float bodies[kGroundFogBodies][4];
    float bodyVelocity[kGroundFogBodies][4];
    float cameraInfo[4], floorInfo[4], floorSize[4], restInfo[4], overlayInfo[4];
    float blasts[kGroundFogBlasts][4];
};
static_assert(sizeof(StepConstants) % 16 == 0, "D3D constant alignment");
enum Entry { Curl, Force, Advect, Divergence, Jacobi, Project, Init, Rebase, Smooth, FrontMask, FrontNearby }; // the order of GroundFogShaderEntries

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> shaders[GroundFogShaderEntryCount];
    ComPtr<ID3D11VertexShader> overlayVS;
    ComPtr<ID3D11PixelShader> overlayPS;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    Surface fluid[2], pressure[2], scalar, smooth;
    int current = 0, currentPressure = 0;
    bool ready = false;
    // The grid and everything simulated on it. Cleared together.
    GroundFogGrid grid;
    std::string gridKey;
    bool fluidValid = false;
    GroundFogClock clock;
    std::vector<GroundFogTrackedPawn> tracked;
    StepConstants k{}; // the last constants, so the overlay can show bodies while the demo is paused
    float coverage = 1; // the Coverage the field's resting pattern was laid down for
    // The floor map of the current map: built on a worker from the map collision, keyed by its
    // content so a reload of the same map reuses it.
    std::string floorKey, pendingKey;
    std::future<std::shared_ptr<GroundFogFloorMap>> floorJob;
    GroundFogFloorMap floorInfo; // dimensions only; the heights live in floorTexture
    ComPtr<ID3D11Texture2D> floorTexture, thicknessTexture;
    ComPtr<ID3D11ShaderResourceView> floorSrv, thicknessSrv;
    // What is in front of the fog (FrontMask): the HDR frame before CS2's transparent effects, the finished
    // one, and the mask they make. `frontWanted` is set by a frame with fog, so nothing runs without it.
    ComPtr<ID3D11Texture2D> frontBefore, frontAfter;
    ComPtr<ID3D11ShaderResourceView> frontBeforeSrv, frontAfterSrv;
    Surface frontMask, frontNearby;
    unsigned frontWidth = 0, frontHeight = 0, frontAge = 0;
    bool frontWanted = false, frontPending = false, frontValid = false;
    unsigned idle = 0;
    // For GroundFogStatus and the debug log.
    unsigned steps = 0, bodies = 0, restarts = 0;
} state;
std::atomic<bool> g_dumpRequested{false}, g_overlay{false};
std::atomic<float> g_overlaySpan{kGroundFogOverlaySpan};
FogSceneTargetSource g_sceneSource = nullptr;

bool MakeSurface(DXGI_FORMAT format, Surface &out, int divisor = 1) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(state.grid.width + divisor - 1) / divisor;
    d.Height = UINT(state.grid.height + divisor - 1) / divisor;
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
    state.smooth = Surface{};
    state.fluidValid = false;
    state.grid = GroundFogGrid{};
    state.gridKey.clear();
    state.clock = GroundFogClock{};
    state.tracked.clear();
}
// The grid belongs to the map. It only changes when the map's collision data arrives or changes.
bool PrepareGrid(const Frame &f) {
    const auto &mesh = f.occlusion;
    const std::string key = mesh ? mesh->contentKey + "|" + std::to_string(mesh->positions.size()) : std::string("camera");
    if (state.fluid[0].texture && key == state.gridKey) return true;
    ReleaseFluid();
    GroundFogGrid grid = mesh ? GroundFogGridForMap(mesh->low, mesh->high) : GroundFogGrid{};
    if (!grid.Valid()) grid = GroundFogGridAround(f.camera);
    if (!grid.Valid()) return false;
    state.grid = grid;
    RenderPrivateTextureScope privateTextures;
    const bool ok = MakeSurface(DXGI_FORMAT_R16G16B16A16_FLOAT, state.fluid[0]) &&
                    MakeSurface(DXGI_FORMAT_R16G16B16A16_FLOAT, state.fluid[1]) &&
                    MakeSurface(DXGI_FORMAT_R32_FLOAT, state.pressure[0]) &&
                    MakeSurface(DXGI_FORMAT_R32_FLOAT, state.pressure[1]) && MakeSurface(DXGI_FORMAT_R32_FLOAT, state.scalar) &&
                    MakeSurface(DXGI_FORMAT_R16_FLOAT, state.smooth, 4);
    if (!ok) {
        MvmDebugLog_LinefAlways("environment.groundfog", "could not allocate the fluid surfaces (%dx%d)", grid.width, grid.height);
        ReleaseFluid();
        return false;
    }
    state.gridKey = key;
    MvmDebugLog_LinefAlways("environment.groundfog", "grid cells=%dx%d cell=%.1f origin=(%.0f %.0f) source=%s", grid.width, grid.height,
                            grid.cell, grid.originX, grid.originY, mesh ? "map" : "camera");
    return true;
}
void ReleaseFront() {
    state.frontBefore.Reset();
    state.frontAfter.Reset();
    state.frontBeforeSrv.Reset();
    state.frontAfterSrv.Reset();
    state.frontMask = Surface{};
    state.frontNearby = Surface{};
    state.frontWanted = state.frontPending = state.frontValid = false;
}
void ReleaseFloorMap() {
    state.floorTexture.Reset();
    state.floorSrv.Reset();
    state.thicknessTexture.Reset();
    state.thicknessSrv.Reset();
    state.floorInfo = GroundFogFloorMap{};
    state.floorKey.clear();
}
// Starts the floor map for a new map and uploads a finished one. The build runs on a worker: on a
// large map it takes a few hundred milliseconds, which must never land on the render thread.
void PumpFloorMap(const Frame &f) {
    const auto mesh = f.occlusion;
    const std::string key = mesh ? mesh->contentKey + "|" + std::to_string(mesh->positions.size()) : std::string();
    if (state.floorJob.valid() &&
        state.floorJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::shared_ptr<GroundFogFloorMap> built;
        try { built = state.floorJob.get(); } catch (...) { built.reset(); }
        if (built && built->Valid() && state.pendingKey == key) {
            RenderPrivateTextureScope privateTextures;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = UINT(built->width);
            d.Height = UINT(built->height);
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{built->layers.data(), UINT(built->width * sizeof(float) * 4), 0};
            ComPtr<ID3D11Texture2D> texture, thicknessTexture;
            ComPtr<ID3D11ShaderResourceView> view, thicknessView;
            std::vector<unsigned char> held(built->thickness.size() * kGroundFogFloorLayers);
            for (size_t c = 0; c < built->thickness.size(); ++c)
                for (int l = 0; l < kGroundFogFloorLayers; ++l)
                    held[c * kGroundFogFloorLayers + l] = (unsigned char)(std::clamp(built->thickness[c][l], 0.f, 1.f) * 255.f + .5f);
            D3D11_TEXTURE2D_DESC td = d;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D11_SUBRESOURCE_DATA thicknessData{held.data(), UINT(built->width * kGroundFogFloorLayers), 0};
            if (SUCCEEDED(state.device->CreateTexture2D(&d, &data, &texture)) &&
                SUCCEEDED(state.device->CreateShaderResourceView(texture.Get(), nullptr, &view)) &&
                SUCCEEDED(state.device->CreateTexture2D(&td, &thicknessData, &thicknessTexture)) &&
                SUCCEEDED(state.device->CreateShaderResourceView(thicknessTexture.Get(), nullptr, &thicknessView))) {
                state.floorTexture = texture;
                state.floorSrv = view;
                state.thicknessTexture = thicknessTexture;
                state.thicknessSrv = thicknessView;
                state.floorInfo = *built;
                state.floorInfo.layers.clear();
                state.floorInfo.layers.shrink_to_fit();
                state.floorInfo.thickness.clear();
                state.floorInfo.thickness.shrink_to_fit();
                state.floorKey = key;
            }
        }
        state.pendingKey.clear();
    }
    if (key.empty()) {
        if (!state.floorKey.empty()) ReleaseFloorMap();
        return;
    }
    if (key == state.floorKey || key == state.pendingKey || state.floorJob.valid()) return;
    if (!state.floorKey.empty()) ReleaseFloorMap();
    state.pendingKey = key;
    state.floorJob = std::async(std::launch::async, [mesh]() {
        const auto started = std::chrono::steady_clock::now();
        auto map = std::make_shared<GroundFogFloorMap>(
            BuildGroundFogFloorMap(mesh->positions, mesh->indices, mesh->low, mesh->high, {}, mesh->navPositions, mesh->navIndices));
        size_t floors = 0, columns = 0;
        for (const auto &column : map->layers) {
            size_t n = 0;
            for (float h : column) n += h > kGroundFogNoFloor * .5f;
            floors += n;
            columns += n > 0;
        }
        MvmDebugLog_LinefAlways("environment.groundfog.floor",
            "texels=%dx%d cell=%.1f origin=(%.0f %.0f) columns=%zu floors=%zu triangles=%zu navTriangles=%zu ms=%lld", map->width,
            map->height, map->cell, map->originX, map->originY, columns, floors, mesh->indices.size() / 3, mesh->navIndices.size() / 3,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        return map;
    });
}

void Dispatch(ID3D11DeviceContext *ctx, Entry entry, ID3D11ShaderResourceView *in0, ID3D11ShaderResourceView *in1,
              ID3D11ShaderResourceView *in2, ID3D11UnorderedAccessView *out0, ID3D11UnorderedAccessView *out1) {
    ID3D11ShaderResourceView *views[3] = {in0, in1, in2};
    ID3D11UnorderedAccessView *outs[2] = {out0, out1};
    ctx->CSSetShader(state.shaders[entry].Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 3, views);
    ctx->CSSetUnorderedAccessViews(0, 2, outs, nullptr);
    const int divisor = entry == Smooth ? 4 : 1;
    ctx->Dispatch((UINT(state.grid.width + divisor - 1) / divisor + 7) / 8, (UINT(state.grid.height + divisor - 1) / divisor + 7) / 8, 1);
    // Unbind, so the next pass may read what this one wrote.
    ID3D11ShaderResourceView *noViews[3] = {};
    ID3D11UnorderedAccessView *noOuts[2] = {};
    ctx->CSSetShaderResources(0, 3, noViews);
    ctx->CSSetUnorderedAccessViews(0, 2, noOuts, nullptr);
}
template <class Shader>
bool CompileGroundFog(const char *entry, const char *profile, ComPtr<ID3DBlob> &code) {
    ComPtr<ID3DBlob> error;
    if (SUCCEEDED(CompileEnvironmentShader(GroundFogShader, sizeof(GroundFogShader) - 1, "EnvironmentGroundFog", nullptr, entry, profile,
                                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &error)))
        return true;
    MvmDebugLog_LinefAlways("environment.groundfog", "compile=%s error=%s", entry,
                            error ? static_cast<const char *>(error->GetBufferPointer()) : "compiler unavailable");
    return false;
}
float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else { // subnormal
            int e = -1;
            uint32_t m = mantissa;
            do { ++e; m <<= 1; } while (!(m & 0x400));
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3ff) << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000u | mantissa << 13;
    else bits = sign | (exponent + 112) << 23 | mantissa << 13;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
} // namespace

void EndGroundFog() {
    if (state.floorJob.valid()) {
        try { state.floorJob.wait(); } catch (...) {}
    }
    state = State{};
}

void BeginGroundFog(ID3D11Device *device) {
    EndGroundFog();
    if (!device) return;
    state.device = device;
    for (int i = 0; i < GroundFogShaderEntryCount; ++i) {
        ComPtr<ID3DBlob> code;
        if (!CompileGroundFog<ID3D11ComputeShader>(GroundFogShaderEntries[i], "cs_5_0", code) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &state.shaders[i])))
            return;
    }
    {
        // The overlay is a test view: without it the fog still runs.
        ComPtr<ID3DBlob> vs, ps;
        if (CompileGroundFog<ID3D11VertexShader>("OverlayVS", "vs_5_0", vs) && CompileGroundFog<ID3D11PixelShader>("OverlayPS", "ps_5_0", ps)) {
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
    state.ready = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &state.constants)) &&
                  SUCCEEDED(device->CreateSamplerState(&sd, &state.sampler));
}

GroundFogFrameInfo PrepareGroundFog(ID3D11DeviceContext *ctx, const Frame &f, const Climate &climate) {
    GroundFogFrameInfo info;
    if (!state.ready || !ctx || climate[CL_enabled] <= 0 || climate[CL_groundFog] <= 0) {
        if (state.ready && ++state.idle == kIdleFrames) {
            ReleaseFluid();
            ReleaseFloorMap();
            ReleaseFront();
            MvmDebugLog_LinefAlways("environment.groundfog", "released fluid and floor map after %u frames off", kIdleFrames);
        }
        return info;
    }
    state.idle = 0;
    info.active = true;
    PumpFloorMap(f);
    auto &k = state.k;
    k.floorInfo[3] = 0;
    if (state.floorSrv) {
        info.floorMap = true;
        info.floorX = state.floorInfo.originX;
        info.floorY = state.floorInfo.originY;
        info.floorCell = state.floorInfo.cell;
        info.floorWidth = float(state.floorInfo.width);
        info.floorHeight = float(state.floorInfo.height);
        k.floorInfo[0] = info.floorX;
        k.floorInfo[1] = info.floorY;
        k.floorInfo[2] = info.floorCell;
        k.floorInfo[3] = 1;
        k.floorSize[0] = info.floorWidth;
        k.floorSize[1] = info.floorHeight;
    }
    const float windAngle = Radians(climate[CL_groundFogWindAngle]);
    const float windX = std::cos(windAngle) * climate[CL_groundFogWind];
    const float windY = std::sin(windAngle) * climate[CL_groundFogWind];
    info.driftX = float(std::fmod(f.time * double(windX), 100000.0));
    info.driftY = float(std::fmod(f.time * double(windY), 100000.0));
    if (!PrepareGrid(f)) return info;

    const auto &grid = state.grid;
    info.windowX = grid.originX;
    info.windowY = grid.originY;
    info.windowWidth = float(grid.width) * grid.cell;
    info.windowHeight = float(grid.height) * grid.cell;
    k.gridInfo[0] = grid.originX;
    k.gridInfo[1] = grid.originY;
    k.gridInfo[2] = float(grid.width);
    k.gridInfo[3] = float(grid.height);
    k.stepInfo[1] = grid.cell;
    Vec3 forward, right, up;
    CameraBasis(f.angles, forward, right, up);
    const float flat = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    k.cameraInfo[0] = f.camera.x;
    k.cameraInfo[1] = f.camera.y;
    k.cameraInfo[2] = flat > .01f ? forward.x / flat : 1.f;
    k.cameraInfo[3] = flat > .01f ? forward.y / flat : 0.f;
    k.restInfo[0] = info.driftX;
    k.restInfo[1] = info.driftY;
    k.restInfo[2] = climate[CL_groundFogCoverage];
    k.windInfo[3] = float(std::fmod(f.time, 10000.0));

    const GroundFogAdvance advance = GroundFogTick(state.clock, f.time);
    ID3D11Buffer *buffer = state.constants.Get();
    ID3D11SamplerState *sampler = state.sampler.Get();
    GPU_PASS_SCOPE(ctx, "composite: ground fog fluid");
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    ctx->CSSetSamplers(0, 1, &sampler);
    bool changed = false;
    if (!state.fluidValid || advance.reset) {
        changed = true;
        // Only a new grid or a real seek gets here. Log it: a restart during plain playback is a bug.
        MvmDebugLog_LinefAlways("environment.groundfog", "restart reason=%s time=%.4f", !state.fluidValid ? "new grid" : "seek", f.time);
        const float zero[4] = {0, 0, 0, 0};
        ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
        Dispatch(ctx, Init, nullptr, nullptr, nullptr, state.fluid[state.current].uav.Get(), nullptr);
        for (auto &p : state.pressure) ctx->ClearUnorderedAccessViewFloat(p.uav.Get(), zero);
        state.fluidValid = true;
        state.tracked.clear();
        state.coverage = k.restInfo[2];
        ++state.restarts;
    }
    // Coverage is a slider people drag and watch, often on a paused frame: answer at once.
    if (std::abs(k.restInfo[2] - state.coverage) > 1e-4f) {
        k.restInfo[3] = state.coverage;
        ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
        Dispatch(ctx, Rebase, state.fluid[state.current].srv.Get(), nullptr, nullptr, state.fluid[1 - state.current].uav.Get(), nullptr);
        state.current = 1 - state.current;
        state.coverage = k.restInfo[2];
        changed = true;
    }
    info.fluid = true;
    const auto smoothField = [&] {
        Dispatch(ctx, Smooth, state.fluid[state.current].srv.Get(), nullptr, nullptr, nullptr, state.smooth.uav.Get());
    };
    if (advance.dt <= 0 || advance.steps <= 0) {
        if (changed) smoothField();
        return info;
    }

    std::vector<std::pair<int, Vec3>> pawns;
    for (const auto &a : f.actors) pawns.push_back({a.entity, a.origin});
    const auto bodies = GroundFogTrackBodies(state.tracked, pawns, advance.dt, kBodyRadius);
    state.bodies = unsigned(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
        k.bodies[i][0] = bodies[i].position.x;
        k.bodies[i][1] = bodies[i].position.y;
        // Never narrower than the grid can draw.
        k.bodies[i][2] = (std::max)(bodies[i].radius * climate[CL_groundFogPushSize], grid.cell * 1.25f);
        k.bodyVelocity[i][0] = bodies[i].vx;
        k.bodyVelocity[i][1] = bodies[i].vy;
    }
    const float dt = advance.dt / float(advance.steps);
    k.stepInfo[0] = dt;
    k.stepInfo[2] = 1.f - std::exp(-dt / (std::max)(climate[CL_groundFogRecovery], .05f));
    k.stepInfo[3] = climate[CL_groundFogSwirl] * 2.5f;
    k.windInfo[0] = windX;
    k.windInfo[1] = windY;
    k.windInfo[2] = 60.f * climate[CL_groundFogSwirl];
    k.bodyInfo[0] = float(bodies.size());
    k.bodyInfo[1] = climate[CL_groundFogFlow];
    int blasts = 0;
    for (const auto &b : f.fogBlasts) {
        if (blasts >= kGroundFogBlasts) break;
        k.blasts[blasts][0] = b.position.x;
        k.blasts[blasts][1] = b.position.y;
        k.blasts[blasts][2] = b.radius;
        k.blasts[blasts][3] = b.push;
        ++blasts;
    }
    k.bodyInfo[2] = float(blasts);
    ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
    for (int s = 0; s < advance.steps; ++s) {
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
            Dispatch(ctx, Jacobi, nullptr, state.scalar.srv.Get(), state.pressure[state.currentPressure].srv.Get(), nullptr,
                     state.pressure[next].uav.Get());
            state.currentPressure = next;
        }
        Dispatch(ctx, Project, in(), nullptr, state.pressure[state.currentPressure].srv.Get(), out(), nullptr);
        swap();
        ++state.steps;
    }
    smoothField();
    if (MvmDebugLog_Active()) {
        static unsigned logged = 0;
        if ((++logged % 120) == 1)
            MvmDebugLog_Linef("environment.groundfog",
                "time=%.4f dt=%.4f steps=%d bodies=%zu blasts=%d restarts=%u grid=%dx%d floorMap=%d wind=(%.1f %.1f) flow=%.2f refill=%.1fs swirl=%.2f",
                f.time, advance.dt, advance.steps, bodies.size(), blasts, state.restarts, grid.width, grid.height, int(info.floorMap), windX,
                windY, climate[CL_groundFogFlow], climate[CL_groundFogRecovery], climate[CL_groundFogSwirl]);
        static unsigned loggedBlast = 0;
        if (blasts > 0 && (++loggedBlast % 30) == 1)
            for (int i = 0; i < blasts; ++i)
                MvmDebugLog_Linef("environment.groundfog.blast", "time=%.3f %d/%d position=(%.1f %.1f) radius=%.0f push=%.0f", f.time, i + 1, blasts,
                                  k.blasts[i][0], k.blasts[i][1], k.blasts[i][2], k.blasts[i][3]);
    }
    return info;
}

void BindGroundFog(ID3D11DeviceContext *ctx) {
    if (!ctx) return;
    ID3D11ShaderResourceView *views[6] = {state.fluidValid ? state.fluid[state.current].srv.Get() : nullptr,
                                          state.floorSrv.Get(), state.fluidValid ? state.smooth.srv.Get() : nullptr,
                                          state.thicknessSrv.Get(), state.frontValid ? state.frontMask.srv.Get() : nullptr,
                                          state.frontValid ? state.frontNearby.srv.Get() : nullptr};
    ctx->PSSetShaderResources(35, 6, views);
    ID3D11SamplerState *sampler = state.sampler.Get();
    ctx->PSSetSamplers(6, 1, &sampler);
}

void SetFogSceneTargetSource(FogSceneTargetSource source) { g_sceneSource = source; }
void CaptureFogBeforeTransparents(ID3D11DeviceContext *immediate) {
    if (!state.ready || !immediate || !g_sceneSource || !state.frontWanted || !state.frontWidth) return;
    state.frontWanted = false;
    ComPtr<ID3D11Texture2D> scene;
    scene.Attach(g_sceneSource(state.frontWidth, state.frontHeight));
    if (!scene) return;
    D3D11_TEXTURE2D_DESC source{}, have{};
    scene->GetDesc(&source);
    if (state.frontBefore) state.frontBefore->GetDesc(&have);
    if (!state.frontBefore || have.Width != source.Width || have.Height != source.Height) {
        ReleaseFront();
        RenderPrivateTextureScope privateTextures;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = source.Width;
        d.Height = source.Height;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC m = d;
        m.Format = DXGI_FORMAT_R16G16_FLOAT;
        m.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_TEXTURE2D_DESC n = m;
        n.Width = (source.Width + 15) / 16;
        n.Height = (source.Height + 15) / 16;
        n.Format = DXGI_FORMAT_R16_FLOAT;
        if (FAILED(state.device->CreateTexture2D(&n, nullptr, &state.frontNearby.texture)) ||
            FAILED(state.device->CreateShaderResourceView(state.frontNearby.texture.Get(), nullptr, &state.frontNearby.srv)) ||
            FAILED(state.device->CreateUnorderedAccessView(state.frontNearby.texture.Get(), nullptr, &state.frontNearby.uav)) ||
            FAILED(state.device->CreateTexture2D(&d, nullptr, &state.frontBefore)) ||
            FAILED(state.device->CreateShaderResourceView(state.frontBefore.Get(), nullptr, &state.frontBeforeSrv)) ||
            FAILED(state.device->CreateTexture2D(&d, nullptr, &state.frontAfter)) ||
            FAILED(state.device->CreateShaderResourceView(state.frontAfter.Get(), nullptr, &state.frontAfterSrv)) ||
            FAILED(state.device->CreateTexture2D(&m, nullptr, &state.frontMask.texture)) ||
            FAILED(state.device->CreateShaderResourceView(state.frontMask.texture.Get(), nullptr, &state.frontMask.srv)) ||
            FAILED(state.device->CreateUnorderedAccessView(state.frontMask.texture.Get(), nullptr, &state.frontMask.uav))) {
            MvmDebugLog_LinefAlways("environment.groundfog", "could not allocate the front mask (%ux%u)", source.Width, source.Height);
            ReleaseFront();
            return;
        }
    }
    if (source.SampleDesc.Count > 1) immediate->ResolveSubresource(state.frontBefore.Get(), 0, scene.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    else immediate->CopyResource(state.frontBefore.Get(), scene.Get());
    state.frontPending = true;
}
bool PrepareFogFrontMask(ID3D11DeviceContext *ctx, unsigned width, unsigned height) {
    state.frontWidth = width;
    state.frontHeight = height;
    state.frontWanted = true;
    if (!state.ready || !ctx || !g_sceneSource) {
        state.frontValid = false;
        return false;
    }
    if (!state.frontPending) {
        // Two renders can share a frame (HDR and display). A mask with no fresh capture goes stale.
        if (++state.frontAge > 4) state.frontValid = false;
        return state.frontValid;
    }
    state.frontPending = false;
    state.frontValid = false;
    ComPtr<ID3D11Texture2D> scene;
    scene.Attach(g_sceneSource(width, height));
    if (!scene) return false;
    D3D11_TEXTURE2D_DESC source{}, have{};
    scene->GetDesc(&source);
    state.frontBefore->GetDesc(&have);
    if (have.Width != source.Width || have.Height != source.Height) return false;
    GPU_PASS_SCOPE(ctx, "composite: fog front mask");
    if (source.SampleDesc.Count > 1) ctx->ResolveSubresource(state.frontAfter.Get(), 0, scene.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    else ctx->CopyResource(state.frontAfter.Get(), scene.Get());
    ID3D11ShaderResourceView *after = state.frontAfterSrv.Get(), *before = state.frontBeforeSrv.Get();
    ID3D11UnorderedAccessView *mask = state.frontMask.uav.Get();
    ctx->CSSetShader(state.shaders[FrontMask].Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &after);
    ctx->CSSetShaderResources(4, 1, &before);
    ctx->CSSetUnorderedAccessViews(0, 1, &mask, nullptr);
    ctx->Dispatch((source.Width + 7) / 8, (source.Height + 7) / 8, 1);
    ID3D11ShaderResourceView *noView = nullptr;
    ID3D11UnorderedAccessView *noMask = nullptr;
    ctx->CSSetShaderResources(0, 1, &noView);
    ctx->CSSetShaderResources(4, 1, &noView);
    ctx->CSSetUnorderedAccessViews(0, 1, &noMask, nullptr);
    ID3D11ShaderResourceView *shares = state.frontMask.srv.Get();
    ID3D11UnorderedAccessView *nearby = state.frontNearby.uav.Get();
    ctx->CSSetShader(state.shaders[FrontNearby].Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &shares);
    ctx->CSSetUnorderedAccessViews(1, 1, &nearby, nullptr);
    ctx->Dispatch(((source.Width + 15) / 16 + 7) / 8, ((source.Height + 15) / 16 + 7) / 8, 1);
    ctx->CSSetShaderResources(0, 1, &noView);
    ctx->CSSetUnorderedAccessViews(1, 1, &noMask, nullptr);
    state.frontAge = 0;
    state.frontValid = true;
    return true;
}

void SetGroundFogOverlay(bool on, float span) {
    g_overlay.store(on);
    if (on && span >= 0) g_overlaySpan.store(span);
}
bool GroundFogOverlay() { return g_overlay.load(); }
void DrawGroundFogOverlay(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target, unsigned width, unsigned height) {
    if (!g_overlay.load() || !ctx || !target || !state.fluidValid || !state.overlayVS || !state.overlayPS || !state.grid.Valid())
        return;
    // On the right, under the camera readout and above the demo bar. Around the camera at a set
    // width, like a radar; a span of 0 fits the whole map, in the map's own proportions.
    const float side = float(height) * .42f, span = g_overlaySpan.load();
    const float mapWidth = float(state.grid.width) * state.grid.cell, mapHeight = float(state.grid.height) * state.grid.cell;
    const float aspect = span > 0 ? 1.f : mapWidth / mapHeight;
    const float w = aspect >= 1 ? side : side * aspect, h = aspect >= 1 ? side / aspect : side;
    D3D11_VIEWPORT viewport{float(width) - w - float(height) * .017f, float(height) * .47f + (side - h) * .5f, w, h, 0, 1};
    auto &k = state.k;
    k.overlayInfo[0] = span > 0 ? k.cameraInfo[0] : state.grid.originX + mapWidth * .5f;
    k.overlayInfo[1] = span > 0 ? k.cameraInfo[1] : state.grid.originY + mapHeight * .5f;
    k.overlayInfo[2] = span > 0 ? span : mapWidth;
    k.overlayInfo[3] = span > 0 ? span : mapHeight;
    ctx->UpdateSubresource(state.constants.Get(), 0, nullptr, &k, 0, 0);
    ID3D11Buffer *buffer = state.constants.Get();
    ID3D11SamplerState *sampler = state.sampler.Get();
    ID3D11ShaderResourceView *views[4] = {state.fluid[state.current].srv.Get(), nullptr, nullptr, state.floorSrv.Get()};
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

void RequestGroundFogDump() { g_dumpRequested.store(true); }
void DumpGroundFogIfRequested(ID3D11DeviceContext *immediate) {
    if (!g_dumpRequested.exchange(false)) return;
    if (!immediate || !state.fluidValid || !state.fluid[state.current].texture) {
        MvmDebugLog_LinefAlways("environment.groundfog.dump", "nothing to dump: ground fog is off or has not stepped yet");
        return;
    }
    RenderPrivateTextureScope privateTextures;
    D3D11_TEXTURE2D_DESC d{};
    state.fluid[state.current].texture->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(state.device->CreateTexture2D(&d, nullptr, &staging))) return;
    immediate->CopyResource(staging.Get(), state.fluid[state.current].texture.Get());
    if (FAILED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    const int w = state.grid.width, h = state.grid.height;
    const int stride = (w * 3 + 3) & ~3; // BMP rows are padded to four bytes
    std::vector<unsigned char> pixels(size_t(stride) * h);
    double sum = 0;
    float lowest = 1e9f, highest = -1e9f, fastest = 0;
    int cleared = 0, heaped = 0;
    for (int y = 0; y < h; ++y) {
        const auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(mapped.pData) + size_t(y) * mapped.RowPitch);
        for (int x = 0; x < w; ++x) {
            const float vx = HalfToFloat(row[x * 4]), vy = HalfToFloat(row[x * 4 + 1]), fog = HalfToFloat(row[x * 4 + 2]);
            sum += fog;
            lowest = (std::min)(lowest, fog);
            highest = (std::max)(highest, fog);
            fastest = (std::max)(fastest, std::sqrt(vx * vx + vy * vy));
            cleared += fog < .6f;
            heaped += fog > 1.1f;
            const unsigned char grey = (unsigned char)std::clamp(fog / 1.3f * 255.f, 0.f, 255.f);
            // BMP rows run bottom-up: texel row 0 (the grid's south edge) lands at the bottom.
            unsigned char *p = &pixels[size_t(y) * stride + size_t(x) * 3];
            p[0] = p[1] = p[2] = grey;
        }
    }
    immediate->Unmap(staging.Get(), 0);
    const wchar_t *appData = _wgetenv(L"APPDATA");
    std::wstring path = std::wstring(appData ? appData : L".") + L"/HLAE/debuglogs/groundfog_" +
                        std::to_wstring(GetTickCount64()) + L".bmp";
    std::ofstream out(path, std::ios::binary);
    const uint32_t imageBytes = uint32_t(pixels.size()), fileBytes = 54 + imageBytes;
    unsigned char header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = (unsigned char)(v >> (8 * i)); };
    put32(2, fileBytes); put32(10, 54); put32(14, 40); put32(18, uint32_t(w)); put32(22, uint32_t(h));
    header[26] = 1; header[28] = 24; put32(34, imageBytes);
    out.write(reinterpret_cast<const char *>(header), 54);
    out.write(reinterpret_cast<const char *>(pixels.data()), std::streamsize(pixels.size()));
    // The effects mask too, when there is one: red is light an effect added, green what it hid.
    if (state.frontValid && state.frontMask.texture) {
        D3D11_TEXTURE2D_DESC md{};
        state.frontMask.texture->GetDesc(&md);
        md.Usage = D3D11_USAGE_STAGING;
        md.BindFlags = 0;
        md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        md.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> maskStaging;
        D3D11_MAPPED_SUBRESOURCE maskMap{};
        if (SUCCEEDED(state.device->CreateTexture2D(&md, nullptr, &maskStaging))) {
            immediate->CopyResource(maskStaging.Get(), state.frontMask.texture.Get());
            if (SUCCEEDED(immediate->Map(maskStaging.Get(), 0, D3D11_MAP_READ, 0, &maskMap))) {
                const int mw = int(md.Width), mh = int(md.Height), maskStride = (mw * 3 + 3) & ~3;
                std::vector<unsigned char> maskPixels(size_t(maskStride) * mh);
                double lightSum = 0, darkSum = 0;
                for (int y = 0; y < mh; ++y) {
                    const auto *row = reinterpret_cast<const uint16_t *>(static_cast<const char *>(maskMap.pData) + size_t(y) * maskMap.RowPitch);
                    for (int x = 0; x < mw; ++x) {
                        const float light = HalfToFloat(row[x * 2]), dark = HalfToFloat(row[x * 2 + 1]);
                        lightSum += light;
                        darkSum += dark;
                        // Screen rows run top-down and BMP rows bottom-up.
                        unsigned char *p = &maskPixels[size_t(mh - 1 - y) * maskStride + size_t(x) * 3];
                        p[0] = 0;
                        p[1] = (unsigned char)std::clamp(dark * 255.f, 0.f, 255.f);
                        p[2] = (unsigned char)std::clamp(light * 255.f, 0.f, 255.f);
                    }
                }
                immediate->Unmap(maskStaging.Get(), 0);
                std::wstring maskPath = path;
                maskPath.insert(maskPath.rfind(L"groundfog_") + 9, L"_mask");
                std::ofstream maskOut(maskPath, std::ios::binary);
                const uint32_t maskBytes = uint32_t(maskPixels.size());
                put32(2, 54 + maskBytes); put32(18, uint32_t(mw)); put32(22, uint32_t(mh)); put32(34, maskBytes);
                maskOut.write(reinterpret_cast<const char *>(header), 54);
                maskOut.write(reinterpret_cast<const char *>(maskPixels.data()), std::streamsize(maskPixels.size()));
                MvmDebugLog_LinefAlways("environment.groundfog.dump", "mask=%ls written=%d light=%.4f dark=%.4f", maskPath.c_str(), int(bool(maskOut)), lightSum / (double(mw) * mh), darkSum / (double(mw) * mh));
            }
        }
    }
    MvmDebugLog_LinefAlways("environment.groundfog.dump",
        "file=%ls written=%d grid=%dx%d fog min=%.3f mean=%.3f max=%.3f cleared(<0.6)=%d heaped(>1.1)=%d fastest=%.1f u/s steps=%u bodies=%u restarts=%u",
        path.c_str(), int(bool(out)), w, h, lowest, sum / (double(w) * h), highest, cleared, heaped, fastest, state.steps, state.bodies, state.restarts);
}

std::string GroundFogStatus() {
    char text[256];
    std::snprintf(text, sizeof(text), "groundfog: ready=%d fluid=%d grid=%dx%d floorMap=%s steps=%u bodies=%u restarts=%u overlay=%d",
                  int(state.ready), int(state.fluidValid), state.grid.width, state.grid.height,
                  state.floorSrv ? (std::to_string(state.floorInfo.width) + "x" + std::to_string(state.floorInfo.height)).c_str()
                                 : (state.floorJob.valid() ? "building" : "none"),
                  state.steps, state.bodies, state.restarts, int(g_overlay.load()));
    return text;
}
} // namespace Filmmaker::Environment
