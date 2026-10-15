#define NOMINMAX
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentGi.h"
#include "EnvironmentGiMath.h"
#include "EnvironmentGiBuildShaders.h"
#include "EnvironmentShadowMaps.h"
#include "../Debug/MvmDebugLog.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <cstring>
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};
Float4 Pack(Vec3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
struct BuildConstants {
    Float4 lowSize, info, sunDirection, sunRadiance;
    Float4 positions[16], colors[16], directions[16];
    Float4 right[16], up[16], studio[16], patternTime[16];
};
struct TraceConstants {
    Float4 lowSize, info;
};
struct DrawConstants {
    Matrix model;
    Float4 albedo;
};
struct GpuMesh {
    std::shared_ptr<const Mesh> source;
    ComPtr<ID3D11Buffer> vertices;
    uint64_t contentKey=0;
};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> inject;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> build, draw, trace, vertices, indices;
    ComPtr<ID3D11Texture3D> voxels, radiance, directions, emission;
    ComPtr<ID3D11ShaderResourceView> voxelsSrv, radianceSrv, directionsSrv, emissionSrv;
    ComPtr<ID3D11UnorderedAccessView> voxelsUav, radianceUav, directionsUav, emissionUav;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> sampler;
    std::shared_ptr<const OcclusionMesh> world;
    std::map<const Mesh *, GpuMesh> props;
    uint64_t geometryKey = 0, lightingKey = 0, worldKey = 0;
    bool ready = false, attempted = false;
} state;
uint64_t Hash(uint64_t h, const void *p, size_t count) {
    auto b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < count; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
bool Buffer(UINT size, UINT bind, ComPtr<ID3D11Buffer> &out, const void *data = nullptr) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.BindFlags = bind;
    d.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    return SUCCEEDED(state.device->CreateBuffer(&d, data ? &init : nullptr, &out));
}
bool Compile(const char *entry, const char *profile, ComPtr<ID3DBlob> &code) {
    static HMODULE module = EnvironmentShaderCompiler();
    auto compile = module ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    ComPtr<ID3DBlob> errors;
    if (!compile ||
        FAILED(CompileEnvironmentShader(kGiShader, strlen(kGiShader), "EnvironmentWorldGi", nullptr, entry, profile,
                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, &code, &errors))) {
        MvmDebugLog_LinefAlways("environment.gi", "shader=%s failure=%s", entry,
                                errors ? static_cast<const char *>(errors->GetBufferPointer())
                                       : "compiler missing");
        return false;
    }
    return true;
}
bool Initialize() {
    if (state.ready)
        return true;
    if (state.attempted || !state.device)
        return false;
    state.attempted = true;
    MvmDebugLog_LinefAlways("environment.gi", "initializing resolution=%u oneBounce=1 cameraIndependent=1",
                            GiResolution);
    ComPtr<ID3DBlob> vs, gs, ps, cs;
    if (!Compile("VoxelVS", "vs_5_0", vs) || !Compile("VoxelGS", "gs_5_0", gs) ||
        !Compile("VoxelPS", "ps_5_0", ps) || !Compile("InjectCS", "cs_5_0", cs))
        return false;
    D3D11_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(state.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                                &state.vs)) ||
        FAILED(state.device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr,
                                                  &state.gs)) ||
        FAILED(state.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                                               &state.ps)) ||
        FAILED(state.device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr,
                                                 &state.inject)) ||
        FAILED(state.device->CreateInputLayout(&element, 1, vs->GetBufferPointer(), vs->GetBufferSize(),
                                               &state.layout)))
        return false;
    if (!Buffer(sizeof(BuildConstants), D3D11_BIND_CONSTANT_BUFFER, state.build) ||
        !Buffer(sizeof(DrawConstants), D3D11_BIND_CONSTANT_BUFFER, state.draw) ||
        !Buffer(sizeof(TraceConstants), D3D11_BIND_CONSTANT_BUFFER, state.trace))
        return false;
    D3D11_TEXTURE3D_DESC d{};
    d.Width = d.Height = d.Depth = GiResolution;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_UINT;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(state.device->CreateTexture3D(&d, nullptr, &state.voxels)) ||
        FAILED(state.device->CreateShaderResourceView(state.voxels.Get(), nullptr, &state.voxelsSrv)) ||
        FAILED(state.device->CreateUnorderedAccessView(state.voxels.Get(), nullptr, &state.voxelsUav)))
        return false;
    if(FAILED(state.device->CreateTexture3D(&d,nullptr,&state.emission))||
       FAILED(state.device->CreateShaderResourceView(state.emission.Get(),nullptr,&state.emissionSrv))||
       FAILED(state.device->CreateUnorderedAccessView(state.emission.Get(),nullptr,&state.emissionUav)))return false;
    d.MipLevels = 0;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(state.device->CreateTexture3D(&d, nullptr, &state.radiance)) ||
        FAILED(state.device->CreateShaderResourceView(state.radiance.Get(), nullptr, &state.radianceSrv)))
        return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = d.Format;
    u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    u.Texture3D.WSize = GiResolution;
    if (FAILED(state.device->CreateUnorderedAccessView(state.radiance.Get(), &u, &state.radianceUav)))
        return false;
    if (FAILED(state.device->CreateTexture3D(&d, nullptr, &state.directions)) ||
        FAILED(
            state.device->CreateShaderResourceView(state.directions.Get(), nullptr, &state.directionsSrv)) ||
        FAILED(state.device->CreateUnorderedAccessView(state.directions.Get(), &u, &state.directionsUav)))
        return false;
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = FALSE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(state.device->CreateRasterizerState(&r, &state.raster)) ||
        FAILED(state.device->CreateDepthStencilState(&depth, &state.depth)) ||
        FAILED(state.device->CreateSamplerState(&sampler, &state.sampler)))
        return false;
    state.ready = true;
    return true;
}
} // namespace
void EndGi() { state = State{}; }
void BeginGi(ID3D11Device *device) {
    EndGi();
    state.device = device;
}
void PrepareGi(ID3D11DeviceContext *ctx, const Frame &f) {
    TraceConstants trace{};
    if (state.trace)
        ctx->UpdateSubresource(state.trace.Get(), 0, nullptr, &trace, 0, 0);
    if (f.scene.climate[CL_worldGi] < .5f || !f.occlusion) {
        if (state.trace)
            ctx->UpdateSubresource(state.trace.Get(), 0, nullptr, &trace, 0, 0);
        return;
    }
    if (!Initialize())
        return;
    auto c = EffectiveClimate(f.scene.climate);
    auto bounds = ClimateGiBounds(c);
    BuildConstants build{};
    build.lowSize = Pack(bounds.low, bounds.size);
    build.info = {float(GiResolution), bounds.size / GiResolution, 0,
                  c[CL_enabled] > .5f && c[CL_contactShadows] > .5f ? 1.f : 0.f};
    build.sunDirection = Pack(ClimateSunDirection(c));
    build.sunRadiance = Pack(ClimateSunRadiance(c));
    int light = 0;
    for (const auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && light < 16) {
            build.positions[light] = Pack(o.position, o.range);
            build.colors[light] = Pack(o.color, o.intensity * StudioCue(o.studio, f.time));
            Vec3 direction, r, u;
            CameraBasis(o.angles, direction, r, u);
            build.directions[light] =
                Pack(direction, o.kind != "point" ? std::cos(Radians(o.cone * .5f)) : -1);
            build.right[light]=Pack(r,o.studio.width); build.up[light]=Pack(u,o.studio.height);
            build.studio[light]={o.kind=="area"?1.f:0.f,float(o.studio.pattern),float(o.studio.profile),o.studio.patternScale};
            build.patternTime[light].x=o.studio.pattern==4?float(std::fmod(f.time*o.studio.rate+o.studio.phase,6283.185307)):0;
            build.patternTime[light].w = o.shadows ? 1.f : 0.f; // visibility mode invalidates radiance
            ++light;
        }
    build.info.z = float(light);
    if (state.world != f.occlusion) {
        state.world.reset();
        state.vertices.Reset();
        state.indices.Reset();
        state.geometryKey = state.lightingKey = 0;
        if (!Buffer(UINT(f.occlusion->positions.size() * sizeof(Vec3)), D3D11_BIND_VERTEX_BUFFER,
                    state.vertices, f.occlusion->positions.data()) ||
            !Buffer(UINT(f.occlusion->indices.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER,
                    state.indices, f.occlusion->indices.data()))
            return;
        state.world = f.occlusion;
        state.worldKey=Hash(1469598103934665603ull,state.world->contentKey.data(),state.world->contentKey.size());
        // Synthetic/test geometry can lack a manifest; content still identifies it.
        state.worldKey=Hash(state.worldKey,state.world->positions.data(),state.world->positions.size()*sizeof(Vec3));
        state.worldKey=Hash(state.worldKey,state.world->indices.data(),state.world->indices.size()*sizeof(uint32_t));
    }
    uint64_t geometry = 1469598103934665603ull;
    geometry = Hash(geometry, &state.worldKey, sizeof(state.worldKey));
    geometry = Hash(geometry, &build.lowSize, sizeof(build.lowSize));
    geometry = Hash(geometry, &c[CL_bounceAlbedo], sizeof(float));
    std::set<const Mesh *> used;
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight() && o.mesh) {
            auto mesh = o.mesh.get();
            used.insert(mesh);
            auto model = ModelMatrix(o.position, o.angles, o.scale);

            geometry = Hash(geometry, model.data(), sizeof(model));
            geometry = Hash(geometry, &o.color, sizeof(o.color));
            float emission=o.material.enabled?o.material.emission:0;geometry=Hash(geometry,&emission,sizeof(emission));
            if (!state.props.count(mesh)) {
                GpuMesh gpu;
                gpu.source = o.mesh;
                gpu.contentKey=Hash(1469598103934665603ull,o.mesh->vertices.data(),o.mesh->vertices.size()*sizeof(Vertex));
                if (!Buffer(UINT(o.mesh->vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER,
                            gpu.vertices, o.mesh->vertices.data()))
                    return;
                state.props.emplace(mesh, std::move(gpu));
            }
            geometry=Hash(geometry,&state.props.at(mesh).contentKey,sizeof(uint64_t));
        }
    for (auto it = state.props.begin(); it != state.props.end();)
        if (!used.count(it->first))
            it = state.props.erase(it);
        else
            ++it;
    ctx->UpdateSubresource(state.build.Get(), 0, nullptr, &build, 0, 0);
    bool geometryChanged = state.geometryKey != geometry;
    if (geometryChanged) {
        const UINT clear[4]{};
        ctx->ClearUnorderedAccessViewUint(state.voxelsUav.Get(), clear);
        ctx->ClearUnorderedAccessViewUint(state.emissionUav.Get(),clear);
        ID3D11UnorderedAccessView *uav[]={state.voxelsUav.Get(),nullptr,nullptr,state.emissionUav.Get()};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 4, uav, nullptr);
        ctx->OMSetDepthStencilState(state.depth.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, ~0u);
        D3D11_VIEWPORT viewport{0, 0, float(GiResolution), float(GiResolution), 0, 1};
        ctx->RSSetViewports(1, &viewport);
        ctx->RSSetState(state.raster.Get());
        ctx->IASetInputLayout(state.layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(state.vs.Get(), nullptr, 0);
        ctx->GSSetShader(state.gs.Get(), nullptr, 0);
        ctx->PSSetShader(state.ps.Get(), nullptr, 0);
        ID3D11Buffer *cb[] = {state.build.Get(), state.draw.Get()};
        ctx->VSSetConstantBuffers(0, 2, cb);
        ctx->GSSetConstantBuffers(0, 2, cb);
        ctx->PSSetConstantBuffers(0, 2, cb);
        DrawConstants draw{Identity(), {c[CL_bounceAlbedo], c[CL_bounceAlbedo], c[CL_bounceAlbedo], 0}};
        ctx->UpdateSubresource(state.draw.Get(), 0, nullptr, &draw, 0, 0);
        UINT stride = sizeof(Vec3), offset = 0;
        auto vb = state.vertices.Get();
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetIndexBuffer(state.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(UINT(state.world->indices.size()), 0, 0);
        ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        stride = sizeof(Vertex);
        for (const auto &o : f.scene.objects)
            if (o.enabled && !o.IsLight() && o.mesh) {
                draw = {ModelMatrix(o.position, o.angles, o.scale), Pack(o.color, o.material.enabled?o.material.emission:0)};
                ctx->UpdateSubresource(state.draw.Get(), 0, nullptr, &draw, 0, 0);
                vb = state.props.at(o.mesh.get()).vertices.Get();
                ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                ctx->Draw(UINT(o.mesh->vertices.size()), 0);
            }
        ID3D11UnorderedAccessView *none[4]{};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 4, none, nullptr);
        ctx->GSSetShader(nullptr, nullptr, 0);
        state.geometryKey = geometry;
    }
    auto lighting = Hash(geometry, &build, sizeof(build));
    lighting = Hash(lighting, &c[CL_shadowBias], sizeof(float));
    if (geometryChanged || state.lightingKey != lighting) {
        auto cb = state.build.Get();
        ctx->CSSetConstantBuffers(0, 1, &cb);
        BindShadowMaps(ctx, true);
        auto srv = state.voxelsSrv.Get();
        ctx->CSSetShaderResources(0, 1, &srv);
        auto emissionSrv=state.emissionSrv.Get();ctx->CSSetShaderResources(4,1,&emissionSrv);
        ID3D11UnorderedAccessView *uavs[] = {state.radianceUav.Get(), state.directionsUav.Get()};
        ctx->CSSetUnorderedAccessViews(1, 2, uavs, nullptr);
        ctx->CSSetShader(state.inject.Get(), nullptr, 0);
        ctx->Dispatch(GiResolution / 4, GiResolution / 4, GiResolution / 4);
        ID3D11UnorderedAccessView *none[2]{};
        ctx->CSSetUnorderedAccessViews(1, 2, none, nullptr);
        ID3D11ShaderResourceView *nullSrv=nullptr;ctx->CSSetShaderResources(4,1,&nullSrv);ctx->CSSetShaderResources(0,1,&nullSrv);
        ctx->GenerateMips(state.radianceSrv.Get());
        ctx->GenerateMips(state.directionsSrv.Get());
        state.lightingKey = lighting;
        static ULONGLONG lastLog = 0;
        auto now = GetTickCount64();
        if (MvmDebugLog_Active() && (geometryChanged || now - lastLog >= 500)) {
            lastLog = now;
            MvmDebugLog_Linef("environment.gi",
                                "updated=1 voxelized=%d triangles=%zu props=%zu lights=%d cell=%.2f "
                                "low=(%.1f,%.1f,%.1f) cameraIndependent=1",
                                int(geometryChanged), state.world->indices.size() / 3, used.size(), light,
                                build.info.y, bounds.low.x, bounds.low.y, bounds.low.z);
        }
    }
    trace.lowSize = build.lowSize;
    trace.info = {1, build.info.y, float(GiResolution), 0};
    ctx->UpdateSubresource(state.trace.Get(), 0, nullptr, &trace, 0, 0);
}
void BindGi(ID3D11DeviceContext *ctx) {
    auto cb = state.ready ? state.trace.Get() : nullptr;
    ctx->PSSetConstantBuffers(4, 1, &cb);
    ID3D11ShaderResourceView *srvs[] = {state.ready ? state.radianceSrv.Get() : nullptr,
                                        state.ready ? state.directionsSrv.Get() : nullptr};
    ctx->PSSetShaderResources(7, 2, srvs);
    auto sampler = state.sampler.Get();
    ctx->PSSetSamplers(3, 1, &sampler);
}
} // namespace Filmmaker::Environment
