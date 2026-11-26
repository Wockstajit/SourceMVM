#pragma once
// THE SAND'S OWN MAPS (Assets/environment/sand, packed by FX/tools/build-sand-texture.py from a CC0
// material): laid out as CS2's snow material is, so the sand coating draws through the snow's path
// (GameSnowAt in EnvironmentClimateShaders.h):
//   colour  RGBA  colour (sRGB) and, in alpha, the cavities
//   normal  RGB   the normal, hemi-octahedral in r and g (CS2's HemiOctAnisoRoughness), roughness in b
//   height  R     the height
// They ship as one PNG each without mips; the hook builds the chain when it loads them. Each level is
// a 2 x 2 average of the one above, the way that keeps each map's meaning: colour in linear light,
// normals as vectors (decoded, averaged, renormalised and encoded again), the rest as plain numbers.
// Engine-free so Automation/tests/sand-texture-math-tests.cpp can check it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::Environment {
enum class SandMipKind { Colour, HemiOctNormal, Linear };

inline float SandSrgbToLinear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
inline float SandLinearToSrgb(float c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f;
}
// CS2's hemi-octahedral normal, as GameSnowAt decodes it: r, g in 0..1 to a unit normal with z up.
inline void SandDecodeHemiOct(float r, float g, float n[3]) {
    const float tx = r + g - 1, ty = r - g;
    n[0] = tx;
    n[1] = ty;
    n[2] = (std::max)(1 - std::abs(tx) - std::abs(ty), .05f);
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; ++k) n[k] /= l;
}
inline void SandEncodeHemiOct(const float n[3], float &r, float &g) {
    const float z = (std::max)(n[2], 1e-4f);
    const float s = std::abs(n[0]) + std::abs(n[1]) + z;
    const float px = n[0] / s, py = n[1] / s;
    r = (px + py + 1) * .5f;
    g = (px - py + 1) * .5f;
}
inline uint8_t SandByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

// The next level down: half the size each way (at least 1), each texel the average of the 2 x 2 above.
inline std::vector<uint8_t> SandHalve(const std::vector<uint8_t> &above, int w, int h, SandMipKind kind) {
    const int nw = (std::max)(w / 2, 1), nh = (std::max)(h / 2, 1);
    std::vector<uint8_t> out(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float sum[4] = {0, 0, 0, 0}, normal[3] = {0, 0, 0};
            int count = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = (std::min)(x * 2 + dx, w - 1), sy = (std::min)(y * 2 + dy, h - 1);
                    const uint8_t *t = &above[(size_t(sy) * w + sx) * 4];
                    float v[4] = {t[0] / 255.f, t[1] / 255.f, t[2] / 255.f, t[3] / 255.f};
                    if (kind == SandMipKind::Colour)
                        for (int k = 0; k < 3; ++k) v[k] = SandSrgbToLinear(v[k]);
                    if (kind == SandMipKind::HemiOctNormal) {
                        float n[3];
                        SandDecodeHemiOct(v[0], v[1], n);
                        for (int k = 0; k < 3; ++k) normal[k] += n[k];
                    }
                    for (int k = 0; k < 4; ++k) sum[k] += v[k];
                    ++count;
                }
            uint8_t *o = &out[(size_t(y) * nw + x) * 4];
            for (int k = 0; k < 4; ++k) sum[k] /= float(count);
            if (kind == SandMipKind::Colour)
                for (int k = 0; k < 3; ++k) sum[k] = SandLinearToSrgb(sum[k]);
            if (kind == SandMipKind::HemiOctNormal) {
                const float l = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (l > 1e-6f) {
                    for (int k = 0; k < 3; ++k) normal[k] /= l;
                } else {
                    normal[0] = normal[1] = 0;
                    normal[2] = 1;
                }
                SandEncodeHemiOct(normal, sum[0], sum[1]);
            }
            for (int k = 0; k < 4; ++k) o[k] = SandByte(sum[k]);
        }
    return out;
}

// The whole chain from a full-size RGBA8 image down to 1 x 1, [0] being the image itself. Empty when
// the image is not a power of two on both sides (the shipped maps are 1024 x 1024).
inline std::vector<std::vector<uint8_t>> SandMipChain(std::vector<uint8_t> image, int w, int h, SandMipKind kind) {
    std::vector<std::vector<uint8_t>> chain;
    const auto pow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    if (!pow2(w) || !pow2(h) || image.size() != size_t(w) * h * 4) return chain;
    chain.push_back(std::move(image));
    while (w > 1 || h > 1) {
        chain.push_back(SandHalve(chain.back(), w, h, kind));
        w = (std::max)(w / 2, 1);
        h = (std::max)(h / 2, 1);
    }
    return chain;
}
} // namespace Filmmaker::Environment
