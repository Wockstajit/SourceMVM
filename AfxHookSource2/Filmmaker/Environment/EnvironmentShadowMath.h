#pragma once
#include "EnvironmentMath.h"
#include "EnvironmentCasterMath.h"
#include <memory>
#include <vector>
#include <istream>
#include <string>
#include <cstring>
#include <limits>
namespace Filmmaker::Environment {
// 4096 texels over 128 units keeps first-person receivers from magnifying a coarse map.
inline constexpr float CloseSunHalfExtent = 64.f;
// Bridge the former 16:1 close-to-near jump without sacrificing hand detail or map coverage.
inline constexpr float DetailSunHalfExtent = 256.f;
// The four sun cascades, as light-space half extents around the camera.
//
// The outermost one used to be fitted to the whole map, which pinned the coarsest tier at the
// map's own size however the user set shadowRange: on Mirage that is 9192 units across 4096
// texels, 2.24 world units per texel. Anything thinner than that -- cables, railings, trim --
// occupies less than one texel and casts nothing, and because each fixed tier was 4x the last,
// the quality visibly stepped as the camera moved. Centring the outermost tier on the camera at
// shadowRange instead makes that slider mean something and puts the whole filmed range inside a
// far tighter band; the cost is that geometry beyond shadowRange no longer casts sun shadows,
// which is why the range is a user control and clamps to the map when it is large enough.
//
// Spacing is geometric between close and far so neighbouring tiers differ by one constant ratio
// rather than 4x cliffs. Only `close` is fixed: first-person receivers magnify it most.
struct SunCascadeExtents {
    float close = CloseSunHalfExtent, detail = DetailSunHalfExtent, middle = 1024, outer = 4096;
    // What one texel covers in world units, for the tier that has to carry everything else.
    float OuterTexelWorld(float resolution) const {
        return resolution > 0 ? 2 * outer / resolution : 0;
    }
};
inline SunCascadeExtents SunCascadeHalfExtents(float shadowRange, float mapRadius) {
    SunCascadeExtents e;
    // Never finer than the close tier, and never wider than the map: a range past the map's own
    // size would only spend texels on empty space, and reproduces the old full-map behaviour.
    const float ceiling = mapRadius > 0 ? (std::max)(mapRadius, e.close * 2) : e.close * 64;
    float outer = shadowRange > 0 ? shadowRange * .5f : ceiling;
    if (outer < e.close * 2) outer = e.close * 2;
    if (outer > ceiling) outer = ceiling;
    e.outer = outer;
    const float ratio = std::pow(outer / e.close, 1.f / 3.f);
    e.detail = e.close * ratio;
    e.middle = e.detail * ratio;
    return e;
}
struct OcclusionMesh {
    std::string contentKey; // Validated source/material/payload SHA-256 identities.
    std::string lightMetadataPath; // Verified authored entities; never baked radiance.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    Vec3 low, high;
    std::shared_ptr<const VisualCasters> visual;
    // Miniature 3D skybox in its own native coordinate space. Never enlarges map cascades.
    std::shared_ptr<const OcclusionMesh> skybox;
    float skyboxScale=0;Vec3 skyboxOrigin{}; // Authored sky_camera; 0 keeps legacy whole-sky projection.
};
inline bool ReadOcclusionMesh(std::istream &in, OcclusionMesh &out, std::string &error) {
    char magic[8];
    uint32_t nv = 0, ni = 0;
    OcclusionMesh m;
    in.read(magic, 8);
    in.read(reinterpret_cast<char *>(&nv), 4);
    in.read(reinterpret_cast<char *>(&ni), 4);
    in.read(reinterpret_cast<char *>(&m.low), 12);
    in.read(reinterpret_cast<char *>(&m.high), 12);
    if (!in || std::memcmp(magic, "MVMOCC01", 8) || nv < 3 || nv > 16000000 || ni < 3 || ni > 48000000 ||
        ni % 3 || !Finite(m.low) || !Finite(m.high) || m.low.x > m.high.x || m.low.y > m.high.y ||
        m.low.z > m.high.z) {
        error = "Invalid map occlusion header";
        return false;
    }
    m.positions.resize(nv);
    m.indices.resize(ni);
    in.read(reinterpret_cast<char *>(m.positions.data()), nv * sizeof(Vec3));
    in.read(reinterpret_cast<char *>(m.indices.data()), ni * sizeof(uint32_t));
    if (!in || in.peek() != std::char_traits<char>::eof()) {
        error = "Invalid map occlusion length";
        return false;
    }
    for (auto p : m.positions)
        if (!Finite(p) || p.x < m.low.x - .01f || p.y < m.low.y - .01f || p.z < m.low.z - .01f ||
            p.x > m.high.x + .01f || p.y > m.high.y + .01f || p.z > m.high.z + .01f ||
            std::abs(p.x) > 100000 || std::abs(p.y) > 100000 || std::abs(p.z) > 100000) {
            error = "Invalid map occlusion position";
            return false;
        }
    for (auto i : m.indices)
        if (i >= nv) {
            error = "Invalid map occlusion index";
            return false;
        }
    out = std::move(m);
    error.clear();
    return true;
}
inline Matrix ShadowView(Vec3 origin, Vec3 direction) {
    Vec3 f = Normalize(direction),
         r = Normalize(Cross(std::abs(f.z) > .95f ? Vec3{0, 1, 0} : Vec3{0, 0, 1}, f)), u = Cross(f, r);
    return {r.x, r.y, r.z, -Dot(r, origin), u.x, u.y, u.z, -Dot(u, origin),
            f.x, f.y, f.z, -Dot(f, origin), 0,   0,   0,   1};
}
inline Matrix LocalShadowMatrix(Vec3 origin, Vec3 direction, float cone, float range,float nearClip=1.f) {
    float f=(std::max)(range,16.f),n=std::isfinite(nearClip)?std::clamp(nearClip,1.f,f*.25f):1.f;
    float s = 1 / std::tan(Radians(cone * .5f));
    Matrix p = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, f / (f - n), -n * f / (f - n), 0, 0, 1, 0};
    return Multiply(p, ShadowView(origin, direction));
}
inline int PointShadowFace(Vec3 v) {
    float x = std::abs(v.x), y = std::abs(v.y), z = std::abs(v.z);
    if (x >= y && x >= z)
        return v.x >= 0 ? 0 : 1;
    if (y >= z)
        return v.y >= 0 ? 2 : 3;
    return v.z >= 0 ? 4 : 5;
}
inline Vec3 PointShadowDirection(int face) {
    static const Vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    return directions[std::clamp(face, 0, 5)];
}
// Fits the complete map, not a camera frustum. Camera rotation cannot invalidate this projection.
inline Matrix SunShadowMatrix(Vec3 low, Vec3 high, Vec3 sun) {
    Vec3 center = (low + high) * .5f;
    float radius = std::sqrt(Dot(high - low, high - low)) * .5f + 128;
    Matrix view = ShadowView(center + Normalize(sun) * (radius + 128), sun * -1);
    Vec3 a{1e20f, 1e20f, 1e20f}, b{-1e20f, -1e20f, -1e20f};
    for (int i = 0; i < 8; ++i) {
        Vec3 p = Transform(view, {i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z});
        a = {(std::min)(a.x, p.x), (std::min)(a.y, p.y), (std::min)(a.z, p.z)};
        b = {(std::max)(b.x, p.x), (std::max)(b.y, p.y), (std::max)(b.z, p.z)};
    }
    a = a - Vec3{32, 32, 32};
    b = b + Vec3{32, 32, 32};
    Matrix projection = {2 / (b.x - a.x),
                         0,
                         0,
                         -(b.x + a.x) / (b.x - a.x),
                         0,
                         2 / (b.y - a.y),
                         0,
                         -(b.y + a.y) / (b.y - a.y),
                         0,
                         0,
                         1 / (b.z - a.z),
                         -a.z / (b.z - a.z),
                         0,
                         0,
                         0,
                         1};
    return Multiply(projection, view);
}
// Concentrate surface detail around the camera without culling offscreen/upstream casters.
// Keep the full map's depth interval, and crop only the light-space XY projection. A coarse
// center snap avoids rebuilding millions of static triangles for subpixel camera movement;
// the shader blends to the full-map projection before reaching this region's boundary.
// With the map's resolution given, the snap is a whole number of texels: a step that is not
// moves every rasterised edge by a fraction of a texel and the shadow shimmers as the camera
// travels (a 524-unit tier at 2048 texels would otherwise step 62.5 texels at a time).
inline Matrix NearSunShadowMatrix(const Matrix &farMatrix, Vec3 camera, float halfExtent = 1024,
                                  float resolution = 0) {
    Matrix result = farMatrix;
    halfExtent = (std::max)(halfExtent, 64.f);
    float snap = (std::min)(32.f, halfExtent * .125f);
    if (resolution > 0) {
        const float texel = 2 * halfExtent / resolution;
        snap = (std::max)(1.f, std::floor(snap / texel)) * texel;
    }
    for (int row = 0; row < 2; ++row) {
        const int offset = row * 4;
        Vec3 axis = Normalize({farMatrix[offset], farMatrix[offset + 1], farMatrix[offset + 2]});
        const float center = std::floor(Dot(axis, camera) / snap + .5f) * snap;
        result[offset] = axis.x / halfExtent;
        result[offset + 1] = axis.y / halfExtent;
        result[offset + 2] = axis.z / halfExtent;
        result[offset + 3] = -center / halfExtent;
    }
    return result;
}
} // namespace Filmmaker::Environment
