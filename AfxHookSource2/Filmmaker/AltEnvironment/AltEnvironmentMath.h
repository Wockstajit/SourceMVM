#pragma once

// ALTERNATIVE ENVIRONMENT: a demo plays on a different map. The players, weapons, grenades and
// cameras stay in the demo's own coordinates; the replacement world is what moves. This header is
// the engine-free part: the alignment between the two maps, the spawn-group world offset that
// realises it, the top-down floor rasters both maps are drawn from, and AUTO ALIGN.
// Docs/features/alternative-environment.md owns the contract;
// Automation/tests/alt-environment-math-tests.cpp covers it.
//
// Coordinates are Source's: x forward, y left, z up. Yaw is in degrees and turns +x towards +y
// (counter-clockwise seen from above), the same sense as a QAngle yaw.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

namespace Filmmaker::AltEnvironment {

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// Where the demo lands on the replacement map: a demo point p is drawn at R(yaw) * p + (x, y, z)
// in the replacement map's own coordinates. Identity means the two maps share coordinates (a night
// or rain remake of the same map).
struct Alignment {
    double x = 0, y = 0, z = 0, yaw = 0;
};

// (-180, 180].
inline double NormalizeYaw(double degrees) {
    if (!std::isfinite(degrees)) return 0;
    double d = std::fmod(degrees, 360.0);
    if (d <= -180) d += 360;
    if (d > 180) d -= 360;
    return d;
}

inline Vec3 RotateYaw(Vec3 p, double degrees) {
    const double r = degrees * kPi / 180.0, c = std::cos(r), s = std::sin(r);
    return {c * p.x - s * p.y, s * p.x + c * p.y, p.z};
}

// Demo coordinates -> replacement map coordinates.
inline Vec3 DemoToMap(const Alignment &a, Vec3 p) {
    const Vec3 r = RotateYaw(p, a.yaw);
    return {r.x + a.x, r.y + a.y, r.z + a.z};
}

// Replacement map coordinates -> demo coordinates.
inline Vec3 MapToDemo(const Alignment &a, Vec3 q) {
    return RotateYaw({q.x - a.x, q.y - a.y, q.z - a.z}, -a.yaw);
}

inline bool IsIdentity(const Alignment &a, double units = 0.01, double degrees = 0.001) {
    return std::fabs(a.x) <= units && std::fabs(a.y) <= units && std::fabs(a.z) <= units &&
           std::fabs(NormalizeYaw(a.yaw)) <= degrees;
}

// CNETMsg_SpawnGroup_Load places a world with world_offset_pos and world_offset_angle: a point q
// of the world's own geometry is drawn at R(angle.yaw) * q + pos. The demo's entities stay where
// they are, so the replacement world gets the inverse of the alignment: q is drawn at
// MapToDemo(a, q), which is R(-yaw) * q - R(-yaw) * (x, y, z).
struct WorldOffset {
    Vec3 pos;
    double yaw = 0;
};
inline WorldOffset WorldOffsetFor(const Alignment &a) {
    WorldOffset w;
    w.yaw = NormalizeYaw(-a.yaw);
    const Vec3 t = RotateYaw({a.x, a.y, a.z}, -a.yaw);
    w.pos = {-t.x, -t.y, -t.z};
    return w;
}
inline Vec3 ApplyWorldOffset(const WorldOffset &w, Vec3 q) {
    const Vec3 r = RotateYaw(q, w.yaw);
    return {r.x + w.pos.x, r.y + w.pos.y, r.z + w.pos.z};
}

// Turns the demo by `degrees` about one of its own points (the pivot stays where it is on the
// replacement map), which is how the alignment view's rotate handle and buttons behave.
inline Alignment RotateAbout(const Alignment &a, double pivotX, double pivotY, double degrees) {
    const Vec3 anchored = DemoToMap(a, {pivotX, pivotY, 0});
    Alignment out = a;
    out.yaw = NormalizeYaw(a.yaw + degrees);
    const Vec3 turned = RotateYaw({pivotX, pivotY, 0}, out.yaw);
    out.x = anchored.x - turned.x;
    out.y = anchored.y - turned.y;
    return out;
}

// ---- top-down floor raster ----------------------------------------------------------------------

// The upward-facing collision surfaces in each cell, seen from above: `top` is the highest one (what
// the pictures and the correlation see), `layers` up to kLayers distinct heights, highest first, so a
// street under an arch or a balcony still has its floor. Row 0 is the lowest y; cell (i, j) covers
// [minX + i * cell, minX + (i + 1) * cell) x [minY + j * cell, ...).
struct FloorRaster {
    static constexpr int kLayers = 4;
    static constexpr float kLayerMerge = 16; // surfaces closer than this are one floor
    int width = 0, height = 0;
    double cell = 0, minX = 0, minY = 0;
    std::vector<float> top;    // NaN where no floor
    std::vector<float> layers; // kLayers per cell, NaN padded
    float lowZ = 0, highZ = 0; // range of the floor heights present

