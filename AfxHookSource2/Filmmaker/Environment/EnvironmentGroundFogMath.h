#pragma once
// GROUND FOG: the engine-free half of the low fog layer that lies on the floor and is pushed aside
// by players. EnvironmentGroundFog.cpp runs it on the GPU as a 2D fluid (Stable Fluids) over the
// whole map; VolumeAt in EnvironmentClimateShaders.h marches the layer.
//
// Three pieces live here so they can be tested without a device:
//   * the FLOOR MAP: for every map column, the heights of up to four walkable floors, built once per
//     map from the map collision. The fog's thickness is measured from the floor under each point,
//     so it hugs slopes, stairs, rooms and upper floors instead of sitting at one world height.
//   * the FLUID GRID: the map's extent cut into square cells, fixed for the map.
//   * the BODIES and the CLOCK: player velocities from successive positions, and how much demo time
//     a frame simulates (nothing while paused; a seek restarts the fog from rest).
#include "EnvironmentMath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <utility>
#include <vector>

namespace Filmmaker::Environment {

inline constexpr int kGroundFogFloorLayers = 4;
// Marks an unused floor slot. Far below any map, so "highest floor at or below z" skips it.
inline constexpr float kGroundFogNoFloor = -1.0e6f;
// The fluid grid: square cells of at least 8 units (a player, 32 units across, spans four), at most
// 1024 per side, so a large map gets coarser cells rather than a larger bill.
inline constexpr int kGroundFogMaxCells = 1024;
inline constexpr float kGroundFogMinCell = 8.f;
// Same bound as Frame::kMaxActors: the bodies ride the frame constants.
inline constexpr int kGroundFogBodies = 12;
// Blasts, fires and smokes clearing the fog at once.
inline constexpr int kGroundFogBlasts = 16;

struct GroundFogFloorSettings {
    float cell = 16;      // world units per floor-map texel
    int maxCells = 1024;  // per axis; a larger map gets coarser texels
    float minUp = .64f;   // walkable faces: normal.z of at least this (slopes up to about 50 degrees)
    float sameFloor = 24; // heights closer than this in one column are the same floor (a slab's two faces)
    float stepUp = 18;    // a neighbour this much higher is still the same floor (a stair step)
    // WHERE PEOPLE WALK. Fog lies only on floors a player can stand on, and a map's nav mesh says exactly
    // which those are. It stops short of walls by a player's width, so a floor within `navReach` of it
    // counts, at about the same height: `navHeight` over it, plus a little per unit away for a slope.
    float navReach = 48;
    float navHeight = 24;
    // A map with no nav mesh (a Workshop map, say) falls back on geometry: floors joined by steps of at
    // most `connectStep` form a piece, and one under `minPiece` of the largest is dropped, as a roof is.
    // That is a guess. It keeps a roof that happens to step down to the street and drops a raised street.
    float connectStep = 36;
    float minPiece = .12f;
    // On any map, a piece smaller than this (square units) is a crate or a wall top: its top stands out
    // of the fog instead of carrying a layer of its own.
    float minArea = 40000;
    // Fog thins toward a sheer edge within `drainReach`, one with nothing to step onto and a floor at
    // least `drainDrop` below, so it ends softly at a ledge. Stairs and ramps are not edges.
    float drainDrop = 60;
    float drainReach = 96;
    int smooth = 1;       // same-floor blur radius in texels, so cell steps do not show in the fog top
};

struct GroundFogFloorMap {
    float originX = 0, originY = 0, cell = 16;
    int width = 0, height = 0;
    // Per texel, highest first; unused slots hold kGroundFogNoFloor.
    std::vector<std::array<float, kGroundFogFloorLayers>> layers;
    // How much of the layer's depth each floor holds, 0 to 1 (see drainDrop). Same order as `layers`.
    std::vector<std::array<float, kGroundFogFloorLayers>> thickness;
    bool Valid() const {
        return width > 0 && height > 0 && cell > 0 && layers.size() == size_t(width) * size_t(height);
    }
    const std::array<float, kGroundFogFloorLayers> &At(int x, int y) const {
        return layers[size_t(y) * size_t(width) + size_t(x)];
    }
};

// The highest floor of one column that is at or below `z`, or kGroundFogNoFloor.
inline float GroundFogLayerBelow(const std::array<float, kGroundFogFloorLayers> &column, float z) {
    for (float h : column)
        if (h > kGroundFogNoFloor * .5f && h <= z) return h;
    return kGroundFogNoFloor;
}

// Collapses a column's sorted (highest first) heights into floors and keeps at most four. When a
// column has more, the three highest and the lowest survive: the ground is what fog lies on most.
inline std::array<float, kGroundFogFloorLayers> GroundFogMergeColumn(std::vector<float> &heights, float sameFloor) {
    std::array<float, kGroundFogFloorLayers> out;
    out.fill(kGroundFogNoFloor);
    if (heights.empty()) return out;
    std::sort(heights.begin(), heights.end(), [](float a, float b) { return a > b; });
    std::vector<float> floors;
    float last = heights.front();
    floors.push_back(last);
    for (size_t i = 1; i < heights.size(); ++i) {
        // Chained: a slab's top, its underside and a decal sheet just above are one floor, and the
        // floor keeps its TOP height (the one fog lies on).
        if (last - heights[i] >= sameFloor) floors.push_back(heights[i]);
        last = heights[i];
    }
    if (floors.size() > size_t(kGroundFogFloorLayers)) {
        const float lowest = floors.back();
        floors.resize(kGroundFogFloorLayers - 1);
        floors.push_back(lowest);
    }
    for (size_t i = 0; i < floors.size(); ++i) out[i] = floors[i];
    return out;
}

// Drops every floor in a piece too small to count (see connectStep, minPiece and minArea). The largest
// piece always stays. `minCells` is minArea in texels.
inline void GroundFogKeepPieces(std::vector<std::array<float, kGroundFogFloorLayers>> &columns, int width, int height,
                                float connectStep, float minPiece, float minCells) {
    const size_t nodes = columns.size() * kGroundFogFloorLayers;
    std::vector<uint32_t> parent(nodes);
    for (size_t i = 0; i < nodes; ++i) parent[i] = uint32_t(i);
    const auto find = [&](uint32_t a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    const auto joinColumns = [&](size_t a, size_t b) {
        for (int i = 0; i < kGroundFogFloorLayers; ++i) {
            const float ha = columns[a][i];
            if (ha <= kGroundFogNoFloor * .5f) continue;
            for (int j = 0; j < kGroundFogFloorLayers; ++j) {
                const float hb = columns[b][j];
                if (hb <= kGroundFogNoFloor * .5f || std::abs(ha - hb) > connectStep) continue;
                parent[find(uint32_t(a * kGroundFogFloorLayers + i))] = find(uint32_t(b * kGroundFogFloorLayers + j));
            }
        }
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t c = size_t(y) * width + x;
            if (x + 1 < width) joinColumns(c, c + 1);
            if (y + 1 < height) joinColumns(c, c + width);
        }
    std::vector<uint32_t> size(nodes, 0);
    uint32_t largest = 0;
    for (size_t c = 0; c < columns.size(); ++c)
        for (int i = 0; i < kGroundFogFloorLayers; ++i)
            if (columns[c][i] > kGroundFogNoFloor * .5f) largest = (std::max)(largest, ++size[find(uint32_t(c * kGroundFogFloorLayers + i))]);
    const float keep = (std::min)(float(largest), (std::max)(float(largest) * minPiece, minCells));
    for (size_t c = 0; c < columns.size(); ++c) {
        std::array<float, kGroundFogFloorLayers> kept;
        kept.fill(kGroundFogNoFloor);
        int n = 0;
        for (int i = 0; i < kGroundFogFloorLayers; ++i)
            if (columns[c][i] > kGroundFogNoFloor * .5f && float(size[find(uint32_t(c * kGroundFogFloorLayers + i))]) >= keep)
                kept[n++] = columns[c][i];
        columns[c] = kept;
    }
}

// Reads the walkable polygons out of a CS2 nav file into triangles. The layout is read from the game's
// own files (major versions 35 and 36; the header differs between them):
//   u32 0xFEEDFACE, u32 major, u32 minor, header..., u32 vertexCount, vertexCount float3s, u32 polygonCount,
//   polygonCount { u8 sides, sides u32 vertex indices, u32 }, then area data this does not need.
// The header's length is not read, the table is found: the first place where a vertex table and a polygon
// table both check out to their last entry. Any future layout that fails this leaves the map with no nav
// mesh, which only costs it the geometric fallback.
inline bool ParseGroundFogNav(const uint8_t *data, size_t size, std::vector<Vec3> &positions, std::vector<uint32_t> &triangles) {
    positions.clear();
    triangles.clear();
    const auto u32At = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, data + at, 4);
        return v;
    };
    if (!data || size < 32 || u32At(0) != 0xFEEDFACEu) return false;
    const size_t scanEnd = (std::min)(size_t(4096), size - 8);
    for (size_t table = 12; table < scanEnd; ++table) {
        const uint32_t vertexCount = u32At(table);
        if (vertexCount < 3 || vertexCount > 4000000) continue;
        const size_t vertices = table + 4, polygons = vertices + size_t(vertexCount) * 12;
        if (polygons + 4 > size) continue;
        const uint32_t polygonCount = u32At(polygons);
        if (polygonCount < 1 || polygonCount > vertexCount * 4) continue;
        std::vector<Vec3> points(vertexCount);
        std::memcpy(points.data(), data + vertices, size_t(vertexCount) * 12);
        bool good = true;
        for (const auto &p : points) good = good && Finite(p) && std::abs(p.x) < 65536 && std::abs(p.y) < 65536 && std::abs(p.z) < 65536;
        if (!good) continue;
        std::vector<uint32_t> fan;
        size_t at = polygons + 4;
        for (uint32_t i = 0; i < polygonCount && good; ++i) {
            if (at >= size) { good = false; break; }
            const uint32_t sides = data[at++];
            if (sides < 3 || sides > 64 || at + size_t(sides) * 4 + 4 > size) { good = false; break; }
            const uint32_t first = u32At(at);
            for (uint32_t k = 0; k < sides && good; ++k) good = u32At(at + size_t(k) * 4) < vertexCount;
            for (uint32_t k = 2; k < sides && good; ++k) {
                fan.push_back(first);
                fan.push_back(u32At(at + size_t(k - 1) * 4));
                fan.push_back(u32At(at + size_t(k) * 4));
            }
            at += size_t(sides) * 4 + 4;
        }
        if (!good) continue;
        positions = std::move(points);
        triangles = std::move(fan);
        return true;
    }
    return false;
}

