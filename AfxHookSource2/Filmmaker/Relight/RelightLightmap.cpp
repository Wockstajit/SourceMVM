#define NOMINMAX
#include "RelightLightmap.h"
#include "RelightLightmapMath.h"
#include "RelightLightmapShaders.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderPrivateTextureScope.h"
#include <d3d11_3.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace Filmmaker::Relight {
using Microsoft::WRL::ComPtr;
namespace {
struct Loading {
    std::string path;
    std::shared_ptr<LightmapLayout> layout;
    std::string error;
    std::atomic<bool> done{false};
};
struct Constants { float boundsLow[4], boundsExtent[4], modeFill[4], spare[4]; };
struct State {
    std::mutex lock;
    std::shared_ptr<Loading> loading;
    std::shared_ptr<LightmapLayout> layout;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> work;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> input;
    ComPtr<ID3D11Buffer> vertices, indices, constants;
    ComPtr<ID3D11RasterizerState> exact, conservative;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Texture2D> atlas;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT width = 0, height = 0;
    uint64_t drawn = ~0ull;
    bool gpuFailed = false, conservativeSupported = false, verifyPending = false;
    std::string status = "no layout requested";
} g;

// Reads the file on a worker thread; parsing the ~100 MB layout must never sit in a frame.
void Read(std::shared_ptr<Loading> job) {
    try {
        std::ifstream file(std::filesystem::u8path(job->path), std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("cannot open " + job->path);
        const auto size = size_t(file.tellg());
        if (size > 1024ull * 1024 * 1024) throw std::runtime_error("layout file is implausibly large");
        std::vector<uint8_t> bytes(size);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size));
        if (!file) throw std::runtime_error("short read");
        auto layout = std::make_shared<LightmapLayout>();
        layout->source = job->path;
        const auto reason = ParseLightmapLayout(bytes.data(), bytes.size(), *layout);
        if (!reason.empty()) throw std::runtime_error(reason);
        job->layout = std::move(layout);
    } catch (const std::exception &e) {
        job->error = e.what();
    }
    job->done.store(true, std::memory_order_release);
}

bool Compile(const char *entry, const char *profile, ComPtr<ID3DBlob> &code) {
    ComPtr<ID3DBlob> errors;
    if (SUCCEEDED(Environment::CompileEnvironmentShader(kLightmapShader, sizeof(kLightmapShader) - 1, "RelightLightmap",
                                                        nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                                        &code, &errors)))
        return true;
    MvmDebugLog_LinefAlways("relight.lightmap.gpu", "shader=%s failure=%s", entry,
                            errors ? static_cast<const char *>(errors->GetBufferPointer()) : "compiler unavailable");
    return false;
}

bool CreateStatic(ID3D11Device *device) {
    if (g.vs) return true;
    ComPtr<ID3DBlob> vs, ps;
    if (!Compile("VS", "vs_5_0", vs) || !Compile("PS", "ps_5_0", ps)) return false;
    const D3D11_INPUT_ELEMENT_DESC elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R16G16_SNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ps)) ||
        FAILED(device->CreateInputLayout(elements, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g.input)) ||
        FAILED(device->CreateBuffer(&cb, nullptr, &g.constants)) ||
        FAILED(device->CreateRasterizerState(&raster, &g.exact)) ||
        FAILED(device->CreateDepthStencilState(&depth, &g.depth)) ||
        FAILED(device->CreateBlendState(&blend, &g.blend)) || FAILED(device->CreateDeferredContext(0, &g.work)))
        return false;
    // Conservative rasterisation lets a triangle that only touches a texel still write it, which
    // is what pads every chart's border so bilinear sampling never reads an unwritten texel.
    ComPtr<ID3D11Device3> device3;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3))) &&
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2))) &&
        options2.ConservativeRasterizationTier != D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED) {
        D3D11_RASTERIZER_DESC2 r2{};
        r2.FillMode = D3D11_FILL_SOLID;
        r2.CullMode = D3D11_CULL_NONE;
        r2.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;
        ComPtr<ID3D11RasterizerState2> state2;
        if (SUCCEEDED(device3->CreateRasterizerState2(&r2, &state2))) {
            g.conservative = state2;
            g.conservativeSupported = true;
        }
    }
    MvmDebugLog_LinefAlways("relight.lightmap.gpu", "ready conservativeRaster=%d", int(g.conservativeSupported));
    return true;
}