    bool Valid() const { return width > 0 && height > 0 && cell > 0 && top.size() == size_t(width) * size_t(height); }
    bool Inside(int i, int j) const { return i >= 0 && j >= 0 && i < width && j < height; }
    float At(int i, int j) const { return Inside(i, j) ? top[size_t(j) * size_t(width) + size_t(i)] : NAN; }
    bool Floor(int i, int j) const { return !std::isnan(At(i, j)); }
    double CenterX(int i) const { return minX + (i + 0.5) * cell; }
    double CenterY(int j) const { return minY + (j + 0.5) * cell; }
    // The cell containing a world position; may be outside the raster.
    int CellX(double x) const { return int(std::floor((x - minX) / cell)); }
    int CellY(double y) const { return int(std::floor((y - minY) / cell)); }
    float Sample(double x, double y) const { return At(CellX(x), CellY(y)); }
    // Whether the cell has a floor between z - below and z + above, on any layer.
    bool FloorNear(int i, int j, double z, double below, double above) const {
        if (!Inside(i, j) || layers.size() != top.size() * kLayers) return false;
        const float *l = layers.data() + (size_t(j) * size_t(width) + size_t(i)) * kLayers;
        for (int k = 0; k < kLayers && !std::isnan(l[k]); ++k)
            if (l[k] >= z - below && l[k] <= z + above) return true;
        return false;
    }
    bool FloorNearAt(double x, double y, double z, double below, double above) const {
        return FloorNear(CellX(x), CellY(y), z, below, above);
    }
    size_t FloorCount() const {
        size_t n = 0;
        for (float v : top) n += std::isnan(v) ? 0 : 1;
        return n;
    }
};

// Adds a floor height to one cell's layers: merged into a layer within kLayerMerge (the higher one
// wins), else inserted in order, the lowest dropping off when the cell is full.
inline void KeepLayer(float *l, float z) {
    for (int k = 0; k < FloorRaster::kLayers && !std::isnan(l[k]); ++k)
        if (std::fabs(l[k] - z) <= FloorRaster::kLayerMerge) {
            if (z <= l[k]) return;
            l[k] = z;
            for (; k > 0 && l[k] > l[k - 1]; --k) std::swap(l[k], l[k - 1]);
            return;
        }
    int at = 0;
    while (at < FloorRaster::kLayers && !std::isnan(l[at]) && l[at] > z) ++at;
    if (at == FloorRaster::kLayers) return;
    for (int k = FloorRaster::kLayers - 1; k > at; --k) l[k] = l[k - 1];
    l[at] = z;
}

// The cell size that keeps a raster of `extent` units within `maxCells` cells a side, never below
// `minCell`, rounded up to a whole unit.
inline double CellFor(double extent, int maxCells, double minCell) {
    if (!(extent > 0) || maxCells < 1) return minCell;
    return (std::max)(minCell, std::ceil(extent / maxCells));
}

// Rasterises triangles (float xyz triples, uint32 indices) whose normal points up by at least
// minNormalZ. Each cell keeps the surfaces over its centre as layers. Cells under a triangle's
// corners and centroid are marked too, so a sliver narrower than a cell still shows.
inline FloorRaster BuildFloorRaster(const float *xyz, size_t vertexCount, const uint32_t *indices, size_t indexCount,
                                    double cell, double minNormalZ = 0.6) {
    FloorRaster r;
    if (!xyz || !indices || vertexCount < 3 || indexCount < 3 || !(cell > 0)) return r;
    double lx = (std::numeric_limits<double>::max)(), ly = lx, hx = -lx, hy = -lx;
    for (size_t t = 0; t + 2 < indexCount; t += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = indices[t + k];
            if (v >= vertexCount) return r;
            lx = (std::min)(lx, double(xyz[v * 3])), hx = (std::max)(hx, double(xyz[v * 3]));
            ly = (std::min)(ly, double(xyz[v * 3 + 1])), hy = (std::max)(hy, double(xyz[v * 3 + 1]));
        }
    if (!(hx >= lx && hy >= ly)) return r;
    r.cell = cell;
    r.minX = std::floor(lx / cell) * cell;
    r.minY = std::floor(ly / cell) * cell;
    r.width = int(std::floor((hx - r.minX) / cell)) + 1;
    r.height = int(std::floor((hy - r.minY) / cell)) + 1;
    if (r.width <= 0 || r.height <= 0 || double(r.width) * r.height > 64.0e6) return FloorRaster{};
    r.top.assign(size_t(r.width) * size_t(r.height), NAN);
    r.layers.assign(r.top.size() * FloorRaster::kLayers, NAN);
    auto keep = [&r](int i, int j, double z) {
        if (!r.Inside(i, j)) return;
        const size_t cell = size_t(j) * size_t(r.width) + size_t(i);
        KeepLayer(r.layers.data() + cell * FloorRaster::kLayers, float(z));
        r.top[cell] = r.layers[cell * FloorRaster::kLayers];
    };
    for (size_t t = 0; t + 2 < indexCount; t += 3) {
        const float *a = xyz + size_t(indices[t]) * 3, *b = xyz + size_t(indices[t + 1]) * 3, *c = xyz + size_t(indices[t + 2]) * 3;
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(len > 0) || nz / len < minNormalZ) continue;
        // Plane z(x, y) through a with normal n.
        auto zAt = [&](double x, double y) { return a[2] - (nx * (x - a[0]) + ny * (y - a[1])) / nz; };
        const double area = ux * vy - uy * vx; // twice the signed xy area
        const int i0 = r.CellX((std::min)({a[0], b[0], c[0]})), i1 = r.CellX((std::max)({a[0], b[0], c[0]}));
        const int j0 = r.CellY((std::min)({a[1], b[1], c[1]})), j1 = r.CellY((std::max)({a[1], b[1], c[1]}));
        for (int j = (std::max)(j0, 0); j <= (std::min)(j1, r.height - 1); ++j)
            for (int i = (std::max)(i0, 0); i <= (std::min)(i1, r.width - 1); ++i) {
                const double px = r.CenterX(i), py = r.CenterY(j);
                const double w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                const double w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
                const double w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
                const bool inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (inside) keep(i, j, zAt(px, py));
            }
        keep(r.CellX(a[0]), r.CellY(a[1]), a[2]);
        keep(r.CellX(b[0]), r.CellY(b[1]), b[2]);
        keep(r.CellX(c[0]), r.CellY(c[1]), c[2]);
        const double gx = (a[0] + b[0] + c[0]) / 3, gy = (a[1] + b[1] + c[1]) / 3;
        keep(r.CellX(gx), r.CellY(gy), zAt(gx, gy));
    }
    bool any = false;
    for (float v : r.top)
        if (!std::isnan(v)) {
            r.lowZ = any ? (std::min)(r.lowZ, v) : v;
            r.highZ = any ? (std::max)(r.highZ, v) : v;
            any = true;
        }
    return any ? r : FloorRaster{};
}