// Keeps the floors the nav mesh says people stand on, and adds its own floor where collision has none
// at that height, so fog is never missing from a place people walk.
inline void GroundFogKeepNavFloors(std::vector<std::array<float, kGroundFogFloorLayers>> &columns,
                                   const std::vector<std::array<float, kGroundFogFloorLayers>> &nav, int width, int height,
                                   float cell, const GroundFogFloorSettings &s) {
    const auto isFloor = [](float h) { return h > kGroundFogNoFloor * .5f; };
    struct Offset { int dx, dy; float distance; };
    std::vector<Offset> offsets;
    const int reach = (std::max)(0, int(std::ceil(s.navReach / cell)));
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
            const float d = std::sqrt(float(dx * dx + dy * dy)) * cell;
            if (d <= s.navReach) offsets.push_back({dx, dy, d});
        }
    std::sort(offsets.begin(), offsets.end(), [](const Offset &a, const Offset &b) { return a.distance < b.distance; });
    std::vector<float> heights;
    std::vector<std::array<float, kGroundFogFloorLayers>> out(columns.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t c = size_t(y) * width + x;
            heights.clear();
            for (const float h : columns[c]) {
                if (!isFloor(h)) continue;
                bool walked = false;
                for (const auto &o : offsets) {
                    const int nx = x + o.dx, ny = y + o.dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    for (const float g : nav[size_t(ny) * width + nx])
                        walked = walked || (isFloor(g) && std::abs(g - h) <= s.navHeight + o.distance * .25f);
                    if (walked) break;
                }
                if (walked) heights.push_back(h);
            }
            for (const float g : nav[c]) {
                if (!isFloor(g)) continue;
                bool covered = false;
                for (const float h : heights) covered = covered || std::abs(g - h) <= s.navHeight;
                if (!covered) heights.push_back(g);
            }
            out[c] = GroundFogMergeColumn(heights, s.sameFloor);
        }
    columns.swap(out);
}

