#define NOMINMAX
#include "EnvironmentGameTextures.h"
#include "../Debug/MvmDebugLog.h"
#include "../MapData/MapSources.h"
#include "../MapData/MapTextures.h"
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace Filmmaker::Environment {
namespace {
using Microsoft::WRL::ComPtr;
using MapData::TextureMips;

using Loaded = MapData::GameSurfaceTextures;
Loaded LoadFromGame() { return MapData::ReadGameSurfaceTextures(MapData::ResolveCs2Root()); }

DXGI_FORMAT Format(TextureMips::Format f, bool srgb) {
    switch (f) {
    case TextureMips::Format::BC1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case TextureMips::Format::BC3: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case TextureMips::Format::BC4: return DXGI_FORMAT_BC4_UNORM;
    case TextureMips::Format::BC7: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    case TextureMips::Format::BGRA8: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

ComPtr<ID3D11ShaderResourceView> Upload(ID3D11Device *device, const TextureMips &mips, bool srgb, const char *name) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(mips.width);
    desc.Height = UINT(mips.height);
    desc.MipLevels = UINT(mips.mips.size());
    desc.ArraySize = 1;
    desc.Format = Format(mips.format, srgb);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> initial(mips.mips.size());
    for (size_t level = 0; level < mips.mips.size(); ++level)
        initial[level] = {mips.mips[level].data(), UINT(mips.RowPitch(int(level))), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateTexture2D(&desc, initial.data(), &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return nullptr;
    texture->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(strlen(name)), name);
    return view;
}

struct State {
    bool started = false, uploaded = false;
    std::future<Loaded> pending;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ShaderResourceView> color, normal, height, droplets;
    ComPtr<ID3D11SamplerState> sampler;
} g_state;
// The console reads the status on the main thread.
std::mutex g_statusLock;
std::string g_status = "not loaded";
void SetStatus(const std::string &text) {
    std::lock_guard<std::mutex> lock(g_statusLock);
    g_status = text;
}
} // namespace

GameTexturesBound BindGameTextures(ID3D11Device *device, ID3D11DeviceContext *ctx, bool wantSnow, bool wantDroplets) {
    GameTexturesBound bound;
    auto &s = g_state;
    if (!device || !ctx || !(wantSnow || wantDroplets)) return bound;
    if (s.device.Get() != device) {
        ReleaseGameTextures();
        s.device = device;
    }
    if (!s.started) {
        s.started = true;
        SetStatus("loading");
        s.pending = std::async(std::launch::async, LoadFromGame);
    }
    if (!s.uploaded && s.pending.valid() && s.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Loaded loaded = s.pending.get();
        s.uploaded = true;
        std::string status = loaded.status;
        if (loaded.snow) {
            s.color = Upload(device, loaded.snowColor, true, "SourceMVM CS2 snow colour");
            s.normal = Upload(device, loaded.snowNormal, false, "SourceMVM CS2 snow normal");
            s.height = Upload(device, loaded.snowHeight, false, "SourceMVM CS2 snow height");
            if (!s.color || !s.normal || !s.height) {
                s.color.Reset(); s.normal.Reset(); s.height.Reset();
                status += " (snow upload failed)";
            }
        }
        if (loaded.drops && !(s.droplets = Upload(device, loaded.droplets, false, "SourceMVM CS2 water droplets")))
            status += " (droplet upload failed)";
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &s.sampler);
        SetStatus(status);
        MvmDebugLog_LinefAlways("environment.gametextures", "%s", status.c_str());
    }
    bound.snow = wantSnow && s.color && s.normal && s.height && s.sampler;
    bound.droplets = wantDroplets && s.droplets && s.sampler;
    ID3D11ShaderResourceView *views[4] = {bound.snow ? s.color.Get() : nullptr, bound.snow ? s.normal.Get() : nullptr,
                                          bound.snow ? s.height.Get() : nullptr, bound.droplets ? s.droplets.Get() : nullptr};
    ctx->PSSetShaderResources(45, 4, views);
    if (s.sampler) {
        ID3D11SamplerState *sampler = s.sampler.Get();
        ctx->PSSetSamplers(9, 1, &sampler);
    }
    return bound;
}

std::string GameTexturesStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    return g_status;
}

void ReleaseGameTextures() {
    auto &s = g_state;
    // A read in flight finishes on its own; its result is simply not used by a new device.
    if (s.pending.valid()) s.pending.wait();
    s = State{};
    SetStatus("not loaded");
}
} // namespace Filmmaker::Environment
