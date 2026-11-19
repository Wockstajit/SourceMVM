#pragma once
// WEATHER COVER: how much of the open air reaches a place under a roof.
//
// The top-down sky map (the shelter map) answers "is the sky straight above this point" exactly,
// and that is all open ground needs. What it cannot say is how far snow and wet reach in under
// cover: past a doorway, under an arch, into a garage. Averaging the sky map around a point was the
// old answer, and it counted open sky on the far side of a wall, so rooms beside a street got
// patches of snow.
//
// This is a grid of voxels over the playable map, worked out once from the map's render triangles:
//   1. A voxel is OPEN when nothing is above it (rays straight down each column).
//   2. Two neighbouring voxels are joined when no triangle lies between their centres.
//   3. Open air spreads from the open voxels through the joins and loses a little at every step.
//      Walls do not pass it, a wide opening lets more through than a narrow one, and a crack
//      between two meshes lets through next to nothing.
// The lit passes read it with one trilinear lookup and take the larger of it and the sky map.
//
// A second value is kept beside it, TRAFFIC: how near a place is to the way indoors. It spreads the
// other way, out of the rooms that open air does not reach, so it is high in a doorway and for a
// few steps either side of it and nothing in the middle of a street or deep in a building. It is
// where people walk in and out: snow is trodden there and water is carried in.
#include "SurfaceRayGridMath.h"
#include <cstdint>
#include <vector>