// Rasterises the walkable faces of a triangle soup (the map collision) into a floor map, keeping those
// the nav mesh says people stand on. A map with no nav mesh passes none. Pure function; the renderer runs
// it on a worker once per map.
inline GroundFogFloorMap BuildGroundFogFloorMap(const std::vector<Vec3> &positions, const std::vector<uint32_t> &indices,
                                                Vec3 low, Vec3 high, const GroundFogFloorSettings &s = {},
                                                const std::vector<Vec3> &navPositions = {},
                                                const std::vector<uint32_t> &navIndices = {}) {
    GroundFogFloorMap m;
    if (positions.empty() || indices.size() < 3 || !Finite(low) || !Finite(high) || !(high.x > low.x) ||
        !(high.y > low.y) || s.cell <= 0)
        return m;
    const float extent = (std::max)(high.x - low.x, high.y - low.y);
    m.cell = (std::max)(s.cell, extent / float((std::max)(8, s.maxCells - 2)));
    // One spare texel on every side, so a lookup at the map's edge still has a neighbour to blend.
    m.originX = low.x - m.cell;
    m.originY = low.y - m.cell;
    m.width = int(std::ceil((high.x - low.x) / m.cell)) + 2;
    m.height = int(std::ceil((high.y - low.y) / m.cell)) + 2;
    const auto cellOf = [&](float x, float y, int &i, int &j) {
        i = int(std::floor((x - m.originX) / m.cell));
        j = int(std::floor((y - m.originY) / m.cell));
        return i >= 0 && j >= 0 && i < m.width && j < m.height;
    };
    const auto columnsOf = [&](const std::vector<Vec3> &points, const std::vector<uint32_t> &triangles, float minUp, bool eitherFacing) {
        std::vector<std::pair<uint32_t, float>> samples;
        samples.reserve(size_t(m.width) * size_t(m.height) * 2);
        for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
            const uint32_t ia = triangles[t], ib = triangles[t + 1], ic = triangles[t + 2];
            if (ia >= points.size() || ib >= points.size() || ic >= points.size()) continue;
            Vec3 a = points[ia], b = points[ib], c = points[ic];
            if (!Finite(a) || !Finite(b) || !Finite(c)) continue;
            Vec3 n = Cross(b - a, c - a);
            if (eitherFacing && n.z < 0) {
                std::swap(b, c);
                n = Cross(b - a, c - a);
            }
            const float length = std::sqrt(Dot(n, n));
            // Upward faces only: undersides (ceilings, the bottom of a sky-high slab) are not floors.
            if (length < 1e-6f || n.z < minUp * length) continue;
            const float area2 = n.z; // twice the triangle's area as seen from above; > 0 here
            const float minX = (std::min)({a.x, b.x, c.x}), maxX = (std::max)({a.x, b.x, c.x});
            const float minY = (std::min)({a.y, b.y, c.y}), maxY = (std::max)({a.y, b.y, c.y});
            const int i0 = (std::max)(0, int(std::ceil((minX - m.originX) / m.cell - .5f)));
            const int i1 = (std::min)(m.width - 1, int(std::floor((maxX - m.originX) / m.cell - .5f)));
            const int j0 = (std::max)(0, int(std::ceil((minY - m.originY) / m.cell - .5f)));
            const int j1 = (std::min)(m.height - 1, int(std::floor((maxY - m.originY) / m.cell - .5f)));
            bool any = false;
            for (int j = j0; j <= j1; ++j) {
                const float py = m.originY + (float(j) + .5f) * m.cell;
                for (int i = i0; i <= i1; ++i) {
                    const float px = m.originX + (float(i) + .5f) * m.cell;
                    // Edge functions, normalised by the projected area: barycentric weights of a, b, c.
                    const float wa = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) / area2;
                    const float wb = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) / area2;
                    const float wc = 1 - wa - wb;
                    if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f) continue;
                    samples.push_back({uint32_t(j * m.width + i), wa * a.z + wb * b.z + wc * c.z});
                    any = true;
                }
            }
            // A face smaller than a texel covers no texel centre; keep it at its centroid so thin
            // walkways and small platforms do not vanish from the map.
            if (!any) {
                const float cx = (a.x + b.x + c.x) / 3, cy = (a.y + b.y + c.y) / 3;
                int i = 0, j = 0;
                if (cellOf(cx, cy, i, j)) samples.push_back({uint32_t(j * m.width + i), (a.z + b.z + c.z) / 3});
            }
        }
        std::sort(samples.begin(), samples.end(), [](const auto &l, const auto &r) {
            return l.first != r.first ? l.first < r.first : l.second > r.second;
        });
        std::vector<std::array<float, kGroundFogFloorLayers>> out(size_t(m.width) * size_t(m.height));
        for (auto &column : out) column.fill(kGroundFogNoFloor);
        std::vector<float> heights;
        for (size_t k = 0; k < samples.size();) {
            const uint32_t cell = samples[k].first;
            heights.clear();
            while (k < samples.size() && samples[k].first == cell) heights.push_back(samples[k++].second);
            out[cell] = GroundFogMergeColumn(heights, s.sameFloor);
        }
        return out;
    };
    auto raw = columnsOf(positions, indices, s.minUp, false);
    const bool navigable = !navIndices.empty();
    if (navigable) GroundFogKeepNavFloors(raw, columnsOf(navPositions, navIndices, .02f, true), m.width, m.height, m.cell, s);
    GroundFogKeepPieces(raw, m.width, m.height, s.connectStep, navigable ? 0.f : s.minPiece, s.minArea / (m.cell * m.cell));
    // SMOOTH: average each floor with its neighbours' matching floor, so the fog top follows a
    // gentle surface rather than the texel grid.
    m.layers.assign(raw.size(), {});
    const float match = s.sameFloor + s.stepUp;
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x) {
            auto &out = m.layers[size_t(y) * m.width + x];
            out = raw[size_t(y) * m.width + x];
            for (float &h : out) {
                if (h <= kGroundFogNoFloor * .5f) continue;
                float sum = 0, weight = 0;
                for (int dy = -s.smooth; dy <= s.smooth; ++dy)
                    for (int dx = -s.smooth; dx <= s.smooth; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height) continue;
                        const float g = GroundFogLayerBelow(raw[size_t(ny) * m.width + nx], h + match);
                        if (g <= kGroundFogNoFloor * .5f || h - g > match) continue;
                        const float w = (dx == 0 && dy == 0) ? 2.f : 1.f;
                        sum += g * w;
                        weight += w;
                    }
                if (weight > 0) h = sum / weight;
            }
        }
    // DRAIN: the distance from each floor to the nearest sheer edge sets how much fog it holds.
    const auto isFloor = [](float h) { return h > kGroundFogNoFloor * .5f; };
    std::vector<std::array<bool, kGroundFogFloorLayers>> ledge(m.layers.size());
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            for (int l = 0; l < kGroundFogFloorLayers; ++l) {
                const float h = m.At(x, y)[l];
                if (!isFloor(h)) continue;
                const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                bool edge = false;
                for (const auto &step : steps) {
                    const int nx = x + step[0], ny = y + step[1];
                    if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height) continue;
                    const auto &column = m.At(nx, ny);
                    bool joined = false;
                    for (const float g : column) joined = joined || (isFloor(g) && std::abs(g - h) <= s.connectStep);
                    const float below = GroundFogLayerBelow(column, h + s.stepUp);
                    edge = edge || (!joined && isFloor(below) && h - below >= s.drainDrop);
                }
                ledge[size_t(y) * m.width + x][l] = edge;
            }
    struct Offset { int dx, dy; float distance; };
    std::vector<Offset> offsets;
    const int drainTexels = (std::max)(1, int(std::ceil(s.drainReach / m.cell)));
    for (int dy = -drainTexels; dy <= drainTexels; ++dy)
        for (int dx = -drainTexels; dx <= drainTexels; ++dx) {
            const float d = std::sqrt(float(dx * dx + dy * dy)) * m.cell;
            if (d <= s.drainReach) offsets.push_back({dx, dy, d});
        }
    std::sort(offsets.begin(), offsets.end(), [](const Offset &a, const Offset &b) { return a.distance < b.distance; });
    m.thickness.assign(m.layers.size(), {});
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x) {
            const auto &column = m.At(x, y);
            auto &held = m.thickness[size_t(y) * m.width + x];
            held.fill(1.f);
            for (int l = 0; l < kGroundFogFloorLayers; ++l) {
                const float h = column[l];
                if (!isFloor(h)) continue;
                bool found = false;
                for (const auto &o : offsets) {
                    const int nx = x + o.dx, ny = y + o.dy;
                    if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height) continue;
                    const auto &around = m.At(nx, ny);
                    const auto &edges = ledge[size_t(ny) * m.width + nx];
                    for (int k = 0; k < kGroundFogFloorLayers && !found; ++k)
                        found = edges[k] && std::abs(around[k] - h) <= s.drainDrop;
                    if (!found) continue;
                    const float t = std::clamp(o.distance / s.drainReach, 0.f, 1.f);
                    held[l] = t * t * (3 - 2 * t);
                    break;
                }
            }
        }
    return m;
}

