#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSandTextureMath.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace Filmmaker::Environment;

static void Check(bool ok, const char *why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

int main() {
    {
        // sRGB <-> linear round-trip across both the linear toe and gamma curve.
        for (int i = 0; i <= 255; ++i) {
            const float s = float(i) / 255.f;
            const float back = SandLinearToSrgb(SandSrgbToLinear(s));
            Check(std::abs(back - s) < 1e-4f, "sRGB <-> linear round-trip holds across 0..255");
        }
        Check(SandSrgbToLinear(.5f) < .25f, "Mid-grey sRGB is well below 0.5 in linear light");
    }
    {
        // Hemi-octahedral normal encoding round-trip (CS2 HemiOctAnisoRoughness layout).
        const float flat[3] = {0, 0, 1};
        float r = 0, g = 0, out[3] = {};
        SandEncodeHemiOct(flat, r, g);
        Check(std::abs(r - .5f) < 1e-5f && std::abs(g - .5f) < 1e-5f, "Up normal encodes to (0.5, 0.5)");
        SandDecodeHemiOct(r, g, out);
        Check(std::abs(out[0]) < 1e-4f && std::abs(out[1]) < 1e-4f && std::abs(out[2] - 1.f) < 1e-4f,
              "Flat normal decodes back to (0, 0, 1)");

        const float samples[][3] = {
            {.5f, .25f, .8291562f},
            {-.6f, .4f, .6928203f},
            {.2f, -.7f, .6855655f},
            {-.35f, -.35f, .8689074f},
        };
        for (const auto &n : samples) {
            SandEncodeHemiOct(n, r, g);
            Check(r >= 0.f && r <= 1.f && g >= 0.f && g <= 1.f, "Encoded hemi-oct stays in [0, 1]");
            SandDecodeHemiOct(r, g, out);
            const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
            Check(std::abs(len - 1.f) < 1e-4f, "Decoded hemi-oct normal is unit length");
            Check(std::abs(out[0] - n[0]) < 2e-3f && std::abs(out[1] - n[1]) < 2e-3f && std::abs(out[2] - n[2]) < 2e-3f,
                  "Tilted hemi-oct normals round-trip accurately in every quadrant");
        }
    }
    {
        // Mip chain validation & level sizes.
        Check(SandMipChain(std::vector<uint8_t>(3 * 3 * 4), 3, 3, SandMipKind::Linear).empty(),
              "Non-power-of-two texture is rejected");
        Check(SandMipChain(std::vector<uint8_t>(4 * 4 * 4 - 1), 4, 4, SandMipKind::Linear).empty(),
              "Wrong byte count is rejected");
        auto chain = SandMipChain(std::vector<uint8_t>(8 * 8 * 4, 200), 8, 8, SandMipKind::Linear);
        Check(chain.size() == 4, "8x8 mip chain has 4 levels (8, 4, 2, 1)");
        Check(chain[0].size() == 8 * 8 * 4 && chain[1].size() == 4 * 4 * 4 &&
                  chain[2].size() == 2 * 2 * 4 && chain[3].size() == 4,
              "Each mip level halves both dimensions down to 1x1");
        Check(chain[3][0] == 200 && chain[3][3] == 200, "Constant linear image stays constant through mips");
    }
    {
        // Colour mips average in linear light, keeping alpha linear (cavities/AO).
        std::vector<uint8_t> checker = {
            0, 0, 0, 0,       255, 255, 255, 255,
            255, 255, 255, 0, 0, 0, 0, 255,
        };
        auto mip = SandHalve(checker, 2, 2, SandMipKind::Colour);
        Check(mip.size() == 4, "2x2 halves to 1x1");
        // Linear average of 0 and 1 is 0.5 -> sRGB ~ 0.735 -> byte 188 (not gamma-space 128).
        Check(mip[0] >= 185 && mip[0] <= 191 && mip[1] == mip[0] && mip[2] == mip[0],
              "Colour mip averages in linear light rather than sRGB bytes");
        Check(mip[3] == 128, "Alpha channel (cavities/AO) averages linearly");
    }
    {
        // Hemi-oct normal mips average decoded 3D vectors (opposing tilts cancel to flat up),
        // while roughness in B averages as a plain number.
        const float left[3] = {-.6f, 0.f, .8f};
        const float right[3] = {.6f, 0.f, .8f};
        float lr = 0, lg = 0, rr = 0, rg = 0;
        SandEncodeHemiOct(left, lr, lg);
        SandEncodeHemiOct(right, rr, rg);
        const uint8_t lrb = SandByte(lr), lgb = SandByte(lg);
        const uint8_t rrb = SandByte(rr), rgb = SandByte(rg);
        std::vector<uint8_t> normals = {
            lrb, lgb, 100, 255,  rrb, rgb, 200, 255,
            lrb, lgb, 100, 255,  rrb, rgb, 200, 255,
        };
        auto mip = SandHalve(normals, 2, 2, SandMipKind::HemiOctNormal);
        float avg[3] = {};
        SandDecodeHemiOct(mip[0] / 255.f, mip[1] / 255.f, avg);
        Check(std::abs(avg[0]) < .02f && std::abs(avg[1]) < .02f && avg[2] > .99f,
              "Opposing hemi-oct normals average back to flat vertical");
        Check(mip[2] == 150, "Roughness in B averages linearly across the 2x2 block");
    }
    std::cout << "Sand texture math passed\n";
    return 0;
}
