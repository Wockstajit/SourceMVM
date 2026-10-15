#pragma once

// Small engine-free geometry helpers shared by the map cache writers.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Filmmaker::MapData {

using Float3 = std::array<float, 3>;

// Exact-position key (== semantics: +0 and -0 are the same position).
struct Float3Hash {
    size_t operator()(const Float3 &p) const {
        uint32_t b[3];
        for (int k = 0; k < 3; ++k) {
            const float f = p[k] == 0.f ? 0.f : p[k];
            std::memcpy(&b[k], &f, 4);
        }
        return size_t(b[0]) ^ (size_t(b[1]) * 397u) ^ (size_t(b[2]) * 7919u) ^ (size_t(b[2]) << 32);
    }
};

// Indexed opaque mesh: drops degenerate triangles and shares exactly coincident positions.
struct OccluderBuilder {
    // MVMOCC01 limits the runtime reader accepts (EnvironmentShadowMath.h ReadOcclusionMesh).
    static constexpr size_t kMaxVertices = 16000000, kMaxIndices = 48000000;
    std::vector<Float3> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<Float3, uint32_t, Float3Hash> unique;

    void Add(const Float3 &p) {
        for (float f : p)
            if (std::isnan(f) || std::fabs(f) > 100000.f) throw std::runtime_error("geometry outside runtime bounds");
        auto it = unique.find(p);
        if (it == unique.end()) {
            it = unique.emplace(p, uint32_t(vertices.size())).first;
            vertices.push_back(p);
        }
        indices.push_back(it->second);
    }
    void AddTriangle(const Float3 &a, const Float3 &b, const Float3 &c) {
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (nx * nx + ny * ny + nz * nz <= 1e-8) return;
        Add(a);
        Add(b);
        Add(c);
    }
};

inline void BoundsOf(const std::vector<Float3> &points, Float3 &low, Float3 &high) {
    low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    high = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const Float3 &p : points)
        for (int k = 0; k < 3; ++k) {
            low[k] = std::fmin(low[k], p[k]);
            high[k] = std::fmax(high[k], p[k]);
        }
}

struct ByteWriter {
    std::vector<uint8_t> out;
    void Bytes(const void *data, size_t size) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        out.insert(out.end(), p, p + size);
    }
    template <class T> void Put(T value) { Bytes(&value, sizeof value); }
};

} // namespace Filmmaker::MapData
