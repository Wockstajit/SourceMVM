#pragma once
// The wind fluid's grid and what stands in the wind's way (EnvironmentWindFluid.h). Engine-free.
#include "EnvironmentMath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::Environment {
// Eight units a cell: what the wind carries is drawn as it is, with no pattern laid over it, so the
// grid itself has to hold the wisps and curls. (At 24 units it held only broad patches, and a
// second, drawn pattern was faded in and out over them to look like detail; that pulsed.)
inline constexpr float kWindMinCell = 8.f;  // world units a cell on an ordinary map
inline constexpr int kWindMaxCells = 1024;  // a larger map gets wider cells, not more of them
struct WindGrid {
    float cell = 0, originX = 0, originY = 0;
    int width = 0, height = 0;
    bool Valid() const { return width > 8 && height > 8 && cell > 0; }
    float CentreX(int x) const { return originX + (float(x) + .5f) * cell; }
    float CentreY(int y) const { return originY + (float(y) + .5f) * cell; }
};
// One grid over the whole map, with a margin of open cells round it for the wind to come in by.
inline WindGrid WindGridForMap(Vec3 low, Vec3 high) {
    WindGrid g;
    if (!(high.x > low.x) || !(high.y > low.y) || !std::isfinite(high.x - low.x) || !std::isfinite(high.y - low.y)) return g;
    const int margin = 4;
    const float extent = (std::max)(high.x - low.x, high.y - low.y);
    g.cell = (std::max)(kWindMinCell, extent / float(kWindMaxCells - 2 * margin));
    g.originX = low.x - g.cell * margin;
    g.originY = low.y - g.cell * margin;
    g.width = (std::min)(kWindMaxCells, int(std::ceil((high.x - low.x) / g.cell)) + 2 * margin);
    g.height = (std::min)(kWindMaxCells, int(std::ceil((high.y - low.y) / g.cell)) + 2 * margin);
    return g;
}
// WHAT STANDS IN THE WIND'S WAY. `tops` is, for each cell, the height below which the sky is shut
// off (a street's own ground; a building's roof). A cell is a WALL when its top stands `rise` units
// over the lowest top within `reach` units, and `rise` is taller than a person: the fluid is the
// air people stand in, and what it carries is drawn skimming the ground. So the faces of a house on a
// street and a yard wall are solid, three cells deep, and the street is not; nor is a slope, a
// stair, a kerb or a waist-high ledge, which the wind blows over. (At a house's height only, blown
// snow ran straight through every yard wall; measured against lower ground further off, most of a
// hilly map counted as building.) A house's inside is left open but
// walled in.
inline constexpr float kWindObstacleRise = 96.f, kWindObstacleReach = 72.f;
inline std::vector<uint8_t> WindObstacles(const std::vector<float> &tops, int width, int height, float cell, float rise = kWindObstacleRise,
                                          float reach = kWindObstacleReach) {
    std::vector<uint8_t> solid(tops.size(), 0);
    if (width <= 0 || height <= 0 || tops.size() != size_t(width) * height) return solid;
    const int r = (std::max)(1, int(std::ceil(reach / cell)));
    // The lowest top within reach, as two passes (along rows, then along columns).
    std::vector<float> rows(tops.size()), lowest(tops.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float m = tops[size_t(y) * width + x];
            for (int k = (std::max)(0, x - r); k <= (std::min)(width - 1, x + r); ++k) m = (std::min)(m, tops[size_t(y) * width + k]);
            rows[size_t(y) * width + x] = m;
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float m = rows[size_t(y) * width + x];
            for (int k = (std::max)(0, y - r); k <= (std::min)(height - 1, y + r); ++k) m = (std::min)(m, rows[size_t(k) * width + x]);
            lowest[size_t(y) * width + x] = m;
        }
    for (size_t i = 0; i < tops.size(); ++i) solid[i] = tops[i] > lowest[i] + rise ? 255 : 0;
    return solid;
}