// The floor under a world point: the highest floor at or below it (plus `above`, so a point a few
// units into a slope still finds that slope), blended across the four nearest texels that have one.
// Only texels near the closest one's height blend: a wall or ledge between two floors is a real jump,
// and averaging across it made a floor halfway up the wall, with fog floating on it.
// Mirrors GroundFogFloor in EnvironmentClimateShaders.h.
inline constexpr float kGroundFogFloorBlend = 40;
inline float GroundFogFloorBelow(const GroundFogFloorMap &m, float x, float y, float z, float above = 8,
                                 float *thickness = nullptr) {
    if (thickness) *thickness = 1;
    if (!m.Valid()) return kGroundFogNoFloor;
    const float fx = (x - m.originX) / m.cell - .5f, fy = (y - m.originY) / m.cell - .5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    float heights[4], weights[4], held[4], best = -1, reference = kGroundFogNoFloor;
    for (int k = 0; k < 4; ++k) {
        const int cx = x0 + (k & 1), cy = y0 + (k >> 1);
        heights[k] = kGroundFogNoFloor;
        held[k] = 1;
        weights[k] = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty) + 1e-4f;
        if (cx < 0 || cy < 0 || cx >= m.width || cy >= m.height) continue;
        heights[k] = GroundFogLayerBelow(m.At(cx, cy), z + above);
        if (m.thickness.size() == m.layers.size())
            for (int l = 0; l < kGroundFogFloorLayers; ++l)
                if (m.At(cx, cy)[l] == heights[k]) { held[k] = m.thickness[size_t(cy) * m.width + cx][l]; break; }
        if (heights[k] > kGroundFogNoFloor * .5f && weights[k] > best) {
            best = weights[k];
            reference = heights[k];
        }
    }
    if (best < 0) return kGroundFogNoFloor;
    float sum = 0, weight = 0, heldSum = 0;
    for (int k = 0; k < 4; ++k)
        if (heights[k] > kGroundFogNoFloor * .5f && std::abs(heights[k] - reference) < kGroundFogFloorBlend) {
            sum += heights[k] * weights[k];
            heldSum += held[k] * weights[k];
            weight += weights[k];
        }
    if (thickness) *thickness = heldSum / weight;
    return sum / weight;
}