// The same floor at `factor` times the cell size; each coarse cell merges its cells' layers.
inline FloorRaster Downsample(const FloorRaster &in, int factor) {
    if (factor <= 1 || !in.Valid()) return in;
    FloorRaster r;
    r.cell = in.cell * factor;
    r.minX = in.minX;
    r.minY = in.minY;
    r.width = (in.width + factor - 1) / factor;
    r.height = (in.height + factor - 1) / factor;
    r.lowZ = in.lowZ;
    r.highZ = in.highZ;
    r.top.assign(size_t(r.width) * size_t(r.height), NAN);
    r.layers.assign(r.top.size() * FloorRaster::kLayers, NAN);
    const bool layered = in.layers.size() == in.top.size() * FloorRaster::kLayers;
    for (int j = 0; j < in.height; ++j)
        for (int i = 0; i < in.width; ++i) {
            const size_t from = size_t(j) * size_t(in.width) + size_t(i);
            const size_t to = size_t(j / factor) * size_t(r.width) + size_t(i / factor);
            float *l = r.layers.data() + to * FloorRaster::kLayers;
            if (layered) {
                for (int k = 0; k < FloorRaster::kLayers && !std::isnan(in.layers[from * FloorRaster::kLayers + k]); ++k)
                    KeepLayer(l, in.layers[from * FloorRaster::kLayers + k]);
            } else if (!std::isnan(in.top[from])) {
                KeepLayer(l, in.top[from]);
            }
            r.top[to] = l[0];
        }
    return r;
}

// ---- the top-down pictures ----------------------------------------------------------------------

// RGBA8, row 0 at the TOP (highest y), the way an image is stored. Floors are shaded by height
// from dark (lowest) to light (highest); a cell whose neighbour is no floor, or a step taller than
// a stair, is an edge and drawn as `edge`. Cells without floor are transparent.
struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};
inline std::vector<uint8_t> ShadeFloor(const FloorRaster &f, Rgba low, Rgba high, Rgba edge, float stepUnits = 40) {
    std::vector<uint8_t> px(size_t(f.width) * size_t(f.height) * 4, 0);
    const float span = (std::max)(1.f, f.highZ - f.lowZ);
    for (int j = 0; j < f.height; ++j)
        for (int i = 0; i < f.width; ++i) {
            const float v = f.At(i, j);
            if (std::isnan(v)) continue;
            bool isEdge = false;
            for (int d = 0; d < 4 && !isEdge; ++d) {
                const float n = f.At(i + (d == 0) - (d == 1), j + (d == 2) - (d == 3));
                isEdge = std::isnan(n) || std::fabs(n - v) > stepUnits;
            }
            Rgba c = edge;
            if (!isEdge) {
                const float t = std::clamp((v - f.lowZ) / span, 0.f, 1.f);
                c = {uint8_t(low.r + (high.r - low.r) * t + .5f), uint8_t(low.g + (high.g - low.g) * t + .5f),
                     uint8_t(low.b + (high.b - low.b) * t + .5f), uint8_t(low.a + (high.a - low.a) * t + .5f)};
            }
            uint8_t *o = px.data() + (size_t(f.height - 1 - j) * size_t(f.width) + size_t(i)) * 4;
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = c.a;
        }
    return px;
}

