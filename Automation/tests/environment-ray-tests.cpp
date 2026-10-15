#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentRayBackend.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSurfaceMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentRaySurfaceMath.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <fstream>
#include <chrono>
using namespace Filmmaker::Environment;
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char *label, const std::string &error = {}) {
    if (!ok) {
        std::cerr << label << ": " << error << '\n';
        exit(1);
    }
}
int main(int argc, char **argv) {
    ComPtr<IDXGIFactory6> factory;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Create factory");
    RayBackend backend;
    std::string error;
    bool ready = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;
        if (backend.Begin(adapter.Get(), error)) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            std::wcout << L"DXR adapter: " << d.Description << L'\n';
            ready = true;
            break;
        }
    }
    if (!ready) {
        std::cerr << "SKIP: " << error << '\n';
        return 77;
    }
    OcclusionMesh mesh;
    mesh.positions = {{-10, -10, 10}, {10, -10, 10}, {0, 10, 10}, {-10, -10, 20}, {10, -10, 20}, {0, 10, 20}};
    mesh.indices = {0, 1, 2, 3, 4, 5};
    Check(backend.Build(mesh, error), "Build triangles", error);
    std::vector<HardwareRay> rays = {{{0, 0, 0}, .01f, {0, 0, 1}, 100},
                                     {{40, 0, 0}, .01f, {0, 0, 1}, 100},
                                     {{0, 0, 30}, .01f, {0, 0, -1}, 100},
                                     {{0, 0, 0}, 11, {0, 0, 1}, 100},
                                     {{0, 0, 0}, .01f, {0, 0, 1}, 5}};
    std::vector<HardwareHit> hits;
    Check(backend.Trace(rays, hits, error), "Trace", error);
    Check(hits.size() == 5 && std::abs(hits[0].distance - 10) < .001f && hits[0].primitive == 0,
          "Nearest triangle");
    Check(hits[1].distance < 0 && hits[1].primitive == UINT32_MAX && hits[4].distance < 0,
          "Miss and maximum interval");
    Check(hits[2].primitive == 1 && std::abs(hits[2].distance - 10) < .001f, "Backface ray");
    Check(hits[3].primitive == 1 && std::abs(hits[3].distance - 20) < .001f, "Minimum interval");
    Check(std::abs(hits[0].u - .25f) < .001f && std::abs(hits[0].v - .5f) < .001f,
          "Barycentric material coordinates");
    std::vector<RaySurfaceVertex> surfaceVertices;
    for (size_t i = 0; i < mesh.positions.size(); ++i)
        surfaceVertices.push_back({mesh.positions[i], {0, 0, 1}, float(i % 3 == 1), float(i % 3 == 2)});
    RaySurface surface;
    Check(ResolveRaySurface(surfaceVertices, mesh.indices, {7, 9}, hits[0].primitive, hits[0].u, hits[0].v,
                            surface),
          "DXR hit resolves surface");
    Check(surface.material == 7 && std::abs(surface.u - .25f) < .001f && std::abs(surface.v - .5f) < .001f,
          "DXR hit carries exact material and UV");
    auto original = hits;
    Check(backend.Trace({rays[1]}, hits, error), "Different ray batch", error);
    Check(backend.Trace(rays, hits, error), "Repeated batch", error);
    Check(!memcmp(original.data(), hits.data(), hits.size() * sizeof(HardwareHit)),
          "History-independent hits");
    mesh.indices = {3, 4, 5};
    Check(backend.Build(mesh, error), "Replace geometry", error);
    Check(backend.Trace({rays[0]}, hits, error) && std::abs(hits[0].distance - 20) < .001f,
          "Rebuild replaces old geometry", error);
    mesh.indices[0] = 999;
    Check(!backend.Build(mesh, error), "Reject invalid geometry");
    Check(backend.Trace({rays[0]}, hits, error) && std::abs(hits[0].distance - 20) < .001f,
          "Failed build preserves last valid scene", error);
    HardwareRay invalid = rays[0];
    invalid.direction = {0, 0, 0};
    Check(!backend.Trace({invalid}, hits, error), "Reject degenerate ray");
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        Check(ReadOcclusionMesh(file, mesh, error), "Read map", error);
        auto start = std::chrono::steady_clock::now();
        Check(backend.Build(mesh, error), "Build map DXR", error);
        std::cout
            << "Map triangles=" << mesh.indices.size() / 3 << " buildMs="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
            << '\n';
        rays.clear();
        for (int i = 0; i < 32; ++i) {
            float x = (i + .5f) / 32, y = ((i * 13) % 32 + .5f) / 32;
            rays.push_back({{mesh.low.x + (mesh.high.x - mesh.low.x) * x,
                             mesh.low.y + (mesh.high.y - mesh.low.y) * y, mesh.high.z + 100},
                            .01f,
                            {0, 0, -1},
                            mesh.high.z - mesh.low.z + 200});
        }
        Check(backend.Trace(rays, hits, error), "Trace real map", error);
        unsigned found = 0;
        for (size_t i = 0; i < rays.size(); ++i) {
            SurfaceHit cpu;
            bool hit = SurfaceRaycast(mesh, rays[i].origin, rays[i].direction, rays[i].maximum, cpu);
            Check(hit == (hits[i].distance >= 0), "CPU/DXR hit agreement");
            if (hit) {
                ++found;
                Check(std::abs(cpu.distance - hits[i].distance) < .05f, "CPU/DXR distance agreement");
            }
        }
        std::cout << "Map rays=" << rays.size() << " hits=" << found << " CPU reference passed\n";
    }
    std::cout << "Hardware ray tests passed\n";
}