// How dense the layer is at height `h` over its floor, for a local fog top `top`. Softness 0 is full
// through most of the layer with a defined top that rolls off over the last stretch; 1 thins all the way
// up from the floor and reaches higher, so there is no surface at all. Mirrors GroundFogProfile in
// EnvironmentClimateShaders.h.
inline float GroundFogProfile(float h, float top, float soft = 0) {
    if (!(top > 0)) return 0;
    if (h <= 0) return 1;
    const float reach = top * (1 + 1.2f * soft);
    if (h >= reach) return 0;
    const float start = .55f * (1 - soft);
    const float t = std::clamp((h - start * reach) / ((1 - start) * reach), 0.f, 1.f);
    return std::pow(1 - t * t * (3 - 2 * t), 1 + 1.5f * soft);
}
// The fluid's fog amount decides both how dense the fog is and how tall it stands, so the top of the
// layer is a surface the flow shapes: swirls show as relief, a cleared trail is a trench down to the
// floor, and the fog shoved aside stands taller. Mirror these in EnvironmentClimateShaders.h.
inline float GroundFogDensityShare(float fluid) {
    const float a = std::clamp(fluid, 0.f, 1.f);
    return a * a * (3 - 2 * a) * (1 + .4f * (std::max)(fluid - 1, 0.f));
}
// The top follows a smoothed copy of the field and stops at 1.25: following the fine marbling directly
// made the top spike wherever fog was pushed or heaped.
inline float GroundFogTopShare(float smoothedFluid) { return std::pow(std::clamp(smoothedFluid, 0.f, 1.25f), .8f); }
// What clears the fog besides bodies: explosions, fires and smokes.
struct GroundFogBlast {
    Vec3 position;
    float radius = 0, push = 0; // push: outward speed it gives the air, u/s
};
// A blast is a shock ring: it grows fast and its push is gone in a third of a second, leaving a hole.
inline GroundFogBlast GroundFogExplosion(Vec3 position, float age) {
    const float t = std::clamp(age / .35f, 0.f, 1.f);
    return {position, 140 + 240 * t, 1400 * (1 - t)};
}
inline constexpr float kGroundFogFireRadius = 150, kGroundFogFirePush = 40, kGroundFogSmokeShare = .8f;