namespace Filmmaker::Environment {
inline constexpr float kWeatherCoverCell = 16.f;
inline constexpr unsigned kWeatherCoverMaxWide = 320, kWeatherCoverMaxTall = 96;
// How many voxel steps under cover the spread is followed. Past it the value is under 1 %.
inline constexpr unsigned kWeatherCoverReach = 24;
inline constexpr unsigned kWeatherCoverSweeps = 40;
// What one 16-unit step under cover keeps, across a wide opening. Half is left 48 units in.
inline constexpr float kWeatherCoverStepKeep = .79f;
// Traffic falls off faster: it is a few steps either side of a doorway.
inline constexpr unsigned kWeatherTrafficReach = 14, kWeatherTrafficSweeps = 28;
inline constexpr float kWeatherTrafficStepKeep = .78f;
// A covered place holding less open air than this (about three steps in from a wide doorway) is
// indoors, and a source of traffic.
inline constexpr float kWeatherIndoors = .3f;

struct WeatherCoverLayout {
    Vec3 low{}, high{};
    unsigned nx = 0, ny = 0, nz = 0;
    size_t Cells() const { return size_t(nx) * ny * nz; }
    Vec3 Cell() const { return {(high.x - low.x) / nx, (high.y - low.y) / ny, (high.z - low.z) / nz}; }
    size_t Index(unsigned x, unsigned y, unsigned z) const { return (size_t(z) * ny + y) * nx + x; }
    Vec3 Centre(unsigned x, unsigned y, unsigned z) const {
        const Vec3 c = Cell();
        return {low.x + (x + .5f) * c.x, low.y + (y + .5f) * c.y, low.z + (z + .5f) * c.z};
    }
    bool operator==(const WeatherCoverLayout &o) const {
        return nx == o.nx && ny == o.ny && nz == o.nz && low.x == o.low.x && low.y == o.low.y && low.z == o.low.z &&
               high.x == o.high.x && high.y == o.high.y && high.z == o.high.z;
    }
};
// Voxels about 16 units across. A map too large for the caps gets wider voxels, not more of them.
inline WeatherCoverLayout MakeWeatherCoverLayout(Vec3 low, Vec3 high) {
    WeatherCoverLayout l;
    l.low = low;
    l.high = high;
    auto count = [](float extent, unsigned cap) {
        return unsigned(std::clamp(std::ceil((std::max)(extent, 1.f) / kWeatherCoverCell), 1.f, float(cap)));
    };
    l.nx = count(high.x - low.x, kWeatherCoverMaxWide);
    l.ny = count(high.y - low.y, kWeatherCoverMaxWide);
    l.nz = count(high.z - low.z, kWeatherCoverMaxTall);
    return l;
}
// The share a voxel loses per sweep so that a step of `cell` units keeps kWeatherCoverStepKeep per
// 16 units. From the plane-wave solution of e = (1 - loss) * mean of six neighbours, four of them
// level with the voxel: r = (1 - loss)(r^2 + 4r + 1) / 6.
inline float WeatherCoverLoss(float cell, float stepKeep = kWeatherCoverStepKeep) {
    const float r = std::pow(stepKeep, (std::max)(cell, 1.f) / 16.f);
    return 1 - 6 * r / (r * r + 4 * r + 1);
}
struct WeatherCoverMesh {
    const std::vector<Vec3> *positions = nullptr;
    const std::vector<uint32_t> *indices = nullptr;
    const SurfaceRayGrid *grid = nullptr;
    bool Ray(Vec3 origin, Vec3 direction, float range, SurfaceGridRayHit *hit = nullptr) const {
        return SurfaceGridRay(*positions, *indices, *grid, origin, direction, range, hit);
    }
};
inline constexpr uint8_t kCoverJoinX = 1, kCoverJoinY = 2, kCoverJoinZ = 4;
struct WeatherCoverBake {
    WeatherCoverLayout layout;
    std::vector<uint8_t> open;  // 1 = nothing above this voxel
    std::vector<uint8_t> joins; // kCoverJoin bits toward +x, +y, +z
    std::vector<float> exposure, traffic;
    void Resize(const WeatherCoverLayout &l) {
        layout = l;
        open.assign(l.Cells(), 0);
        joins.assign(l.Cells(), 0);
        exposure.assign(l.Cells(), 0.f);
        traffic.assign(l.Cells(), 0.f);
    }
};
// Columns [first,last) of the nx*ny columns. Five rays come straight down each column, its centre
// and four points a quarter cell out, and the highest thing any of them meets is the column's roof:
// a single ray slipping through a seam between two roof meshes must not open a column indoors.
inline void WeatherCoverColumns(const WeatherCoverMesh &mesh, WeatherCoverBake &bake, size_t first, size_t last) {
    const auto &l = bake.layout;
    const Vec3 cell = l.Cell();
    const float top = (std::max)(l.high.z, mesh.grid->high.z) + 1, range = top - l.low.z + 1;
    static const float offsets[5][2] = {{0, 0}, {-.25f, -.25f}, {.25f, -.25f}, {-.25f, .25f}, {.25f, .25f}};
    for (size_t column = first; column < last; ++column) {
        const unsigned x = unsigned(column % l.nx), y = unsigned(column / l.nx);
        const Vec3 centre = l.Centre(x, y, 0);
        float roof = -1e30f;
        for (const auto &o : offsets) {
            SurfaceGridRayHit hit;
            if (mesh.Ray({centre.x + o[0] * cell.x, centre.y + o[1] * cell.y, top}, {0, 0, -1}, range, &hit))
                roof = (std::max)(roof, top - hit.distance);
        }
        for (unsigned z = 0; z < l.nz; ++z)
            bake.open[l.Index(x, y, z)] = l.low.z + (z + .5f) * cell.z > roof ? 1 : 0;
    }
}
// Voxels [first,last): which of the three neighbours on the positive side each is joined to. Two
// open voxels are joined without asking; they are both the source and nothing spreads between them.
// A JOIN NEEDS A REAL OPENING. Five parallel rays cross the face between the two voxels (through its
// centre and 0.15 of a voxel out each way across it, close enough to stay on the centre's side of a roof): the centre one and most of them must get
// through. One ray
// through the centre joined two voxels whenever it found a crack: a wall that stops a couple of units
// short of the ceiling, or a seam between two meshes, opened a sealed room to the street, and snow
// lay across the floor of a closed interior (the same cracks the sun shone through). A doorway or a
// window is clear across most of the face and joins as before.
inline void WeatherCoverJoins(const WeatherCoverMesh &mesh, WeatherCoverBake &bake, size_t first, size_t last) {
    const auto &l = bake.layout;
    const Vec3 cell = l.Cell();
    const size_t stride[3] = {1, l.nx, size_t(l.nx) * l.ny};
    const unsigned dims[3] = {l.nx, l.ny, l.nz};
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float lengths[3] = {cell.x, cell.y, cell.z};
    static const float offsets[5][2] = {{0, 0}, {-.15f, -.15f}, {.15f, -.15f}, {-.15f, .15f}, {.15f, .15f}};
    for (size_t i = first; i < last; ++i) {
        const unsigned at[3] = {unsigned(i % l.nx), unsigned(i / l.nx % l.ny), unsigned(i / stride[2])};
        const Vec3 centre = l.Centre(at[0], at[1], at[2]);
        uint8_t bits = 0;
        for (int a = 0; a < 3; ++a) {
            if (at[a] + 1 >= dims[a]) continue;
            if (bake.open[i] && bake.open[i + stride[a]]) {
                bits |= uint8_t(1 << a);
                continue;
            }
            const int u = (a + 1) % 3, v = (a + 2) % 3;
            // The centre must be clear (as one ray always required), and so must most of the five: a
            // voxel cut by a roof or a wall top has offset rays above it that are clear while the
            // centre, which is what the voxel stands for, is not.
            int clear = 0;
            for (int k = 0; k < 5 && clear < 3 && clear + (5 - k) >= 3 && (k == 0 || clear > 0); ++k) {
                Vec3 origin = centre;
                (&origin.x)[u] += offsets[k][0] * lengths[u];
                (&origin.x)[v] += offsets[k][1] * lengths[v];
                if (!mesh.Ray(origin, axes[a], lengths[a])) ++clear;
            }
            if (clear >= 3) bits |= uint8_t(1 << a);
        }
        bake.joins[i] = bits;
    }
}
// The joined neighbours of voxel i, as indices. Returns how many.
inline int WeatherCoverNeighbours(const WeatherCoverBake &bake, size_t i, size_t out[6]) {
    const auto &l = bake.layout;
    const size_t stride[3] = {1, l.nx, size_t(l.nx) * l.ny};
    const unsigned at[3] = {unsigned(i % l.nx), unsigned(i / l.nx % l.ny), unsigned(i / stride[2])};
    int n = 0;
    for (int a = 0; a < 3; ++a) {
        if (bake.joins[i] & (1 << a)) out[n++] = i + stride[a];
        if (at[a] > 0 && (bake.joins[i - stride[a]] & (1 << a))) out[n++] = i - stride[a];
    }
    return n;
}
// Spreads open air under cover. `parallel(count, body)` runs body(first,last) over [0,count), on as
// many threads as the caller likes: voxels are swept in two colours (a checkerboard), and a voxel's
// neighbours are all the other colour, so one colour can be swept from any number of threads.
template <class Parallel> inline void WeatherCoverSolve(WeatherCoverBake &bake, Parallel parallel) {
    const auto &l = bake.layout;
    const size_t cells = l.Cells();
    // The covered voxels within reach of open air, by breadth-first steps through the joins.
    std::vector<uint8_t> steps(cells, 255);
    std::vector<uint32_t> front, next, active[2];
    for (size_t i = 0; i < cells; ++i) {
        bake.exposure[i] = bake.open[i] ? 1.f : 0.f;
        if (!bake.open[i]) continue;
        steps[i] = 0;
        size_t around[6];
        const int n = WeatherCoverNeighbours(bake, i, around);
        for (int k = 0; k < n; ++k)
            if (!bake.open[around[k]]) {
                front.push_back(uint32_t(i));
                break;
            }
    }
    for (unsigned step = 1; step <= kWeatherCoverReach && !front.empty(); ++step) {
        next.clear();
        for (const uint32_t i : front) {
            size_t around[6];
            const int n = WeatherCoverNeighbours(bake, i, around);
            for (int k = 0; k < n; ++k) {
                const size_t j = around[k];
                if (steps[j] != 255) continue;
                steps[j] = uint8_t(step);
                next.push_back(uint32_t(j));
                const unsigned x = unsigned(j % l.nx), y = unsigned(j / l.nx % l.ny), z = unsigned(j / (size_t(l.nx) * l.ny));
                active[(x + y + z) & 1].push_back(uint32_t(j));
            }
        }
        front.swap(next);
    }
    const Vec3 cell = l.Cell();
    const float keep = 1 - WeatherCoverLoss((cell.x + cell.y + cell.z) / 3);
    for (unsigned sweep = 0; sweep < kWeatherCoverSweeps; ++sweep)
        for (const auto &colour : active)
            parallel(colour.size(), [&](size_t first, size_t last) {
                for (size_t k = first; k < last; ++k) {
                    const size_t i = colour[k];
                    size_t around[6];
                    const int n = WeatherCoverNeighbours(bake, i, around);
                    float sum = 0;
                    for (int q = 0; q < n; ++q) sum += bake.exposure[around[q]];
                    bake.exposure[i] = n ? keep * sum / n : 0.f;
                }
            });
    // FLOORS. A lookup half a voxel above a floor blends in the voxel under it, which is inside the
    // ground and holds nothing. That voxel takes the value of the air above it, so a floor reads
    // what the air over it holds. Downward only: sideways it would carry a street into the room
    // behind a thin wall.
    const size_t layer = size_t(l.nx) * l.ny;
    for (size_t i = 0; i + layer < cells; ++i)
        if (steps[i] == 255 && steps[i + layer] != 255) bake.exposure[i] = bake.exposure[i + layer];
}
// TRAFFIC, after WeatherCoverSolve. Its sources are the rooms: covered voxels that open air does not
// reach but that are joined to it somewhere (a voxel inside a wall or under the ground is joined to
// nothing outside and is no room). From them it spreads through the joins, into the open as well,
// and loses more per step than open air does.
template <class Parallel> inline void WeatherCoverTraffic(WeatherCoverBake &bake, Parallel parallel) {
    const auto &l = bake.layout;
    const size_t cells = l.Cells();
    // Everything joined to open air, however far in.
    std::vector<uint8_t> air(cells, 0);
    std::vector<uint32_t> front, next;
    for (size_t i = 0; i < cells; ++i)
        if (bake.open[i]) {
            air[i] = 1;
            front.push_back(uint32_t(i));
        }
    while (!front.empty()) {
        next.clear();
        for (const uint32_t i : front) {
            size_t around[6];
            const int n = WeatherCoverNeighbours(bake, i, around);
            for (int k = 0; k < n; ++k)
                if (!air[around[k]]) {
                    air[around[k]] = 1;
                    next.push_back(uint32_t(around[k]));
                }
        }
        front.swap(next);
    }
    std::vector<uint8_t> steps(cells, 255);
    std::vector<uint32_t> active[2];
    front.clear();
    for (size_t i = 0; i < cells; ++i) {
        const bool room = air[i] && !bake.open[i] && bake.exposure[i] < kWeatherIndoors;
        bake.traffic[i] = room ? 1.f : 0.f;
        if (room) {
            steps[i] = 0;
            front.push_back(uint32_t(i));
        }
    }
    for (unsigned step = 1; step <= kWeatherTrafficReach && !front.empty(); ++step) {
        next.clear();
        for (const uint32_t i : front) {
            size_t around[6];
            const int n = WeatherCoverNeighbours(bake, i, around);
            for (int k = 0; k < n; ++k) {
                const size_t j = around[k];
                if (steps[j] != 255) continue;
                steps[j] = uint8_t(step);
                next.push_back(uint32_t(j));
                const unsigned x = unsigned(j % l.nx), y = unsigned(j / l.nx % l.ny), z = unsigned(j / (size_t(l.nx) * l.ny));
                active[(x + y + z) & 1].push_back(uint32_t(j));
            }
        }
        front.swap(next);
    }
    const Vec3 cell = l.Cell();
    const float keep = 1 - WeatherCoverLoss((cell.x + cell.y + cell.z) / 3, kWeatherTrafficStepKeep);
    for (unsigned sweep = 0; sweep < kWeatherTrafficSweeps; ++sweep)
        for (const auto &colour : active)
            parallel(colour.size(), [&](size_t first, size_t last) {
                for (size_t k = first; k < last; ++k) {
                    const size_t i = colour[k];
                    size_t around[6];
                    const int n = WeatherCoverNeighbours(bake, i, around);
                    float sum = 0;
                    for (int q = 0; q < n; ++q) sum += bake.traffic[around[q]];
                    bake.traffic[i] = n ? keep * sum / n : 0.f;
                }
            });
    // Floors, as for open air: the voxel in the ground under a floor takes the value above it.
    const size_t layer = size_t(l.nx) * l.ny;
    for (size_t i = 0; i + layer < cells; ++i)
        if (!air[i]) bake.traffic[i] = bake.traffic[i + layer];
}
// STORED AS THE EIGHTH ROOT. Between a street (1) and the room behind a thin wall (0) a trilinear
// lookup has nothing to go on and blends the two. Blending eighth roots and raising the result to
// the eighth power keeps that blend under 20 % until the lookup is less than a fifth of a voxel
// from the street's own voxel, so the wall leaks about 3 units and not 13. Under cover the value
// falls by a fixed share per step, which an eighth root keeps linear.
inline uint8_t WeatherCoverEncode(float exposure) {
    if (exposure <= 0.f) return 0;
    if (exposure >= 1.f) return 255;
    return uint8_t(std::lround(255 * std::pow(exposure, .125f)));
}
inline float WeatherCoverDecode(float stored) {
    const float a = stored * stored, b = a * a;
    return b * b;
}
// Two bytes a voxel: open air, then traffic.
inline std::vector<uint8_t> WeatherCoverTexels(const WeatherCoverBake &bake) {
    std::vector<uint8_t> texels(bake.exposure.size() * 2);
    for (size_t i = 0; i < bake.exposure.size(); ++i) {
        texels[i * 2] = WeatherCoverEncode(bake.exposure[i]);
        texels[i * 2 + 1] = WeatherCoverEncode(bake.traffic[i]);
    }
    return texels;
}
// What the shader reads at a world point: the trilinear blend of the stored roots, decoded.
// Channel 0 is open air, 1 is traffic.
inline float WeatherCoverSample(const WeatherCoverLayout &l, const std::vector<uint8_t> &texels, Vec3 p, unsigned channel = 0) {
    const Vec3 cell = l.Cell();
    const float g[3] = {(p.x - l.low.x) / cell.x - .5f, (p.y - l.low.y) / cell.y - .5f, (p.z - l.low.z) / cell.z - .5f};
    const unsigned dims[3] = {l.nx, l.ny, l.nz};
    int base[3];
    float f[3];
    for (int a = 0; a < 3; ++a) {
        const float c = std::clamp(g[a], 0.f, float(dims[a] - 1));
        base[a] = (std::min)(int(c), int(dims[a]) - 1);
        f[a] = c - base[a];
    }
    float sum = 0;
    for (int k = 0; k < 8; ++k) {
        const unsigned x = (std::min)(unsigned(base[0] + (k & 1)), l.nx - 1), y = (std::min)(unsigned(base[1] + (k >> 1 & 1)), l.ny - 1),
                       z = (std::min)(unsigned(base[2] + (k >> 2)), l.nz - 1);
        sum += texels[l.Index(x, y, z) * 2 + channel] / 255.f * (k & 1 ? f[0] : 1 - f[0]) * (k & 2 ? f[1] : 1 - f[1]) * (k & 4 ? f[2] : 1 - f[2]);
    }
    return WeatherCoverDecode(sum);
}
} // namespace Filmmaker::Environment