// Marks the cells under world points (the players' feet) with `color` in an image ShadeFloor made
// for the same raster, `radius` cells around each. With `clearOnly` it never paints over a pixel
// that already shows something (the demo layer keeps its edges on top of the walked paths).
inline void StampPoints(std::vector<uint8_t> &px, const FloorRaster &f, const std::vector<Vec3> &points, Rgba color, int radius = 0,
                        bool clearOnly = false) {
    if (px.size() != size_t(f.width) * size_t(f.height) * 4) return;
    for (const Vec3 &p : points) {
        const int ci = f.CellX(p.x), cj = f.CellY(p.y);
        for (int dj = -radius; dj <= radius; ++dj)
            for (int di = -radius; di <= radius; ++di) {
                const int i = ci + di, j = cj + dj;
                if (!f.Inside(i, j) || di * di + dj * dj > radius * radius) continue;
                uint8_t *o = px.data() + (size_t(f.height - 1 - j) * size_t(f.width) + size_t(i)) * 4;
                if (clearOnly && o[3] != 0) continue;
                o[0] = color.r, o[1] = color.g, o[2] = color.b, o[3] = color.a;
            }
    }
}

// ---- AUTO ALIGN -----------------------------------------------------------------------------------

// How well an alignment fits. `floor` is the share of the demo map's floor that lands on the
// replacement map's floor at the matching height; `players` the share of the players' feet that
// land on a floor within reach under them. Both are 0..1.
struct Fit {
    double floor = 0, players = 0;
    size_t floorCells = 0, playerSamples = 0;
    double Score() const { return playerSamples ? 0.5 * floor + 0.5 * players : floor; }
};

// The height step that best matches the two floors under an xy alignment (the alignment's own z
// is ignored): the most common difference, 8-unit bins, refined to the mean of its bin. Roofs over
// a floor in one map only add scattered bins, so the floor-to-floor step wins.
inline bool BestHeightStep(const FloorRaster &demo, const FloorRaster &target, const Alignment &a, int stride, double &step) {
    constexpr double kBin = 8;
    std::vector<double> diffs;
    for (int j = 0; j < demo.height; j += stride)
        for (int i = 0; i < demo.width; i += stride) {
            const float v = demo.At(i, j);
            if (std::isnan(v)) continue;
            const Vec3 m = DemoToMap({a.x, a.y, 0, a.yaw}, {demo.CenterX(i), demo.CenterY(j), 0});
            const float t = target.Sample(m.x, m.y);
            if (!std::isnan(t)) diffs.push_back(double(t) - v);
        }
    if (diffs.empty()) return false;
    std::sort(diffs.begin(), diffs.end());
    size_t bestStart = 0, bestCount = 0;
    for (size_t s = 0, e = 0; s < diffs.size(); ++s) {
        while (e < diffs.size() && diffs[e] - diffs[s] < kBin) ++e;
        if (e - s > bestCount) bestCount = e - s, bestStart = s;
    }
    double sum = 0;
    for (size_t k = bestStart; k < bestStart + bestCount; ++k) sum += diffs[k];
    step = sum / double(bestCount);
    return true;
}

// Scores an alignment. The demo's top floor matches when any of the replacement's floor layers in
// that place is at the same height. Feet count as on the floor when the replacement has a floor
// layer between `footBelow` units under them and `footAbove` units over them (a player's origin is
// at their feet; the layers are what lets a street under an arch carry them).
inline Fit Evaluate(const FloorRaster &demo, const FloorRaster &target, const std::vector<Vec3> &feet, const Alignment &a,
                    int stride = 1, double heightTolerance = 24, double footBelow = 48, double footAbove = 16) {
    Fit fit;
    size_t total = 0, hit = 0;
    for (int j = 0; j < demo.height; j += stride)
        for (int i = 0; i < demo.width; i += stride) {
            const float v = demo.At(i, j);
            if (std::isnan(v)) continue;
            ++total;
            const Vec3 m = DemoToMap(a, {demo.CenterX(i), demo.CenterY(j), v});
            if (target.FloorNearAt(m.x, m.y, m.z, heightTolerance, heightTolerance)) ++hit;
        }
    fit.floorCells = total;
    fit.floor = total ? double(hit) / double(total) : 0;
    size_t feetHit = 0;
    for (const Vec3 &p : feet) {
        const Vec3 m = DemoToMap(a, p);
        if (target.FloorNearAt(m.x, m.y, m.z, footBelow, footAbove)) ++feetHit;
    }
    fit.playerSamples = feet.size();
    fit.players = feet.empty() ? 0 : double(feetHit) / double(feet.size());
    return fit;
}

// ---- where the demo lands ---------------------------------------------------------------------

// Where a demo point standing on a floor (a player's feet, a floor cell) lands on the replacement map
// under an alignment, given in map coordinates: On a floor there (48 units under the feet to 16 over,
// the reach the fit counts feet with), Floating over one (the only floor in that place is further
// down), Under it (floor only above: inside or beneath the geometry), or Outside (no floor in that
// place at all -- off the map, in the void).
enum class Landing : uint8_t { On, Floating, Under, Outside };