// WHERE AN EFFECT IS. Particles write no depth, so fog measured to the surface behind one is too much
// when the effect is close, and an effect drawn over the fog regardless pastes a far fire on top in
// pieces. The places effects come from are known (explosions, fires, muzzles, smokes), which says how
// much of the ray to the surface lies in front of the effect. FogEffectShare in
// EnvironmentClimateShaders.h is the same calculation.
struct FogEffectAnchor {
    Vec3 position;
    float radius = 0;
};
inline constexpr int kFogEffectAnchors = 16;
// The share of the ray in front of a bright effect nobody placed (a tracer's far end, a spark). Most
// of them sit at or just before the surface, so they sink into thick fog and show through thin fog.
inline constexpr float kFogEffectUnknownShare = .7f;
// Returns the share (0..1) of the ray to `maxT` in front of the effect on this ray. `known` says how
// sure that is: 1 well inside an anchor, fading to 0 at its edge, so anchors have no visible boundary.
inline float FogEffectShare(Vec3 camera, Vec3 dir, float maxT, const FogEffectAnchor *anchors, int count, float &known) {
    known = 0;
    float sum = 0, weight = 0;
    for (int i = 0; i < count; ++i) {
        const float r = anchors[i].radius;
        if (r <= 0) continue;
        const float ox = anchors[i].position.x - camera.x, oy = anchors[i].position.y - camera.y,
                    oz = anchors[i].position.z - camera.z;
        const float t = ox * dir.x + oy * dir.y + oz * dir.z;
        if (t <= 0) continue;
        const float mx = ox - dir.x * t, my = oy - dir.y * t, mz = oz - dir.z * t;
        const float edge = std::clamp((std::sqrt(mx * mx + my * my + mz * mz) - r * .6f) / (r * .4f), 0.f, 1.f);
        const float w = 1 - edge * edge * (3 - 2 * edge);
        if (w <= 0) continue;
        known = (std::max)(known, w);
        sum += w * (std::max)(t - r * .3f, 0.f);
        weight += w;
    }
    return weight > 0 ? std::clamp(sum / weight / (std::max)(maxT, 1.f), 0.f, 1.f) : 1.f;
}
// A frame keeps the anchors closest to the camera: those are the ones whose fog matters most.
inline void KeepNearestFogEffects(std::vector<FogEffectAnchor> &anchors, Vec3 camera) {
    if (int(anchors.size()) <= kFogEffectAnchors) return;
    auto far2 = [&](const FogEffectAnchor &a) {
        const float x = a.position.x - camera.x, y = a.position.y - camera.y, z = a.position.z - camera.z;
        return x * x + y * y + z * z;
    };
    std::sort(anchors.begin(), anchors.end(), [&](const auto &a, const auto &b) { return far2(a) < far2(b); });
    anchors.resize(kFogEffectAnchors);
}

