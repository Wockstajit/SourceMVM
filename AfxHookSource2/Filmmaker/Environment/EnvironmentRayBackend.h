#pragma once
#include "EnvironmentShadowMath.h"
#include <dxgi.h>
#include <memory>
namespace Filmmaker::Environment {
struct HardwareRay {
    Vec3 origin;
    float minimum = .01f;
    Vec3 direction;
    float maximum = 32768;
};
struct HardwareHit {
    float distance = -1;
    uint32_t primitive = UINT32_MAX;
    float u = 0, v = 0;
};
// Dedicated worker-thread backend. No calls may run on the engine render/UI thread.
// Geometry is immutable until Build is called again; tracing does not depend on a camera.
class RayBackend {
  public:
    RayBackend();
    ~RayBackend();
    RayBackend(const RayBackend &) = delete;
    RayBackend &operator=(const RayBackend &) = delete;
    bool Begin(IDXGIAdapter *adapter, std::string &error);
    bool Build(const OcclusionMesh &mesh, std::string &error);
    bool Trace(const std::vector<HardwareRay> &rays, std::vector<HardwareHit> &hits, std::string &error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Filmmaker::Environment