bool UploadLayout(ID3D11Device *device) {
    RenderPrivateTextureScope privateTextures;
    const auto &layout = *g.layout;
    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = UINT(layout.vertices.size() * sizeof(LightmapVertex));
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA vd{layout.vertices.data(), 0, 0};
    D3D11_BUFFER_DESC ib{};
    ib.ByteWidth = UINT(layout.indices.size() * 4);
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA id{layout.indices.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&vb, &vd, g.vertices.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateBuffer(&ib, &id, g.indices.ReleaseAndGetAddressOf())))
        return false;
    g.width = layout.header.atlasWidth;
    g.height = layout.header.atlasHeight;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g.width;
    td.Height = g.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = 1;
    if (FAILED(device->CreateTexture2D(&td, nullptr, g.atlas.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRenderTargetView(g.atlas.Get(), nullptr, g.rtv.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(g.atlas.Get(), &sv, g.srv.ReleaseAndGetAddressOf())))
        return false;
    MvmDebugLog_LinefAlways("relight.lightmap.gpu", "uploaded triangles=%zu points=%u atlas=%ux%u vertexBytes=%zu",
                            layout.indices.size() / 3, layout.header.pointCount, g.width, g.height,
                            layout.vertices.size() * sizeof(LightmapVertex));
    return true;
}

bool Draw(ID3D11DeviceContext *immediate, const LightmapParams &params) {
    const auto &layout = *g.layout;
    const auto &h = layout.header;
    Constants c{};
    for (int i = 0; i < 3; ++i) {
        c.boundsLow[i] = h.boundsLow[i];
        c.boundsExtent[i] = h.boundsHigh[i] - h.boundsLow[i];
    }
    c.modeFill[0] = float(params.mode);
    c.modeFill[1] = params.fill.x;
    c.modeFill[2] = params.fill.y;
    c.modeFill[3] = params.fill.z;
    auto work = g.work.Get();
    const float clear[4]{};
    work->ClearRenderTargetView(g.rtv.Get(), clear);
    work->UpdateSubresource(g.constants.Get(), 0, nullptr, &c, 0, 0);
    auto cb = g.constants.Get();
    work->VSSetConstantBuffers(0, 1, &cb);
    work->PSSetConstantBuffers(0, 1, &cb);
    work->VSSetShader(g.vs.Get(), nullptr, 0);
    work->PSSetShader(g.ps.Get(), nullptr, 0);
    work->IASetInputLayout(g.input.Get());
    UINT stride = sizeof(LightmapVertex), offset = 0;
    auto vb = g.vertices.Get();
    work->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    work->IASetIndexBuffer(g.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    auto rtv = g.rtv.Get();
    work->OMSetRenderTargets(1, &rtv, nullptr);
    work->OMSetDepthStencilState(g.depth.Get(), 0);
    const float factor[4]{};
    work->OMSetBlendState(g.blend.Get(), factor, ~0u);
    const D3D11_VIEWPORT vp{0, 0, float(g.width), float(g.height), 0, 1};
    work->RSSetViewports(1, &vp);
    const UINT triangleIndices = UINT(layout.indices.size());
    // 1. Padded triangles, 2. one point per texel-sized object, 3. exact triangles on top, so a
    // neighbour's padding can never overwrite a texel that a chart really owns.
    if (g.conservativeSupported && triangleIndices) {
        work->RSSetState(g.conservative.Get());
        work->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        work->DrawIndexed(triangleIndices, 0, 0);
    }
    work->RSSetState(g.exact.Get());
    if (h.pointCount) {
        work->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        work->Draw(h.pointCount, h.vertexCount);
    }
    if (triangleIndices) {
        work->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        work->DrawIndexed(triangleIndices, 0, 0);
    }
    ComPtr<ID3D11CommandList> commands;
    if (FAILED(work->FinishCommandList(FALSE, &commands))) return false;
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    return true;
}
} // namespace

void LightmapRequest(const std::string &path) {
    std::lock_guard<std::mutex> lock(g.lock);
    if (g.loading && g.loading->path == path) return;
    if (!g.loading && g.layout && g.layout->source == path) return;
    auto job = std::make_shared<Loading>();
    job->path = path;
    g.loading = job;
    g.layout.reset();
    g.drawn = ~0ull;
    g.status = "loading " + path;
    std::thread(Read, job).detach();
}

ID3D11ShaderResourceView *LightmapAtlas(ID3D11Device *device, ID3D11DeviceContext *immediate,
                                        const LightmapParams &params) {
    if (!device || !immediate) return nullptr;
    if (g.device.Get() != device) {
        g.vs.Reset(); g.ps.Reset(); g.input.Reset(); g.vertices.Reset(); g.indices.Reset(); g.constants.Reset();
        g.exact.Reset(); g.conservative.Reset(); g.depth.Reset(); g.blend.Reset(); g.atlas.Reset(); g.rtv.Reset();
        g.srv.Reset(); g.work.Reset(); g.drawn = ~0ull; g.gpuFailed = false; g.conservativeSupported = false;
        g.device = device;
        if (g.layout) g.vertices.Reset();
    }
    {
        std::lock_guard<std::mutex> lock(g.lock);
        if (g.loading && g.loading->done.load(std::memory_order_acquire)) {
            if (g.loading->layout) {
                g.layout = g.loading->layout;
                g.status = "layout loaded: " + std::to_string(g.layout->header.vertexCount) + " triangle vertices, " +
                           std::to_string(g.layout->header.pointCount) + " points";
                g.vertices.Reset();
                g.drawn = ~0ull;
            } else {
                g.status = "layout failed: " + g.loading->error;
                MvmDebugLog_LinefAlways("relight.lightmap.gpu", "%s", g.status.c_str());
            }
            g.loading.reset();
        }
    }
    if (!g.layout || g.gpuFailed) return nullptr;
    if (!CreateStatic(device) || (!g.vertices && !UploadLayout(device))) {
        g.gpuFailed = true;
        g.status = "GPU setup failed";
        return nullptr;
    }
    const uint64_t key = params.generation * 8 + params.mode;
    if (g.drawn != key) {
        if (!Draw(immediate, params)) { g.gpuFailed = true; g.status = "atlas draw failed"; return nullptr; }
        g.drawn = key;
        MvmDebugLog_LinefAlways("relight.lightmap.gpu", "atlas drawn mode=%u generation=%llu", params.mode,
                                static_cast<unsigned long long>(params.generation));
        g.verifyPending = true;
    }
    if (g.verifyPending && MvmDebugLog_Active()) {
        // Evidence that the atlas holds something: how many texels of a few 512x512 blocks are non-zero.
        g.verifyPending = false;
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = 512;
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
            const UINT origins[4][2]{{0, 0}, {g.width / 2, 0}, {0, g.height / 4}, {g.width / 2, g.height / 2}};
            for (auto &origin : origins) {
                const D3D11_BOX box{origin[0], origin[1], 0, origin[0] + 512, origin[1] + 512, 1};
                immediate->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, g.atlas.Get(), 0, &box);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) continue;
                unsigned nonzero = 0;
                for (UINT y = 0; y < 512; ++y) {
                    const auto *row = reinterpret_cast<const uint32_t *>(static_cast<const char *>(m.pData) + y * m.RowPitch);
                    for (UINT x = 0; x < 512; ++x) nonzero += row[x] != 0;
                }
                immediate->Unmap(staging.Get(), 0);
                MvmDebugLog_LinefAlways("relight.lightmap.gpu", "block(%u,%u) nonzero=%u/262144", origin[0], origin[1], nonzero);
            }
        }
    }
    return g.srv.Get();
}

std::string LightmapStatus() { return g.status; }

void EndLightmap() {
    std::lock_guard<std::mutex> lock(g.lock);
    g.loading.reset();
    g.layout.reset();
    g.vs.Reset(); g.ps.Reset(); g.input.Reset(); g.vertices.Reset(); g.indices.Reset(); g.constants.Reset();
    g.exact.Reset(); g.conservative.Reset(); g.depth.Reset(); g.blend.Reset(); g.atlas.Reset(); g.rtv.Reset();
    g.srv.Reset(); g.work.Reset(); g.device.Reset();
    g.drawn = ~0ull; g.gpuFailed = false;
    g.status = "no layout requested";
}

} // namespace Filmmaker::Relight
