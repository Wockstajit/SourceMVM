#pragma once
#include "EnvironmentMaterialMath.h"
#include <limits>
namespace Filmmaker::Environment {
struct RaySurfaceVertex {
    Vec3 position, normal;
    float u = 0, v = 0;
};
struct RaySurface {
    Vec3 position, normal, geometricNormal;
    float u = 0, v = 0;
    uint32_t material = 0;
};
// Primitive IDs from DXR index this exact triangle list. Do not reorder materials separately.
inline bool ResolveRaySurface(const std::vector<RaySurfaceVertex> &vertices,
                              const std::vector<uint32_t> &indices, const std::vector<uint32_t> &materials,
                              uint32_t primitive, float u, float v, RaySurface &out) {
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || v < 0 || u + v > 1.00001f ||
        primitive >= indices.size() / 3 || materials.size() != indices.size() / 3)
        return false;
    size_t offset = size_t(primitive) * 3;
    for (int i = 0; i < 3; ++i)
        if (indices[offset + i] >= vertices.size())
            return false;
    const auto &a = vertices[indices[offset]], &b = vertices[indices[offset + 1]],
               &c = vertices[indices[offset + 2]];
    for (auto *p : {&a, &b, &c})
        if (!Finite(p->position) || !Finite(p->normal) || !std::isfinite(p->u) || !std::isfinite(p->v))
            return false;
    Vec3 geometric = Cross(b.position - a.position, c.position - a.position);
    if (Dot(geometric, geometric) < 1e-12f)
        return false;
    float w = 1 - u - v;
    RaySurface result;
    result.position = a.position * w + b.position * u + c.position * v;
    result.geometricNormal = Normalize(geometric);
    Vec3 normal = a.normal * w + b.normal * u + c.normal * v;
    result.normal = Dot(normal, normal) > 1e-12f ? Normalize(normal) : result.geometricNormal;
    if (Dot(result.normal, result.geometricNormal) < 0)
        result.normal = result.normal * -1;
    result.u = a.u * w + b.u * u + c.u * v;
    result.v = a.v * w + b.v * u + c.v * v;
    result.material = materials[primitive];
    out = result;
    return true;
}
inline float RaySrgbToLinear(float value) {
    return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}
struct RayTextureSample {
    Vec3 color{1, 1, 1};
    float alpha = 1;
};
// Reference implementation for material evaluation tests and offline diagnostics.
// Decode sRGB before filtering; normal/roughness textures pass srgb=false.
inline RayTextureSample SampleRayTexture(const MaterialImage &image, float u, float v, bool srgb = true,
                                         bool repeat = true) {
    if (!ValidMaterialImage(image) || !std::isfinite(u) || !std::isfinite(v))
        return {};
    if (repeat) {
        u -= std::floor(u);
        v -= std::floor(v);
    } else {
        u = std::clamp(u, 0.f, 1.f);
        v = std::clamp(v, 0.f, 1.f);
    }
    float x = u * image.width - .5f, y = v * image.height - .5f;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    auto texel = [&](int px, int py) {
        if (repeat) {
            px = (px % int(image.width) + int(image.width)) % int(image.width);
            py = (py % int(image.height) + int(image.height)) % int(image.height);
        } else {
            px = std::clamp(px, 0, int(image.width) - 1);
            py = std::clamp(py, 0, int(image.height) - 1);
        }
        size_t i = (size_t(py) * image.width + px) * 4;
        RayTextureSample result;
        result.color = {image.rgba[i] / 255.f, image.rgba[i + 1] / 255.f, image.rgba[i + 2] / 255.f};
        result.alpha = image.rgba[i + 3] / 255.f;
        if (srgb)
            result.color = {RaySrgbToLinear(result.color.x), RaySrgbToLinear(result.color.y),
                            RaySrgbToLinear(result.color.z)};
        return result;
    };
    auto a = texel(ix, iy), b = texel(ix + 1, iy), c = texel(ix, iy + 1), d = texel(ix + 1, iy + 1);
    float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    return {a.color * wa + b.color * wb + c.color * wc + d.color * wd,
            a.alpha * wa + b.alpha * wb + c.alpha * wc + d.alpha * wd};
}
} // namespace Filmmaker::Environment
