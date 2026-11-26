#define NOMINMAX
#include "EnvironmentGameTextures.h"
#include "EnvironmentMaterialMath.h"
#include "EnvironmentSandTextureMath.h"
#include "../Debug/MvmDebugLog.h"
#include "../MapData/MapSources.h"
#include "../MapData/MapTextures.h"
#include "../../hlaeFolder.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
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

// THE SAND: four selectable styles, named in the UI by their look (0 Dusty Mix, 1 Cracked Crust,
// 2 Pebbly Grit, 3 Trampled), each packed as three PNGs beside the hook, with mip chains built here
// on first demand.
struct SandStyleInfo {
    const wchar_t *prefix;
    const char *label;
};
constexpr std::array<SandStyleInfo, kSandStyleCount> kSandStyles{{
    {L"sand_", "dusty mix"},
    {L"sand_fine_", "cracked crust"},
    {L"sand_coarse_", "pebbly grit"},
    {L"sand_rough_", "trampled"},
}};
struct SandLoaded {
    TextureMips color, normal, height;
    bool ok = false;
    std::string status;
};
SandLoaded LoadSand(int style) {
    SandLoaded out;
    const auto &info = kSandStyles[size_t(std::clamp(style, 0, kSandStyleCount - 1))];
    const std::filesystem::path folder = std::filesystem::path(GetHlaeFolderW()) / L"resources" / L"environment" / L"sand";
    struct Map { const wchar_t *suffix; SandMipKind kind; TextureMips *into; };
    const Map maps[] = {{L"color.png", SandMipKind::Colour, &out.color},
                        {L"normal.png", SandMipKind::HemiOctNormal, &out.normal},
                        {L"height.png", SandMipKind::Linear, &out.height}};
    for (const Map &map : maps) {
        const std::wstring name = std::wstring(info.prefix) + map.suffix;
        MaterialImage image;
        std::string error;
        if (!LoadMaterialImage((folder / name).wstring(), image, error)) {
            out.status = std::string("sand[") + info.label + "]: " + (folder / name).filename().string() + " " + error;
            return out;
        }
        map.into->format = TextureMips::Format::RGBA8;
        map.into->width = int(image.width);
        map.into->height = int(image.height);
        map.into->mips = SandMipChain(std::move(image.rgba), int(image.width), int(image.height), map.kind);
        if (map.into->mips.empty()) {
            out.status = std::string("sand[") + info.label + "]: " + (folder / name).filename().string() + " is not a power of two";
            return out;
        }
    }
    out.ok = true;
    out.status = std::string("sand[") + info.label + "] " + std::to_string(out.color.width) + "x" + std::to_string(out.color.height);
    return out;
}

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

struct SandSlot {
    bool started = false, uploaded = false;
    std::future<SandLoaded> pending;
    ComPtr<ID3D11ShaderResourceView> color, normal, height;
    std::string status;
    bool Ready() const { return color && normal && height; }
};
struct State {
    bool started = false, uploaded = false;
    std::future<Loaded> pending;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ShaderResourceView> color, normal, height, droplets;
    std::array<SandSlot, kSandStyleCount> sand{};
    ComPtr<ID3D11SamplerState> sampler;
    std::string gameStatus = "not loaded", sandStatus = "sand not loaded";
} g_state;
// The console reads the status on the main thread.
std::mutex g_statusLock;
std::string g_status = "not loaded";
void SetStatus() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    g_status = g_state.gameStatus + "; " + g_state.sandStatus;
}
void RefreshSandStatus() {
    std::string summary;
    for (int i = 0; i < kSandStyleCount; ++i) {
        const auto &slot = g_state.sand[size_t(i)];
        if (slot.status.empty()) continue;
        if (!summary.empty()) summary += ", ";
        summary += slot.status;
    }
    g_state.sandStatus = summary.empty() ? "sand not loaded" : summary;
    SetStatus();
}
void EnsureSampler(ID3D11Device *device) {
    if (g_state.sampler) return;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &g_state.sampler);
}
} // namespace

GameTexturesBound BindGameTextures(ID3D11Device *device, ID3D11DeviceContext *ctx, bool wantSnow, bool wantSand,
                                   bool wantDroplets, int sandStyle) {
    GameTexturesBound bound;
    auto &s = g_state;
    if (!device || !ctx || !(wantSnow || wantSand || wantDroplets)) return bound;
    if (s.device.Get() != device) {
        ReleaseGameTextures();
        s.device = device;
    }
    // CS2's snow and beads: one read of the game's packages, the first time either is wanted.
    if ((wantSnow || wantDroplets) && !s.started) {
        s.started = true;
        s.gameStatus = "loading";
        SetStatus();
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
        EnsureSampler(device);
        s.gameStatus = status;
        SetStatus();
        MvmDebugLog_LinefAlways("environment.gametextures", "%s", status.c_str());
    }
    // The sand: load the selected style asynchronously on first demand; keep any previously ready
    // style (preferring Dusty Mix) bound while a newly chosen style finishes loading (~15 ms).
    const int style = std::clamp(sandStyle, 0, kSandStyleCount - 1);
    if (wantSand && !s.sand[size_t(style)].started) {
        auto &slot = s.sand[size_t(style)];
        slot.started = true;
        slot.status = std::string("sand[") + kSandStyles[size_t(style)].label + "] loading";
        RefreshSandStatus();
        slot.pending = std::async(std::launch::async, LoadSand, style);
    }
    for (int i = 0; i < kSandStyleCount; ++i) {
        auto &slot = s.sand[size_t(i)];
        if (!slot.uploaded && slot.pending.valid() &&
            slot.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            SandLoaded loaded = slot.pending.get();
            slot.uploaded = true;
            std::string status = loaded.status;
            if (loaded.ok) {
                slot.color = Upload(device, loaded.color, true, "SourceMVM sand colour");
                slot.normal = Upload(device, loaded.normal, false, "SourceMVM sand normal");
                slot.height = Upload(device, loaded.height, false, "SourceMVM sand height");
                if (!slot.Ready()) {
                    slot.color.Reset(); slot.normal.Reset(); slot.height.Reset();
                    status += " (upload failed)";
                }
            }
            EnsureSampler(device);
            slot.status = status;
            RefreshSandStatus();
            MvmDebugLog_LinefAlways("environment.gametextures", "%s", status.c_str());
        }
    }
    const SandSlot *activeSand = nullptr;
    if (wantSand && s.sampler) {
        if (s.sand[size_t(style)].Ready())
            activeSand = &s.sand[size_t(style)];
        else if (s.sand[0].Ready())
            activeSand = &s.sand[0];
        else
            for (const auto &slot : s.sand)
                if (slot.Ready()) { activeSand = &slot; break; }
    }
    bound.sand = activeSand != nullptr;
    bound.snow = !wantSand && wantSnow && s.color && s.normal && s.height && s.sampler;
    bound.droplets = wantDroplets && s.droplets && s.sampler;
    ID3D11ShaderResourceView *views[4] = {
        bound.sand ? activeSand->color.Get() : bound.snow ? s.color.Get() : nullptr,
        bound.sand ? activeSand->normal.Get() : bound.snow ? s.normal.Get() : nullptr,
        bound.sand ? activeSand->height.Get() : bound.snow ? s.height.Get() : nullptr,
        bound.droplets ? s.droplets.Get() : nullptr};
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
    for (auto &slot : s.sand)
        if (slot.pending.valid()) slot.pending.wait();
    s = State{};
    SetStatus();
}
} // namespace Filmmaker::Environment