inline Landing LandingAt(const FloorRaster &target, const Vec3 &m, double below = 48, double above = 16) {
    const int i = target.CellX(m.x), j = target.CellY(m.y);
    if (!target.Inside(i, j) || target.layers.size() != target.top.size() * FloorRaster::kLayers) return Landing::Outside;
    const float *l = target.layers.data() + (size_t(j) * size_t(target.width) + size_t(i)) * FloorRaster::kLayers;
    bool lower = false, upper = false;
    for (int k = 0; k < FloorRaster::kLayers && !std::isnan(l[k]); ++k) {
        if (l[k] >= m.z - below && l[k] <= m.z + above) return Landing::On;
        if (l[k] < m.z - below) lower = true;
        else upper = true;
    }
    return lower ? Landing::Floating : upper ? Landing::Under : Landing::Outside;
}

// How the players' walked feet land, counted by kind.
struct Landings {
    size_t on = 0, floating = 0, under = 0, outside = 0;
    size_t Total() const { return on + floating + under + outside; }
    void Add(Landing l) { (l == Landing::On ? on : l == Landing::Floating ? floating : l == Landing::Under ? under : outside) += 1; }
    double Share(size_t n) const { return Total() ? double(n) / double(Total()) : 0; }
};

inline Landings CountLandings(const FloorRaster &target, const std::vector<Vec3> &feet, const Alignment &a) {
    Landings c;
    for (const Vec3 &p : feet) c.Add(LandingAt(target, DemoToMap(a, p)));
    return c;
}

// The colours the map view draws each kind in.
inline Rgba LandingColor(Landing l, uint8_t alpha) {
    switch (l) {
    case Landing::On: return {74, 222, 128, alpha};
    case Landing::Floating: return {250, 204, 21, alpha};
    case Landing::Under: return {168, 85, 247, alpha};
    default: return {248, 82, 82, alpha};
    }
}

// The map view's demo layer, in the demo raster's own pixels (the view turns and moves it by the
// alignment): the demo map's floor tinted by where it lands, the players' walked feet on top in the
// same colours, the demo map's walls as a light outline. `counts` gets the feet's landings.
inline std::vector<uint8_t> LandingPicture(const FloorRaster &demo, const FloorRaster &target, const std::vector<Vec3> &feet,
                                           const Alignment &a, Landings *counts) {
    auto px = ShadeFloor(demo, {0, 0, 0, 0}, {0, 0, 0, 0}, {236, 240, 244, 220});
    for (int j = 0; j < demo.height; ++j)
        for (int i = 0; i < demo.width; ++i) {
            const float v = demo.At(i, j);
            uint8_t *o = px.data() + (size_t(demo.height - 1 - j) * size_t(demo.width) + size_t(i)) * 4;
            if (std::isnan(v) || o[3] != 0) continue; // no floor, or a wall's outline
            const Rgba c = LandingColor(LandingAt(target, DemoToMap(a, {demo.CenterX(i), demo.CenterY(j), v})), 72);
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = c.a;
        }
    Landings c;
    std::vector<Vec3> kind[4];
    for (const Vec3 &p : feet) {
        const Landing l = LandingAt(target, DemoToMap(a, p));
        c.Add(l);
        kind[int(l)].push_back(p);
    }
    // Off the map first, on it last: where they overlap, the good news is what shows.
    for (Landing l : {Landing::Outside, Landing::Under, Landing::Floating, Landing::On}) StampPoints(px, demo, kind[int(l)], LandingColor(l, 235));
    if (counts) *counts = c;
    return px;
}

