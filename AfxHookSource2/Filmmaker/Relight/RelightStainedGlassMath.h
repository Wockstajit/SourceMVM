#pragma once
// Stained glass as sunlight beams. The map's stained glass is opaque geometry: it stays a sun
// shadow caster, so no plain sunlight passes it. Each window instead becomes one directional
// record whose projection starts at the glass and runs away from the sun into the room behind.
// It carries a projected cookie: the glass drawn in that same projection, in its own colours,
// masked by whether the sun actually reaches the glass. The native material's own cookie path
// (record radiance.w > 0) then lights the floor and walls with the window's colours.
#include "RelightMath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace Filmmaker::Relight {
// How far behind the glass a beam reaches, and the cookie resolution per window.
inline constexpr float GlassBeamDepth = 2048;
inline constexpr unsigned GlassCookieSize = 256;
// Panes whose triangles lie within this distance form one window (lead came between panes).
inline constexpr float GlassJoinDistance = 6;
inline constexpr unsigned GlassWindowLimit = 64;

struct GlassTriangle { Vec3 a, b, c; };
struct GlassWindow {
    std::vector<uint32_t> triangles; // indices into the flattened triangle list
    Vec3 low{1e20f, 1e20f, 1e20f}, high{-1e20f, -1e20f, -1e20f};
};

// Groups glass triangles into windows: triangles whose bounds (grown by `join`) overlap are one
// window. Union-find over a sort-and-sweep on x keeps this near-linear for a map's glass.
inline std::vector<GlassWindow> GroupGlassWindows(const std::vector<GlassTriangle>& triangles,
                                                  float join = GlassJoinDistance) {
    const size_t n = triangles.size();
    std::vector<Vec3> lo(n), hi(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& t = triangles[i];
        lo[i] = {(std::min)({t.a.x, t.b.x, t.c.x}) - join * .5f, (std::min)({t.a.y, t.b.y, t.c.y}) - join * .5f,
                 (std::min)({t.a.z, t.b.z, t.c.z}) - join * .5f};
        hi[i] = {(std::max)({t.a.x, t.b.x, t.c.x}) + join * .5f, (std::max)({t.a.y, t.b.y, t.c.y}) + join * .5f,
                 (std::max)({t.a.z, t.b.z, t.c.z}) + join * .5f};
    }
    std::vector<uint32_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return lo[a].x < lo[b].x; });
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = order[i];
        for (size_t j = i + 1; j < n && lo[order[j]].x <= hi[a].x; ++j) {
            const uint32_t b = order[j];
            if (lo[b].y <= hi[a].y && hi[b].y >= lo[a].y && lo[b].z <= hi[a].z && hi[b].z >= lo[a].z)
                parent[find(a)] = find(b);
        }
    }
    std::vector<GlassWindow> windows;
    std::vector<int> slot(n, -1);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t root = find(i);
        if (slot[root] < 0) { slot[root] = int(windows.size()); windows.emplace_back(); }
        auto& w = windows[size_t(slot[root])];
        w.triangles.push_back(i);
        for (Vec3 p : {triangles[i].a, triangles[i].b, triangles[i].c}) {
            w.low = {(std::min)(w.low.x, p.x), (std::min)(w.low.y, p.y), (std::min)(w.low.z, p.z)};
            w.high = {(std::max)(w.high.x, p.x), (std::max)(w.high.y, p.y), (std::max)(w.high.z, p.z)};
        }
    }
    // Largest windows first, so a cap on the count drops the smallest fragments.
    std::sort(windows.begin(), windows.end(), [](const GlassWindow& a, const GlassWindow& b) {
        return a.triangles.size() > b.triangles.size();
    });
    return windows;
}

