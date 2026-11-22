#pragma once
// Lit bounds of a shadowed map lamp.
//
// The light lists (RelightClusterMath.h) hold a lamp for every tile and depth slice its range
// sphere touches. Walls do not shorten a sphere, so a lamp in one room stays a candidate for the
// pixels of the rooms around it: each of them runs the shipped loop's cull test and the lamp's
// shadow lookup only to find the lamp does not reach it.
//
// A lamp of the static tier has its six cube faces drawn once from the map's rigid geometry
// (EnvironmentShadowMaps.cpp, mapStaticBase). Nothing the lamp lights lies behind that depth: the
// casters added each frame (players, props, foliage) only ever bring a face's depth nearer. So the
// farthest static depth in each direction bounds what the lamp can light, and the box around those
// bounds replaces the range sphere's box in the lists.
//
// Each 128-texel face is summarized as 8 x 8 blocks of 16 texels holding the farthest depth in the
// block. The lamp filter reads at most its guard band (12 texels) around a receiver's texel, so a
// receiver is lit only if it is no deeper than the farthest texel of its own block or a block next
// to it, plus the shadow bias. Each block's frustum from the lamp to that depth is therefore a
// superset of the block's lit points, and the box around all of them a superset of the lamp's.
#include "RelightMath.h"
#include "../Environment/EnvironmentShadowMath.h"

namespace Filmmaker::Relight {
inline constexpr unsigned LitBlocks = 8;
inline constexpr unsigned LitBlockTexels = StaticSize / LitBlocks;
inline constexpr unsigned LitFaceBlocks = LitBlocks * LitBlocks;
static_assert(LitBlockTexels * LitBlocks == StaticSize, "blocks tile a static face");
static_assert(float(LitBlockTexels) >= FaceGuardTexels, "the lamp filter stays within the next block");
// World units added on every side: rasterization of the face, the kernel's own rounding.
inline constexpr float LitBoundsPad = 16.f;
// Depth added to a block's farthest depth: receivers this much deeper than the stored depth still
// pass the comparison (twice the receiver bias, and a floor for a zero bias).
inline float LitDepthMargin(float shadowBias) {
    return 2 * (std::max)(0.f, std::isfinite(shadowBias) ? shadowBias : 0.f) + 1e-4f;
}

struct LitBounds {
    Vec3 low{}, high{};
    bool valid = false;
};

// `blockFarthest`: `faces` x 64 values, each face row-major with row 0 the top texel rows of its
// tile. `rendered`: the world-to-clip matrix each face was drawn with (guard band included), so
// clip x, y in [-1, 1] span the whole tile. A value outside [0, 1] (a NaN texel) counts as the far
// plane. Invalid when a matrix cannot be inverted or a corner is not finite: the lamp then keeps
// its range sphere.
inline LitBounds LampLitBounds(const float *blockFarthest, const Environment::Matrix *rendered, unsigned faces,
                               Vec3 position, float depthMargin, float pad = LitBoundsPad) {
    LitBounds out;
    if (!blockFarthest || !rendered || !faces || !Environment::Finite(position)) return out;
    Vec3 low = position, high = position;
    for (unsigned face = 0; face < faces; ++face) {
        Environment::Matrix inverse{};
        if (!Environment::Inverse(rendered[face], inverse)) return out;
        const float *blocks = blockFarthest + size_t(face) * LitFaceBlocks;
        for (unsigned by = 0; by < LitBlocks; ++by)
            for (unsigned bx = 0; bx < LitBlocks; ++bx) {
                float farthest = 0;
                for (unsigned y = by ? by - 1 : 0; y <= (std::min)(by + 1, LitBlocks - 1); ++y)
                    for (unsigned x = bx ? bx - 1 : 0; x <= (std::min)(bx + 1, LitBlocks - 1); ++x) {
                        const float value = blocks[y * LitBlocks + x];
                        farthest = (std::max)(farthest, value >= 0 && value <= 1 ? value : 1.f);
                    }
                const float depth = (std::min)(1.f, farthest + depthMargin);
                // The block's frustum between the face's near plane and that depth (a panel light's
                // near plane is its own surface, wider than its position).
                for (unsigned corner = 0; corner < 8; ++corner) {
                    const float x = -1 + 2 * float(bx + (corner & 1)) / LitBlocks;
                    const float y = 1 - 2 * float(by + ((corner >> 1) & 1)) / LitBlocks;
                    const Vec3 p = Environment::Transform(inverse, {x, y, corner & 4 ? 0.f : depth});
                    if (!Environment::Finite(p)) return out;
                    low = {(std::min)(low.x, p.x), (std::min)(low.y, p.y), (std::min)(low.z, p.z)};
                    high = {(std::max)(high.x, p.x), (std::max)(high.y, p.y), (std::max)(high.z, p.z)};
                }
            }
    }
    const Vec3 padding{pad, pad, pad};
    out.low = low - padding;
    out.high = high + padding;
    out.valid = Environment::Finite(out.low) && Environment::Finite(out.high);
    return out;
}

// The GPU form of one record's bounds: low.xyz, then high.xyz with w = 1 when valid.
struct LitBoundsRow {
    float low[4]{}, high[4]{};
};
static_assert(sizeof(LitBoundsRow) == 32, "cluster shader stride");
inline LitBoundsRow ToLitBoundsRow(const LitBounds &bounds) {
    if (!bounds.valid) return {};
    return {{bounds.low.x, bounds.low.y, bounds.low.z, 0}, {bounds.high.x, bounds.high.y, bounds.high.z, 1}};
}
} // namespace Filmmaker::Relight
