#define NOMINMAX
#include "EnvironmentWeatherCover.h"
#include "EnvironmentShadowSpatialMath.h"
#include "EnvironmentWeatherCoverMath.h"
#include "../Debug/MvmDebugLog.h"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
// The box the volume covers: where people walk (the nav mesh) with room around and above it, inside
// the map's own box. Above it everything is open sky, which the sky map already knows.
WeatherCoverLayout LayoutOf(const OcclusionMesh &world) {
    Vec3 low = world.low, high = world.high;
    if (!world.navPositions.empty()) {
        Vec3 a = world.navPositions[0], b = a;
        for (const auto &p : world.navPositions) {
            a = {(std::min)(a.x, p.x), (std::min)(a.y, p.y), (std::min)(a.z, p.z)};
            b = {(std::max)(b.x, p.x), (std::max)(b.y, p.y), (std::max)(b.z, p.z)};
        }
        low = {(std::max)(low.x, a.x - 384), (std::max)(low.y, a.y - 384), (std::max)(low.z, a.z - 96)};
        high = {(std::min)(high.x, b.x + 384), (std::min)(high.y, b.y + 384), (std::min)(high.z, b.z + 384)};
    }
    return MakeWeatherCoverLayout(low, high);
}
// Half the machine's threads, at most eight, below normal priority: the game keeps its frame rate
// while the bake runs.
unsigned WorkerThreads() { return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u); }
// THE BAKE IS KEPT. Working out the cover takes a second or two of eight threads on every launch,
// and until it is done rain and snow fall through roofs. Its result depends only on the map's
// triangles and the box it covers, so it is filed under a fingerprint of exactly those (every
// vertex and index, the layout, and kCoverVersion) in <AppData>\HLAE\source_mvm\environment\cover
// and read back the next time: a map that changes gets another fingerprint, and a bake that
// changes must bump kCoverVersion. A file that does not read back whole is ignored and baked again.
constexpr uint32_t kCoverVersion = 2; // 2: joins need most of five rays (no joins through cracks)
constexpr char kCoverMagic[8] = {'M', 'V', 'M', 'C', 'O', 'V', 'R', '1'};
uint64_t Fingerprint(const void *data, size_t bytes, uint64_t h) {
    const auto *p = static_cast<const unsigned char *>(data);
    // Eight bytes a step (FNV-1a over words): the map is tens of megabytes.
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t word;
        std::memcpy(&word, p + i, 8);
        h = (h ^ word) * 1099511628211ull;
    }
    for (; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
std::filesystem::path CoverFile(uint64_t fingerprint) {
    // Found from the environment, as the shader cache is: this file is also built into test
    // programs that have none of the hook's own folder helpers. No folder, no file kept.
    wchar_t roaming[32768];
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", roaming, 32768);
    if (!length || length >= 32768) return {};
    wchar_t name[40];
    swprintf_s(name, L"%016llx.cover", static_cast<unsigned long long>(fingerprint));
    return std::filesystem::path(roaming) / L"HLAE" / L"source_mvm" / L"environment" / L"cover" / name;
}
bool ReadCover(const std::filesystem::path &path, const WeatherCoverLayout &layout, std::vector<uint8_t> &texels) {
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary);
    char magic[8];
    uint32_t dims[3];
    if (!file.read(magic, 8) || std::memcmp(magic, kCoverMagic, 8) || !file.read(reinterpret_cast<char *>(dims), sizeof(dims))) return false;
    if (dims[0] != layout.nx || dims[1] != layout.ny || dims[2] != layout.nz) return false;
    texels.resize(layout.Cells() * 2);
    if (!file.read(reinterpret_cast<char *>(texels.data()), std::streamsize(texels.size()))) return false;
    return file.peek() == std::ifstream::traits_type::eof();
}
void WriteCover(const std::filesystem::path &path, const WeatherCoverLayout &layout, const std::vector<uint8_t> &texels) {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    auto partial = path;
    partial += L".part";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        const uint32_t dims[3] = {layout.nx, layout.ny, layout.nz};
        file.write(kCoverMagic, 8);
        file.write(reinterpret_cast<const char *>(dims), sizeof(dims));
        file.write(reinterpret_cast<const char *>(texels.data()), std::streamsize(texels.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(partial, ignored);
            return;
        }
    }
    std::filesystem::rename(partial, path, ignored);
    if (ignored) std::filesystem::remove(partial, ignored);
}
struct Job {
    std::shared_ptr<const OcclusionMesh> world;
    WeatherCoverLayout layout;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::string status = "weather cover: starting";
    std::unique_ptr<std::vector<uint8_t>> texels;
    std::thread thread;
    ~Job() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
    void Say(const std::string &text) {
        std::lock_guard<std::mutex> lock(mutex);
        status = text;
    }
    template <class Body> void Parallel(size_t count, Body body) {
        std::atomic<size_t> next{0};
        const size_t chunk = (std::max)(size_t(64), count / (WorkerThreads() * 64));
        auto run = [&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                if (stop.load(std::memory_order_relaxed)) return;
                const size_t first = next.fetch_add(chunk);
                if (first >= count) return;
                body(first, (std::min)(count, first + chunk));
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < WorkerThreads(); ++i) pool.emplace_back(run);
        run();
        for (auto &t : pool) t.join();
    }
    void Run() {
        using Clock = std::chrono::steady_clock;
        auto since = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
        const auto start = Clock::now();
        Say("weather cover: reading the map's triangles");
        RigidShadowGeometry scratch;
        SurfaceRayGrid rays;
        WeatherCoverBake bake;
        try {
            const auto source = RigidShadowSource(*world, scratch);
            // Only the triangles in and above the volume: its box with a voxel around it, up to the
            // highest thing on the map (a roof far above still shelters what is under it).
            uint64_t fingerprint = Fingerprint(&layout, sizeof(layout), 14695981039346656037ull ^ kCoverVersion);
            fingerprint = Fingerprint(source.positions.data(), source.positions.size() * sizeof(Vec3), fingerprint);
            fingerprint = Fingerprint(source.indices.data(), source.indices.size() * sizeof(uint32_t), fingerprint);
            const auto kept = CoverFile(fingerprint);
            {
                auto read = std::make_unique<std::vector<uint8_t>>();
                if (ReadCover(kept, layout, *read)) {
                    char text[240];
                    std::snprintf(text, sizeof(text), "weather cover ready: %ux%ux%u voxels, read back in %.2f s", layout.nx, layout.ny, layout.nz, since(start) / 1000);
                    MvmDebugLog_LinefAlways("environment.cover", "%s file=%016llx triangles=%zu", text, static_cast<unsigned long long>(fingerprint), source.indices.size() / 3);
                    std::lock_guard<std::mutex> lock(mutex);
                    texels = std::move(read);
                    status = text;
                    return;
                }
            }
            const Vec3 cell = layout.Cell();
            float top = layout.high.z;
            for (const auto &point : source.positions) top = (std::max)(top, point.z);
            const Vec3 boxLow = layout.low - cell, boxHigh{layout.high.x + cell.x, layout.high.y + cell.y, (std::min)(top, layout.high.z + 4096) + 1};
            if (source.indices.size() < 3 || !BuildSurfaceRayGrid(source.positions, source.indices, rays, 8.f, &boxLow, &boxHigh)) {
                Say("weather cover: no usable map triangles (sky map only)");
                return;
            }
            const double gridMs = since(start);
            const WeatherCoverMesh mesh{&source.positions, &source.indices, &rays};
            bake.Resize(layout);
            Say("weather cover: finding the roofs");
            auto at = Clock::now();
            Parallel(size_t(layout.nx) * layout.ny, [&](size_t a, size_t b) { WeatherCoverColumns(mesh, bake, a, b); });
            const double roofMs = since(at);
            Say("weather cover: finding the walls");
            at = Clock::now();
            Parallel(layout.Cells(), [&](size_t a, size_t b) { WeatherCoverJoins(mesh, bake, a, b); });
            const double wallMs = since(at);
            if (stop) return;
            rays = SurfaceRayGrid{};
            Say("weather cover: spreading open air under cover");
            at = Clock::now();
            WeatherCoverSolve(bake, [&](size_t count, auto body) { Parallel(count, body); });
            if (stop) return;
            WeatherCoverTraffic(bake, [&](size_t count, auto body) { Parallel(count, body); });
            const double spreadMs = since(at);
            if (stop) return;
            size_t open = 0, reached = 0;
            for (size_t i = 0; i < bake.open.size(); ++i) {
                open += bake.open[i];
                reached += !bake.open[i] && bake.exposure[i] > .01f;
            }
            auto result = std::make_unique<std::vector<uint8_t>>(WeatherCoverTexels(bake));
            if (!stop) WriteCover(kept, layout, *result);
            char text[240];
            std::snprintf(text, sizeof(text), "weather cover ready: %ux%ux%u voxels, %u threads, %.1f s (triangles %.1f, roofs %.1f, walls %.1f, spread %.1f)",
                          layout.nx, layout.ny, layout.nz, WorkerThreads(), since(start) / 1000, gridMs / 1000, roofMs / 1000,
                          wallMs / 1000, spreadMs / 1000);
            MvmDebugLog_LinefAlways("environment.cover", "%s triangles=%zu render=%u open=%zu underCover=%zu low=(%.0f %.0f %.0f) high=(%.0f %.0f %.0f)",
                                    text, source.indices.size() / 3, unsigned(world->visual && world->visual->renderGeometry), open, reached,
                                    layout.low.x, layout.low.y, layout.low.z, layout.high.x, layout.high.y, layout.high.z);
            std::lock_guard<std::mutex> lock(mutex);
            texels = std::move(result);
            status = text;
        } catch (const std::bad_alloc &) {
            Say("weather cover: not enough memory (sky map only)");
        }
    }
};
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    WeatherCoverLayout layout;
    std::vector<uint8_t> texels; // kept for WeatherCoverAt
};
std::unique_ptr<Job> g_job;
Gpu g_gpu;
} // namespace
void EndWeatherCover() {
    g_job.reset();
    g_gpu = Gpu{};
}
std::string WeatherCoverStatus() {
    if (!g_job) return "weather cover: off";
    std::lock_guard<std::mutex> lock(g_job->mutex);
    return g_job->status;
}
float WeatherCoverAt(Vec3 point) {
    if (!g_gpu.view || g_gpu.texels.empty()) return 0;
    return WeatherCoverSample(g_gpu.layout, g_gpu.texels, point);
}
bool WeatherCoverReady() { return g_gpu.view && !g_gpu.texels.empty(); }
float WeatherCoverTop(float x, float y, float low, float high, float step) {
    if (!WeatherCoverReady()) return low;
    for (float z = high; z > low; z -= step)
        if (WeatherCoverSample(g_gpu.layout, g_gpu.texels, {x, y, z}) < .35f) return z;
    return low;
}
bool WriteWeatherCoverPlan(const OcclusionMesh &world, const std::string &path, std::string &error) {
    if (!g_gpu.view || g_gpu.texels.empty()) {
        error = "The weather cover is not ready";
        return false;
    }
    if (world.navIndices.size() < 3) {
        error = "This map has no nav mesh";
        return false;
    }
    const auto &l = g_gpu.layout;
    const float scale = 8;
    const int w = int(std::ceil((l.high.x - l.low.x) / scale)), h = int(std::ceil((l.high.y - l.low.y) / scale));
    std::vector<uint8_t> rgb(size_t(w) * h * 3, 0);
    // Lowest floors first, so an upper floor is drawn over the one beneath it.
    std::vector<uint32_t> order(world.navIndices.size() / 3);
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    auto top = [&](uint32_t t) {
        return (std::max)({world.navPositions[world.navIndices[t * 3]].z, world.navPositions[world.navIndices[t * 3 + 1]].z,
                           world.navPositions[world.navIndices[t * 3 + 2]].z});
    };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return top(a) < top(b); });
    size_t open = 0, reached = 0, sheltered = 0;
    for (const uint32_t t : order) {
        const Vec3 a = world.navPositions[world.navIndices[t * 3]], b = world.navPositions[world.navIndices[t * 3 + 1]],
                   c = world.navPositions[world.navIndices[t * 3 + 2]];
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::abs(area) < 1e-3f) continue;
        const int x0 = (std::max)(0, int(((std::min)({a.x, b.x, c.x}) - l.low.x) / scale)),
                  x1 = (std::min)(w - 1, int(((std::max)({a.x, b.x, c.x}) - l.low.x) / scale)),
                  y0 = (std::max)(0, int(((std::min)({a.y, b.y, c.y}) - l.low.y) / scale)),
                  y1 = (std::min)(h - 1, int(((std::max)({a.y, b.y, c.y}) - l.low.y) / scale));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = l.low.x + (x + .5f) * scale, py = l.low.y + (y + .5f) * scale;
                const float u = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area,
                            v = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area, s = 1 - u - v;
                if (u < -.02f || v < -.02f || s < -.02f) continue;
                const float e = WeatherCoverSample(l, g_gpu.texels, {px, py, u * a.z + v * b.z + s * c.z + 8});
                uint8_t *out = &rgb[(size_t(h - 1 - y) * w + x) * 3];
                if (e >= .97f) {
                    out[0] = out[1] = out[2] = 205;
                    ++open;
                } else if (e > .03f) {
                    const float g = std::clamp((e - .03f) / .42f, 0.f, 1.f);
                    out[0] = 20;
                    out[1] = uint8_t(90 + 150 * g);
                    out[2] = 60;
                    ++reached;
                } else {
                    out[0] = 28;
                    out[1] = 36;
                    out[2] = 110;
                    ++sheltered;
                }
            }
    }
    FILE *file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") || !file) {
        error = "Could not write " + path;
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    const double total = double((std::max)(size_t(1), open + reached + sheltered));
    char text[240];
    std::snprintf(text, sizeof(text), "floor: %.0f%% open, %.0f%% reached under cover, %.0f%% sheltered; %dx%d pixels of 8 units from (%.0f %.0f), heights %.0f to %.0f",
                  100 * open / total, 100 * reached / total, 100 * sheltered / total, w, h, l.low.x, l.high.y, l.low.z, l.high.z);
    error = text;
    return true;
}
WeatherCoverBinding PrepareWeatherCover(ID3D11Device *device, const std::shared_ptr<const OcclusionMesh> &world) {
    if (!device || !world) return {};
    if (g_gpu.device.Get() != device || (g_job && g_job->world != world)) EndWeatherCover();
    g_gpu.device = device;
    if (!g_job) {
        g_job = std::make_unique<Job>();
        g_job->world = world;
        g_job->layout = LayoutOf(*world);
        g_job->thread = std::thread([job = g_job.get()] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            job->Run();
        });
    }
    std::unique_ptr<std::vector<uint8_t>> ready;
    {
        std::lock_guard<std::mutex> lock(g_job->mutex);
        ready = std::move(g_job->texels);
    }
    if (ready) {
        const auto &l = g_job->layout;
        D3D11_TEXTURE3D_DESC d{};
        d.Width = l.nx;
        d.Height = l.ny;
        d.Depth = l.nz;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = ready->data();
        initial.SysMemPitch = l.nx * 2;
        initial.SysMemSlicePitch = l.nx * l.ny * 2;
        g_gpu.texture.Reset();
        g_gpu.view.Reset();
        if (SUCCEEDED(device->CreateTexture3D(&d, &initial, &g_gpu.texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(g_gpu.texture.Get(), nullptr, &g_gpu.view))) {
            g_gpu.layout = l;
            g_gpu.texels = std::move(*ready);
        } else {
            g_gpu.texture.Reset();
            g_gpu.view.Reset();
        }
    }
    if (!g_gpu.view) return {};
    const auto &l = g_gpu.layout;
    const Vec3 cell = l.Cell();
    return {g_gpu.view.Get(), l.low, {1 / cell.x, 1 / cell.y, 1 / cell.z}, l.nx, l.ny, l.nz};
}
} // namespace Filmmaker::Environment
