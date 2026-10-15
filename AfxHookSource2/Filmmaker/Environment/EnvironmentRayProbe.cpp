#define NOMINMAX
#include "EnvironmentRayProbe.h"
#include "EnvironmentRayBackend.h"
#include "EnvironmentSurfaceMath.h"
#include "../Debug/MvmDebugLog.h"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <future>
#include <chrono>
namespace Filmmaker::Environment {
namespace {
std::atomic<bool> requested{false};
std::mutex requestMutex;
std::string requestedMap;
std::future<void> worker;
} // namespace
void RequestHardwareRayProbe(const std::string &map) {
    std::lock_guard<std::mutex> lock(requestMutex);
    requestedMap = map;
    requested = true;
}
void PumpHardwareRayProbe(ID3D11Device *device, const Frame &frame) {
    if (worker.valid()) {
        if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            worker.get();
        } catch (const std::exception &e) {
            MvmDebugLog_LinefAlways("environment.ray", "worker failure=%s", e.what());
        }
    }
    std::string map;
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        if (!requested.exchange(false))
            return;
        map = requestedMap;
    }
    if (!device || !frame.occlusion) {
        MvmDebugLog_LinefAlways("environment.ray", "probe rejected: map data not loaded yet");
        return;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) {
        MvmDebugLog_LinefAlways("environment.ray", "probe rejected: game adapter unavailable");
        return;
    }
    auto mesh = frame.occlusion;
    auto camera = frame.camera;
    Vec3 forward, right, up;
    CameraBasis(frame.angles, forward, right, up);
    MvmDebugLog_LinefAlways("environment.ray", "probe queued map=%s triangles=%zu camera=(%.2f %.2f %.2f)",
                            map.c_str(), mesh->indices.size() / 3, camera.x, camera.y, camera.z);
    worker = std::async(std::launch::async, [adapter, mesh, camera, forward, right, up, map]() {
        RayBackend backend;
        std::string error;
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        auto start = std::chrono::steady_clock::now();
        if (!backend.Begin(adapter.Get(), error) || !backend.Build(*mesh, error)) {
            MvmDebugLog_LinefAlways("environment.ray", "probe failed map=%s reason=%s", map.c_str(),
                                    error.c_str());
            return;
        }
        double buildMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<HardwareRay> rays;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x)
                rays.push_back(
                    {camera, .01f, Normalize(forward + right * (x * .12f) + up * (y * .12f)), 32768});
        std::vector<HardwareHit> hits;
        start = std::chrono::steady_clock::now();
        if (!backend.Trace(rays, hits, error)) {
            MvmDebugLog_LinefAlways("environment.ray", "trace failed: %s", error.c_str());
            return;
        }
        double traceMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        unsigned mismatches = 0, found = 0;
        float largest = 0;
        for (size_t i = 0; i < rays.size(); ++i) {
            SurfaceHit reference;
            bool hit = SurfaceRaycast(*mesh, rays[i].origin, rays[i].direction, rays[i].maximum, reference);
            if (hit != (hits[i].distance >= 0))
                ++mismatches;
            if (hit && hits[i].distance >= 0) {
                ++found;
                float delta = std::abs(reference.distance - hits[i].distance);
                largest = (std::max)(largest, delta);
                if (delta > .05f)
                    ++mismatches;
            }
        }
        MvmDebugLog_LinefAlways("environment.ray",
                                "probe complete map=%s adapterLuid=%08x:%08x sameGameAdapter=1 hardware=1 "
                                "triangles=%zu rays=%zu hits=%u mismatches=%u maxDistanceError=%.6f "
                                "buildMs=%.3f traceReadbackMs=%.3f materialsCaptured=0 shadingConnected=0",
                                map.c_str(), UINT(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                                mesh->indices.size() / 3, rays.size(), found, mismatches, largest, buildMs,
                                traceMs);
    });
}
} // namespace Filmmaker::Environment