// THE FLUID GRID covers the whole map and stays put. A grid that followed the camera threw fog away
// whenever the camera turned or cut to another player, and made a render depend on its camera path:
// here the fog stays where players pushed it, whatever the camera does.
struct GroundFogGrid {
    float originX = 0, originY = 0, cell = kGroundFogMinCell;
    int width = 0, height = 0;
    bool Valid() const { return width > 0 && height > 0 && cell > 0; }
    bool operator==(const GroundFogGrid &o) const {
        return originX == o.originX && originY == o.originY && cell == o.cell && width == o.width && height == o.height;
    }
};
inline GroundFogGrid GroundFogGridForMap(Vec3 low, Vec3 high) {
    GroundFogGrid g;
    if (!Finite(low) || !Finite(high) || !(high.x > low.x) || !(high.y > low.y)) return g;
    const float extent = (std::max)(high.x - low.x, high.y - low.y);
    g.cell = (std::max)(kGroundFogMinCell, extent / float(kGroundFogMaxCells - 2));
    g.originX = low.x - g.cell;
    g.originY = low.y - g.cell;
    g.width = (std::min)(kGroundFogMaxCells, int(std::ceil((high.x - low.x) / g.cell)) + 2);
    g.height = (std::min)(kGroundFogMaxCells, int(std::ceil((high.y - low.y) / g.cell)) + 2);
    return g;
}
// Before a map's collision data is ready, and on a map without any: 768 cells around where the camera
// first was (6144 units). Replaced by the map's own grid as soon as there is one.
inline GroundFogGrid GroundFogGridAround(Vec3 camera) {
    GroundFogGrid g;
    if (!Finite(camera)) return g;
    g.cell = kGroundFogMinCell;
    g.width = g.height = 768;
    const float half = float(g.width) * g.cell * .5f;
    g.originX = std::round((camera.x - half) / g.cell) * g.cell;
    g.originY = std::round((camera.y - half) / g.cell) * g.cell;
    return g;
}