// THE MAP THE FLUID READS: 255 for a wall (WindObstacles), and for open air how open the ground around
// it is, 0..127 for 0..1: the share within kWindOpenRadius that is not under a roof. A roof here is a
// top standing `rise` over the lowest ground within kWindRoofReach, which takes in a whole house, not
// just its faces. The fluid is 2D, so every gust that crosses a block of houses had to squeeze through
// its streets: one street through the middle of Dust2 (the catwalk) carried a steady jet at the speed
// cap while every other street was still (reported 2026-11-18). Real air also goes over the roofs, so
// among houses the fluid only partly keeps its volume (Divergence), and no one street has to carry it.
inline constexpr float kWindRoofReach = 320.f, kWindOpenRadius = 240.f;
inline std::vector<uint8_t> WindAirMap(const std::vector<float> &tops, int width, int height, float cell) {
    if (width <= 0 || height <= 0 || cell <= 0 || tops.size() != size_t(width) * height) return std::vector<uint8_t>(tops.size(), 127);
    std::vector<uint8_t> map = WindObstacles(tops, width, height, cell);
    const std::vector<uint8_t> roofed = WindObstacles(tops, width, height, cell, kWindObstacleRise, kWindRoofReach);
    // A summed-area table of the roofed cells: the share in any square is four reads.
    const size_t stride = size_t(width) + 1;
    std::vector<uint32_t> sum(stride * (size_t(height) + 1), 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            sum[(y + 1) * stride + x + 1] = (roofed[size_t(y) * width + x] ? 1u : 0u) + sum[y * stride + x + 1] + sum[(y + 1) * stride + x] - sum[y * stride + x];
    const int r = (std::max)(1, int(std::lround(kWindOpenRadius / cell)));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint8_t &m = map[size_t(y) * width + x];
            if (m) continue;
            const int x0 = (std::max)(0, x - r), x1 = (std::min)(width, x + r + 1), y0 = (std::max)(0, y - r), y1 = (std::min)(height, y + r + 1);
            const uint32_t covered = sum[y1 * stride + x1] - sum[y0 * stride + x1] - sum[y1 * stride + x0] + sum[y0 * stride + x0];
            const float open = 1.f - float(covered) / float((x1 - x0) * (y1 - y0));
            m = uint8_t(std::lround(std::clamp(open, 0.f, 1.f) * 127.f));
        }
    return map;
}

// THE NOISE BLOWN SNOW, SPRAY AND ASH ARE TORN BY (GustVolumeMarch), which the volume's flow map
// drags along with the air: a Perlin-Worley volume (Schneider 2015: billows from value noise, cut into cauliflower lumps by
// inverted cell noise) that repeats seamlessly every `size` texels in all three directions. 0..255.
namespace detail {
inline uint32_t NoiseHash(int x, int y, int z, uint32_t seed) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
inline float NoiseUnit(uint32_t h) { return float(h & 0xffffff) / 16777216.f; }
inline int Wrap(int v, int period) { return ((v % period) + period) % period; }
// Value noise with `period` lattice cells across the volume, at volume coordinate p (0..1).
inline float TiledValue(float px, float py, float pz, int period, uint32_t seed) {
    const float x = px * period, y = py * period, z = pz * period;
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    auto s = [](float t) { return t * t * (3 - 2 * t); };
    const float fx = s(x - ix), fy = s(y - iy), fz = s(z - iz);
    auto at = [&](int dx, int dy, int dz) {
        return NoiseUnit(NoiseHash(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed));
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(at(0, 0, 0), at(1, 0, 0), fx), lerp(at(0, 1, 0), at(1, 1, 0), fx), fy),
                lerp(lerp(at(0, 0, 1), at(1, 0, 1), fx), lerp(at(0, 1, 1), at(1, 1, 1), fx), fy), fz);
}
// Inverted cell noise: 1 at a cell's feature point, falling toward its edges.
inline float TiledWorley(float px, float py, float pz, int period, uint32_t seed) {
    const float x = px * period, y = py * period, z = pz * period;
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    float best = 9;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                const uint32_t h = NoiseHash(Wrap(cx, period), Wrap(cy, period), Wrap(cz, period), seed);
                const float fx = cx + NoiseUnit(h) - x, fy = cy + NoiseUnit(h * 747796405u + 1) - y, fz = cz + NoiseUnit(h * 2891336453u + 7) - z;
                best = (std::min)(best, fx * fx + fy * fy + fz * fz);
            }
    return 1 - (std::min)(std::sqrt(best), 1.f);
}
} // namespace detail
inline std::vector<uint8_t> GustNoiseVolume(int size = 64) {
    std::vector<uint8_t> out;
    if (size < 4) return out;
    out.resize(size_t(size) * size * size);
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float px = (x + .5f) / size, py = (y + .5f) / size, pz = (z + .5f) / size;
                const float billow = detail::TiledValue(px, py, pz, 4, 1) * .53f + detail::TiledValue(px, py, pz, 8, 2) * .27f +
                                     detail::TiledValue(px, py, pz, 16, 3) * .13f + detail::TiledValue(px, py, pz, 32, 4) * .07f;
                const float cells = detail::TiledWorley(px, py, pz, 4, 5) * .625f + detail::TiledWorley(px, py, pz, 8, 6) * .25f +
                                    detail::TiledWorley(px, py, pz, 16, 7) * .125f;
                // Remap the billows into the cells (Schneider's remap(perlin, 0, 1, worley, 1)), then
                // spread the result over the whole range.
                float v = cells + billow * (1 - cells);
                v = (std::min)((std::max)((v - .45f) / .5f, 0.f), 1.f);
                out[(size_t(z) * size + y) * size + x] = uint8_t(v * 255.f + .5f);
            }
    return out;
}
} // namespace Filmmaker::Environment