namespace Detail {

// Runs fn(0) .. fn(count - 1), each exactly once, on up to `threads` threads (0: the cores less
// two for the game, at most 8). Every worker but the calling thread runs `init` first (the game
// lowers their priority with it). Results go into per-index slots the caller merges in index
// order, so the answer never depends on which thread finished first.
template <class Fn>
void ParallelFor(int count, int threads, const std::function<void()> &init, Fn fn) {
    if (threads <= 0) {
        const int cores = int(std::thread::hardware_concurrency());
        threads = (std::min)(8, cores > 3 ? cores - 2 : 1);
    }
    threads = (std::min)(threads, count);
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<int> next{0};
    auto work = [&](bool worker) {
        if (worker && init) init();
        for (int i = next++; i < count; i = next++) fn(i);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work, true);
    work(false);
    for (std::thread &t : pool) t.join();
}

// Single precision: the correlations sum +-1 cells, which a float carries exactly enough, at half
// the memory and twice the speed of double.
using Complex = std::complex<float>;

// In place, n a power of two. The twiddles come from a table computed in double for this n, so no
// error builds up along a stage.
inline void Fft(Complex *a, size_t n, const std::vector<Complex> &twiddle, bool inverse) {
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex w = inverse ? std::conj(twiddle[k * step]) : twiddle[k * step];
                const Complex u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
    if (inverse) {
        const float scale = 1.0f / float(n);
        for (size_t i = 0; i < n; ++i) a[i] *= scale;
    }
}

inline std::vector<Complex> Twiddles(size_t n) {
    std::vector<Complex> t(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        const double angle = -2 * kPi * double(k) / double(n);
        t[k] = Complex(float(std::cos(angle)), float(std::sin(angle)));
    }
    return t;
}

// Row-major n x n, in place.
inline void Fft2(std::vector<Complex> &a, size_t n, const std::vector<Complex> &twiddle, bool inverse) {
    for (size_t y = 0; y < n; ++y) Fft(a.data() + y * n, n, twiddle, inverse);
    std::vector<Complex> line(n);
    for (size_t x = 0; x < n; ++x) {
        for (size_t y = 0; y < n; ++y) line[y] = a[y * n + x];
        Fft(line.data(), n, twiddle, inverse);
        for (size_t y = 0; y < n; ++y) a[y * n + x] = line[y];
    }
}

inline size_t Pow2AtLeast(size_t v) {
    size_t n = 1;
    while (n < v) n <<= 1;
    return n;
}

// The demo floor turned by `yaw` about the world origin, resampled onto `cell`-sized cells; minX
// and minY are the turned grid's corner. Each turned cell samples the demo cell under its centre.
struct Turned {
    int width = 0, height = 0;
    double minX = 0, minY = 0;
    std::vector<float> weight; // 1 = floor
};
inline Turned Turn(const FloorRaster &demo, double yaw, double cell) {
    Turned t;
    double lx = 1e300, ly = 1e300, hx = -1e300, hy = -1e300;
    for (int k = 0; k < 4; ++k) {
        const Vec3 c = RotateYaw({demo.minX + (k & 1) * demo.width * demo.cell, demo.minY + (k >> 1) * demo.height * demo.cell, 0}, yaw);
        lx = (std::min)(lx, c.x), hx = (std::max)(hx, c.x), ly = (std::min)(ly, c.y), hy = (std::max)(hy, c.y);
    }
    t.minX = std::floor(lx / cell) * cell;
    t.minY = std::floor(ly / cell) * cell;
    t.width = int(std::ceil((hx - t.minX) / cell)) + 1;
    t.height = int(std::ceil((hy - t.minY) / cell)) + 1;
    t.weight.assign(size_t(t.width) * size_t(t.height), 0.f);
    for (int j = 0; j < t.height; ++j)
        for (int i = 0; i < t.width; ++i) {
            const Vec3 p = RotateYaw({t.minX + (i + 0.5) * cell, t.minY + (j + 0.5) * cell, 0}, -yaw);
            if (!std::isnan(demo.Sample(p.x, p.y))) t.weight[size_t(j) * size_t(t.width) + size_t(i)] = 1.f;
        }
    return t;
}

struct Peak {
    double x = 0, y = 0, yaw = 0, score = -1e300;
};

// One cell size's correlation setup: the target floor (+1 floor, -1 no floor inside its bounds,
// so landing on empty space costs as much as landing on floor gains) and its spectrum, sized so
// no shift of a turned demo floor wraps around.
struct Correlator {
    FloorRaster demo, target;
    size_t n = 0;
    std::vector<Complex> spectrum, twiddle;
};
inline Correlator MakeCorrelator(const FloorRaster &demo, const FloorRaster &target, int factor) {
    Correlator c;
    c.demo = Downsample(demo, factor);
    c.target = Downsample(target, factor);
    const double turned = std::hypot(double(c.demo.width), double(c.demo.height)) + 3;
    c.n = Pow2AtLeast(size_t((std::max)(c.target.width, c.target.height) + std::ceil(turned)));
    c.twiddle = Twiddles(c.n);
    c.spectrum.assign(c.n * c.n, Complex(0));
    for (int j = 0; j < c.target.height; ++j)
        for (int i = 0; i < c.target.width; ++i) c.spectrum[size_t(j) * c.n + size_t(i)] = c.target.Floor(i, j) ? 1.0f : -1.0f;
    Fft2(c.spectrum, c.n, c.twiddle, false);
    return c;
}

// The best xy offsets for one yaw: c[s] = sum_p turned[p] * target[p + s]. Appends up to `keep`
// peaks at least two cells apart.
inline void CorrelateYaw(const Correlator &c, double yaw, int keep, std::vector<Peak> &out) {
    const double cell = c.target.cell;
    const Turned t = Turn(c.demo, yaw, cell);
    const size_t n = c.n;
    if (size_t(t.width + c.target.width) > n || size_t(t.height + c.target.height) > n) return;
    std::vector<Complex> a(n * n, Complex(0));
    for (int j = 0; j < t.height; ++j)
        for (int i = 0; i < t.width; ++i) a[size_t(j) * n + size_t(i)] = t.weight[size_t(j) * size_t(t.width) + size_t(i)];
    Fft2(a, n, c.twiddle, false);
    for (size_t k = 0; k < a.size(); ++k) a[k] = std::conj(a[k]) * c.spectrum[k];
    Fft2(a, n, c.twiddle, true);
    // Shifts run from -(turned width - 1) to target width - 1; negative ones sit at the top end.
    std::vector<Peak> local;
    for (size_t sy = 0; sy < n; ++sy)
        for (size_t sx = 0; sx < n; ++sx) {
            const long dx = sx + size_t(t.width) > n ? long(sx) - long(n) : long(sx);
            const long dy = sy + size_t(t.height) > n ? long(sy) - long(n) : long(sy);
            Peak p;
            p.x = c.target.minX - t.minX + dx * cell;
            p.y = c.target.minY - t.minY + dy * cell;
            p.yaw = yaw;
            p.score = a[sy * n + sx].real();
            bool merged = false;
            for (Peak &q : local)
                if (std::fabs(q.x - p.x) <= 2 * cell && std::fabs(q.y - p.y) <= 2 * cell) {
                    if (p.score > q.score) q = p;
                    merged = true;
                    break;
                }
            if (merged) continue;
            if (int(local.size()) < keep) local.push_back(p);
            else {
                auto worst = std::min_element(local.begin(), local.end(), [](const Peak &l, const Peak &r) { return l.score < r.score; });
                if (p.score > worst->score) *worst = p;
            }
        }
    out.insert(out.end(), local.begin(), local.end());
}

// Best first, with peaks that are the same place at neighbouring yaws folded into one.
inline std::vector<Peak> Distinct(std::vector<Peak> peaks, double cells, double degrees, double cell, size_t keep) {
    std::sort(peaks.begin(), peaks.end(), [](const Peak &l, const Peak &r) { return l.score > r.score; });
    std::vector<Peak> out;
    for (const Peak &p : peaks) {
        bool same = false;
        for (const Peak &q : out)
            same = same || (std::hypot(p.x - q.x, p.y - q.y) <= cells * cell && std::fabs(NormalizeYaw(p.yaw - q.yaw)) <= degrees);
        if (!same) out.push_back(p);
        if (out.size() >= keep) break;
    }
    return out;
}

} // namespace Detail

