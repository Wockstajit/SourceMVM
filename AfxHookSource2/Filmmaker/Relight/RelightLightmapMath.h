#pragma once
// Engine-free description of the lightmap layout file written by
// MapDataTool lightmap (Filmmaker/MapData/MapLightmap), and the arithmetic that reads it back.
//
// Why the layout exists at all: CS2's world shaders read baked irradiance from an atlas addressed
// by a per-vertex lightmap UV. Replacing that irradiance with live lighting means knowing which
// world point every atlas texel belongs to, and that mapping is only in the compiled map.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::Relight {

// 16 bytes, read by the input assembler directly:
//   unorm16 x3 position inside [boundsLow, boundsHigh], unorm16 atlas page,
//   unorm16 x2 atlas uv, snorm16 x2 octahedral normal.
struct LightmapVertex {
    uint16_t position[3], page, uv[2];
    int16_t normal[2];
};
static_assert(sizeof(LightmapVertex) == 16, "the input layout assumes 16-byte vertices");

struct LightmapHeader {
    char magic[8];
    uint32_t version, atlasWidth, atlasHeight, atlasPages;
    uint32_t vertexCount, indexCount, pointCount;
    float uvScale[2], boundsLow[3], boundsHigh[3];
};
static_assert(sizeof(LightmapHeader) == 68, "header layout is part of the file format");

struct LightmapLayout {
    LightmapHeader header{};
    // Triangle vertices first, then one vertex per texel-sized point.
    std::vector<LightmapVertex> vertices;
    std::vector<uint32_t> indices;
    std::string source;
};

// Reads and validates a layout file. Returns an empty string on success, else the reason.
inline std::string ParseLightmapLayout(const uint8_t *bytes, size_t size, LightmapLayout &out) {
    if (size < sizeof(LightmapHeader)) return "truncated header";
    LightmapHeader h;
    std::memcpy(&h, bytes, sizeof(h));
    if (std::memcmp(h.magic, "MVMLMAP2", 8) != 0 || h.version != 2) return "not a lightmap layout (version 2)";
    if (!h.atlasWidth || !h.atlasHeight || h.atlasPages == 0 || h.atlasWidth > 16384 || h.atlasHeight > 16384 ||
        h.atlasPages > 64)
        return "implausible atlas size";
    const uint64_t vertices = uint64_t(h.vertexCount) + h.pointCount;
    const uint64_t expected = sizeof(h) + vertices * sizeof(LightmapVertex) + uint64_t(h.indexCount) * 4;
    if (expected != size) return "size does not match the header";
    if (h.indexCount % 3) return "index count is not a multiple of three";
    for (float v : {h.boundsLow[0], h.boundsLow[1], h.boundsLow[2], h.boundsHigh[0], h.boundsHigh[1], h.boundsHigh[2]})
        if (!std::isfinite(v)) return "non-finite bounds";
    out.header = h;
    out.vertices.resize(size_t(vertices));
    std::memcpy(out.vertices.data(), bytes + sizeof(h), size_t(vertices) * sizeof(LightmapVertex));
    out.indices.resize(h.indexCount);
    std::memcpy(out.indices.data(), bytes + sizeof(h) + size_t(vertices) * sizeof(LightmapVertex), size_t(h.indexCount) * 4);
    for (auto i : out.indices)
        if (i >= h.vertexCount) return "index outside the triangle vertices";
    return {};
}

// Mirror of the shader's decode, for tests: octahedral snorm16 pair back to a unit vector.
inline std::array<float, 3> DecodeOctahedral(int16_t x, int16_t y) {
    float ox = x / 32767.f, oy = y / 32767.f;
    float nx = ox, ny = oy, nz = 1.f - std::fabs(ox) - std::fabs(oy);
    if (nz < 0) {
        const float tx = (1.f - std::fabs(ny)) * (nx >= 0 ? 1.f : -1.f);
        const float ty = (1.f - std::fabs(nx)) * (ny >= 0 ? 1.f : -1.f);
        nx = tx;
        ny = ty;
    }
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    return length > 1e-8f ? std::array<float, 3>{nx / length, ny / length, nz / length} : std::array<float, 3>{0, 0, 1};
}

} // namespace Filmmaker::Relight