// THE CLOCK. Demo time, not wall time: a paused demo freezes the fog, slow motion slows it, and an
// export at a fixed frame rate steps it the same way every time. A seek (more than half a second either
// way) starts the fog again from rest instead of fast-forwarding a fluid through the gap.
//
// The demo clock also steps back by a few milliseconds during ordinary playback (measured 2026-11-08:
// 9 steps in 1200 frames, up to 22 ms). That is not a seek. Treating it as one wiped the whole field
// every second or two, which read as fog that never reacted and kept snapping back to its default. Such
// a step simulates nothing and keeps the latest time, so playback carries on when the clock catches up.
struct GroundFogClock {
    double last = 0;
    bool valid = false;
};
struct GroundFogAdvance {
    float dt = 0;   // seconds to simulate this frame (0: nothing moves)
    int steps = 0;  // fluid steps that time is split into
    bool reset = false;
};
inline constexpr double kGroundFogSeekGap = .5;
inline GroundFogAdvance GroundFogTick(GroundFogClock &c, double time) {
    GroundFogAdvance out;
    if (!std::isfinite(time)) return out;
    if (!c.valid) {
        c.valid = true;
        c.last = time;
        out.reset = true;
        return out;
    }
    const double dt = time - c.last;
    if (dt == 0) return out;
    if (dt > kGroundFogSeekGap || dt < -kGroundFogSeekGap) {
        c.last = time;
        out.reset = true;
        return out;
    }
    if (dt < 0) return out;
    c.last = time;
    out.dt = float(dt);
    // A step of at most 1/45 s keeps a running player's push within a few cells per step.
    out.steps = std::clamp(int(std::ceil(dt * 45.0)), 1, 4);
    return out;
}

// THE DRIFT. How far the wind has carried the rest pattern, the billows and the wisps, in world units.
// It was demo time x the current wind, so a change of Drift speed or direction (both are timeline
// channels) re-multiplied the whole demo time: at 1500 s, 12 -> 45 units/s moved the pattern 49,500
// units. Now the wind is added up as time passes, so a change only alters where the fog goes next.
// A seek (the same gap that restarts the fluid) and the first frame start from time x wind, which is
// what a steady wind adds up to anyway, so a render that seeks to its start begins at the same
// offset every run. The small backward steps of playback hold, as in GroundFogTick. No wrap: the
// noise is not periodic, so a wrap was a visible pop, and a float keeps sub-unit precision past
// a two hour demo at full wind.
struct GroundFogDrift {
    double x = 0, y = 0;
    double last = 0;
    bool valid = false;
};
inline void GroundFogDriftTick(GroundFogDrift &d, double time, double windX, double windY) {
    if (!std::isfinite(time) || !std::isfinite(windX) || !std::isfinite(windY)) return;
    const double dt = d.valid ? time - d.last : 0;
    if (!d.valid || dt > kGroundFogSeekGap || dt < -kGroundFogSeekGap) {
        d.valid = true;
        d.last = time;
        d.x = time * windX;
        d.y = time * windY;
        return;
    }
    if (dt <= 0) return;
    d.last = time;
    d.x += dt * windX;
    d.y += dt * windY;
}

// THE BODIES. A player pushes fog with the velocity it moves at, measured from its position on the
// previous simulated frame (same entity). A first sighting, a respawn or a jump of more than a few
// metres in one step pushes nothing that step.
struct GroundFogBody {
    Vec3 position;
    float vx = 0, vy = 0;
    float radius = 0;
};
struct GroundFogTrackedPawn {
    int key = 0;
    Vec3 position;
    float vx = 0, vy = 0;
};
inline std::vector<GroundFogBody> GroundFogTrackBodies(std::vector<GroundFogTrackedPawn> &tracked,
                                                       const std::vector<std::pair<int, Vec3>> &pawns,
                                                       float dt, float radius) {
    std::vector<GroundFogTrackedPawn> next;
    std::vector<GroundFogBody> bodies;
    for (const auto &pawn : pawns) {
        if (!Finite(pawn.second) || int(bodies.size()) >= kGroundFogBodies) continue;
        GroundFogTrackedPawn now{pawn.first, pawn.second, 0, 0};
        for (const auto &before : tracked) {
            if (before.key != pawn.first || !(dt > 0)) continue;
            const float dx = pawn.second.x - before.position.x, dy = pawn.second.y - before.position.y;
            // 400 u/s is a sprinting player; a few times that in one step is a teleport.
            if (dx * dx + dy * dy > (1500 * dt + 16) * (1500 * dt + 16)) break;
            // Light smoothing: a demo's positions move in tick steps, and a raw difference pulses.
            now.vx = before.vx + (dx / dt - before.vx) * .6f;
            now.vy = before.vy + (dy / dt - before.vy) * .6f;
            break;
        }
        next.push_back(now);
        bodies.push_back({now.position, now.vx, now.vy, radius});
    }
    tracked = std::move(next);
    return bodies;
}
} // namespace Filmmaker::Environment