struct AutoAlignOptions {
    double coarseCell = 64;  // first pass: every yaw at this cell size
    double coarseYawStep = 3;
    double fineCell = 32;    // second pass: around the best coarse peaks
    double fineYawStep = 0.5;
    int candidates = 4;
    size_t maxFeet = 4000;   // players' feet samples used (evenly thinned)
    // Accept the maps' own coordinates without searching when they already fit this well.
    double identityAccept = 0.85;
    // A best fit below this is no match: the maps do not share a layout (real remakes score 0.87+).
    double minAccept = 0.4;
    int threads = 0;                // Detail::ParallelFor: 0 = the cores less two, at most 8
    std::function<void()> workerInit; // run first on each worker thread (the game lowers its priority)
};

struct AutoAlignResult {
    bool ok = false;
    bool identity = false; // the maps already shared coordinates
    Alignment alignment;
    Fit fit;
};

// Finds where the demo's floor (and the players' feet, when given) best lands on the replacement
// map. Both rasters must share a cell size no larger than the options' fine cell. Yaw is searched
// all the way round; z is the height step between the two floors. `progress(0..1)` is called as it
// goes, from worker threads too, so it must be safe to call from any thread.
template <class Progress>
AutoAlignResult AutoAlign(const FloorRaster &demo, const FloorRaster &target, const std::vector<Vec3> &allFeet,
                          const AutoAlignOptions &options, Progress progress) {
    AutoAlignResult result;
    if (!demo.Valid() || !target.Valid() || std::fabs(demo.cell - target.cell) > 1e-6 || options.fineCell < demo.cell) return result;
    std::vector<Vec3> feet;
    const size_t every = allFeet.size() > options.maxFeet ? (allFeet.size() + options.maxFeet - 1) / options.maxFeet : 1;
    for (size_t k = 0; k < allFeet.size(); k += every) feet.push_back(allFeet[k]);
    const int stride = (std::max)(1, int(16.0 / demo.cell));

    // The common case first: a remake of the same map in the same place. No offset at all, or only
    // the height step between the two floors.
    Alignment same;
    double sameScore = Evaluate(demo, target, feet, same, stride).Score();
    {
        double step = 0;
        if (BestHeightStep(demo, target, Alignment{}, stride, step) && std::fabs(step) > 0.5) {
            const Alignment lifted{0, 0, step, 0};
            const double s = Evaluate(demo, target, feet, lifted, stride).Score();
            if (s > sameScore + 0.005) same = lifted, sameScore = s;
        }
        if (sameScore >= options.identityAccept) {
            result.ok = result.identity = true;
            result.alignment = same;
            result.fit = Evaluate(demo, target, feet, same);
            return result;
        }
    }

    const int coarseFactor = (std::max)(1, int(std::lround(options.coarseCell / demo.cell)));
    const int fineFactor = (std::max)(1, int(std::lround(options.fineCell / demo.cell)));
    const Detail::Correlator coarse = Detail::MakeCorrelator(demo, target, coarseFactor);
    const Detail::Correlator fine = Detail::MakeCorrelator(demo, target, fineFactor);
    const double coarseCell = coarse.target.cell, fineCell = fine.target.cell;

    // Every pass runs its yaws on several threads (Detail::ParallelFor); `done` counts finished
    // work for the progress bar: 60% coarse, 30% fine, 10% the last step.
    std::atomic<int> done{0};
    const int yawCount = (std::max)(1, int(std::lround(360.0 / options.coarseYawStep)));
    std::vector<std::vector<Detail::Peak>> perYaw(static_cast<size_t>(yawCount));
    Detail::ParallelFor(yawCount, options.threads, options.workerInit, [&](int k) {
        Detail::CorrelateYaw(coarse, NormalizeYaw(-180 + (k + 1) * 360.0 / yawCount), 3, perYaw[size_t(k)]);
        progress(0.6 * double(++done) / double(yawCount));
    });
    std::vector<Detail::Peak> peaks;
    for (const auto &v : perYaw) peaks.insert(peaks.end(), v.begin(), v.end());
    const std::vector<Detail::Peak> candidates =
        Detail::Distinct(peaks, 3, 2 * options.coarseYawStep, coarseCell, size_t((std::max)(1, options.candidates)));
    if (candidates.empty()) return result;

    // Around each candidate: its yaw +- one coarse step in fine steps, at the fine cell size.
    std::vector<double> fineYaws;
    for (double d = -options.coarseYawStep; d <= options.coarseYawStep + 1e-9; d += options.fineYawStep) fineYaws.push_back(d);
    const int fineCount = int(candidates.size() * fineYaws.size());
    std::vector<std::vector<Detail::Peak>> perFine(static_cast<size_t>(fineCount));
    done = 0;
    Detail::ParallelFor(fineCount, options.threads, options.workerInit, [&](int t) {
        const Detail::Peak &c = candidates[size_t(t) / fineYaws.size()];
        Detail::CorrelateYaw(fine, NormalizeYaw(c.yaw + fineYaws[size_t(t) % fineYaws.size()]), 3, perFine[size_t(t)]);
        progress(0.6 + 0.3 * double(++done) / double(fineCount));
    });
    // Each candidate keeps only its own neighbourhood: another region's peak is another candidate.
    std::vector<Detail::Peak> tops;
    for (size_t ci = 0; ci < candidates.size(); ++ci) {
        const Detail::Peak &c = candidates[ci];
        std::vector<Detail::Peak> mine;
        for (size_t yi = 0; yi < fineYaws.size(); ++yi)
            for (const Detail::Peak &p : perFine[ci * fineYaws.size() + yi])
                if (std::hypot(p.x - c.x, p.y - c.y) <= 2 * coarseCell) mine.push_back(p);
        const std::vector<Detail::Peak> top = Detail::Distinct(mine, 1, options.fineYawStep, fineCell, 2);
        tops.insert(tops.end(), top.begin(), top.end());
    }

    // Last step on the full raster: quarter-cell offsets and quarter-degree yaws around each peak.
    // The height step is the floors' at the peak for that yaw; a quarter cell does not change it.
    std::vector<double> lastYaws;
    for (double d = -options.fineYawStep; d <= options.fineYawStep + 1e-9; d += 0.25) lastYaws.push_back(d);
    const int lastCount = int(tops.size() * lastYaws.size());
    std::vector<Alignment> lastBest(static_cast<size_t>(lastCount));
    std::vector<double> lastScore(size_t(lastCount), -1);
    done = 0;
    Detail::ParallelFor(lastCount, options.threads, options.workerInit, [&](int t) {
        const Detail::Peak &p = tops[size_t(t) / lastYaws.size()];
        const double yaw = NormalizeYaw(p.yaw + lastYaws[size_t(t) % lastYaws.size()]);
        double step = 0;
        if (BestHeightStep(demo, target, Alignment{p.x, p.y, 0, yaw}, stride * 2, step))
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const Alignment a{p.x + dx * fineCell / 4, p.y + dy * fineCell / 4, step, yaw};
                    const double s = Evaluate(demo, target, feet, a, stride * 2).Score();
                    if (s > lastScore[size_t(t)]) lastScore[size_t(t)] = s, lastBest[size_t(t)] = a;
                }
        progress(0.9 + 0.1 * double(++done) / double(lastCount));
    });
    Alignment best;
    double bestScore = -1;
    for (int t = 0; t < lastCount; ++t)
        if (lastScore[size_t(t)] > bestScore) bestScore = lastScore[size_t(t)], best = lastBest[size_t(t)];
    if (bestScore < 0) return result;
    // A search that lands next to the maps' own coordinates and does no better than them is them:
    // keep the exact zero rather than a fraction of a unit that would count as a moved world.
    const bool nearSame = std::hypot(best.x - same.x, best.y - same.y) <= 2 * fineCell && std::fabs(NormalizeYaw(best.yaw)) <= 1;
    if (nearSame && Evaluate(demo, target, feet, same, stride * 2).Score() >= bestScore - 0.005) {
        result.ok = result.identity = true;
        result.alignment = same;
        result.fit = Evaluate(demo, target, feet, same);
        return result;
    }
    result.alignment = best;
    result.fit = Evaluate(demo, target, feet, best);
    // Two different maps: even the best place fits almost nothing (Inferno on Anubis Night, 2% of
    // the floor). Say so rather than move the demo there.
    result.ok = result.fit.Score() >= options.minAccept;
    return result;
}

inline AutoAlignResult AutoAlign(const FloorRaster &demo, const FloorRaster &target, const std::vector<Vec3> &feet,
                                 const AutoAlignOptions &options = {}) {
    return AutoAlign(demo, target, feet, options, [](double) {});
}

} // namespace Filmmaker::AltEnvironment