// World -> beam clip, row-major like every record matrix (clip = rows * (p, 1)). x,y span the
// window as seen from the sun. Its oblique near plane follows the window plane, rather than
// the deepest vertex along the light: a perpendicular depth slab skips blockers behind the
// upper part of a tall window. z is 0 just behind the pane and 1 after `depth` along the light.
// `points` is a triangle stream; `towardSun` points toward the sun.
struct GlassBeam { float matrix[16]{}; bool valid = false; };
inline GlassBeam GlassBeamProjection(const std::vector<Vec3>& points, Vec3 towardSun, float depth = GlassBeamDepth) {
    GlassBeam beam;
    const float length = std::sqrt(towardSun.x * towardSun.x + towardSun.y * towardSun.y + towardSun.z * towardSun.z);
    if (points.size() < 3 || !(length > 0) || !(depth > 0)) return beam;
    const Vec3 w = towardSun * (-1.f / length); // the light's travel direction
    const Vec3 helper = std::abs(w.z) < .9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    Vec3 u = cross(helper, w);
    u = u * (1.f / std::sqrt(dot(u, u)));
    const Vec3 v = cross(w, u);
    Vec3 normal{};float area=0;
    for(size_t i=0;i+2<points.size();i+=3){
        const auto n=cross(points[i+1]-points[i],points[i+2]-points[i]);
        const float a=dot(n,n);if(a>area){normal=n;area=a;}
    }
    if(!(area>1e-8f))return beam;
    normal=normal*(1/std::sqrt(area));
    if(dot(normal,w)<0)normal=normal*(-1);
    const float travel=dot(normal,w);
    if(!(travel>1e-4f))return beam; // a grazing ray cannot transmit through this pane
    float lu = 1e30f, hu = -1e30f, lv = 1e30f, hv = -1e30f, deepest = -1e30f;
    for (Vec3 p : points) {
        lu = (std::min)(lu, dot(p, u)); hu = (std::max)(hu, dot(p, u));
        lv = (std::min)(lv, dot(p, v)); hv = (std::max)(hv, dot(p, v));
        deepest = (std::max)(deepest, dot(p, normal));
    }
    const float su = (std::max)(.5f, .5f * (hu - lu)), sv = (std::max)(.5f, .5f * (hv - lv));
    const float cu = .5f * (hu + lu), cv = .5f * (hv + lv), start = deepest + .25f * travel;
    const Vec3 axes[3]{u * (1 / su), v * (1 / sv), normal * (1 / (depth * travel))};
    const float offsets[3]{-cu / su, -cv / sv, -start / (depth * travel)};
    for (int r = 0; r < 3; ++r) {
        beam.matrix[r * 4 + 0] = axes[r].x; beam.matrix[r * 4 + 1] = axes[r].y;
        beam.matrix[r * 4 + 2] = axes[r].z; beam.matrix[r * 4 + 3] = offsets[r];
    }
    beam.matrix[15] = 1;
    beam.valid = true;
    return beam;
}
inline Vec3 BeamClip(const GlassBeam& beam, Vec3 p) {
    const float* m = beam.matrix;
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
// Cookie W coordinate of window `index` in a Texture3D of `count` slices.
inline float GlassCookieSlice(unsigned index, unsigned count) { return (float(index) + .5f) / float((std::max)(1u, count)); }

// The beam record: directional (row 8 carries the direction toward the sun, w 0), flat
// attenuation, the beam projection in rows 0-3, the cookie selector in radiance.w and a unit
// cookie scale in row 14. No shadow map: the glass would shadow its own beam.
inline LightRecord GlassBeamRecord(const GlassBeam& beam, Vec3 towardSun, Vec3 radiance, float cookieSlice) {
    LightRecord record = SunRecord(towardSun, radiance, beam.matrix, false);
    record[43] = cookieSlice;
    record[56] = 0; record[57] = 0; record[58] = 1; record[59] = 1;
    return record;
}
static_assert(GlassBeamTiles >= GlassWindowLimit, "one shadow tile per stained glass window");
// The beam's own shadow map: the world drawn in the beam's projection, which starts just behind the
// glass, so walls and floors stop the beam while the glass itself does not.
inline void ShadowGlassBeam(LightRecord& record, unsigned beam) {
    TileRect(GlassBeamTile + beam, &record[52]);
    record[65] = 1;
}
inline bool IsGlassBeamRecord(const LightRecord& record) { return record[35] == 0 && record[43] > 0; }
} // namespace Filmmaker::Relight
