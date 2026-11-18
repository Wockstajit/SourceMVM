// The wind fluid's grid and what counts as a building in the wind's way.
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentWindFluidMath.h"
#include <cstdlib>
#include <iostream>

using namespace Filmmaker::Environment;
static void Check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAILED: " << what << "\n";
    std::exit(1);
}
int main() {
    {
        const WindGrid g = WindGridForMap({-3000, -2000, 0}, {3000, 2000, 600});
        Check(g.Valid() && g.cell == kWindMinCell, "An ordinary map gets the finest cells");
        Check(g.originX < -3000 && g.originX + g.width * g.cell > 3000 && g.originY < -2000 && g.originY + g.height * g.cell > 2000,
              "The grid covers the map with open cells round it");
        const WindGrid big = WindGridForMap({-20000, -20000, 0}, {20000, 20000, 600});
        Check(big.width <= kWindMaxCells && big.height <= kWindMaxCells && big.cell > kWindMinCell, "A huge map gets wider cells, not more of them");
        Check(!WindGridForMap({0, 0, 0}, {0, 100, 0}).Valid(), "A map with no extent has no grid");
    }
    {
        // A street 10 cells wide between two houses 200 units tall, and a broad plateau 90 units up.
        const int w = 120, h = 40;
        const float cell = 24;
        std::vector<float> tops(size_t(w) * h, 0.f);
        auto at = [&](int x, int y) -> float & { return tops[size_t(y) * w + x]; };
        for (int y = 0; y < h; ++y) {
            for (int x = 10; x < 20; ++x) at(x, y) = 200; // a house
            for (int x = 30; x < 40; ++x) at(x, y) = 200; // the house across the street
            for (int x = 50; x < 120; ++x) at(x, y) = 90; // a plateau 1680 units wide
        }
        const auto solid = WindObstacles(tops, w, h, cell);
        auto is = [&](int x, int y) { return solid[size_t(y) * w + x] != 0; };
        Check(is(10, 20) && is(19, 20) && is(30, 20) && is(39, 20), "The faces of a house on a street are walls to the wind");
        Check(!is(25, 20) && !is(5, 20), "The street between the houses and the open ground are not");
        Check(!is(100, 20), "The top of a plateau is open: the wind blows over it");
        Check(!is(51, 20), "Nor is its edge, lower than a house: the wind blows over a ledge");
        {
            // A slope rising 20 units a cell (40 degrees) is ground, not a wall.
            std::vector<float> slope(size_t(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) slope[size_t(y) * w + x] = x * 20.f;
            bool any = false;
            for (uint8_t s : WindObstacles(slope, w, h, cell)) any = any || s;
            Check(!any, "A slope a person can walk up is not in the wind's way");
        }
        Check(WindObstacles(tops, w, h - 1, cell).size() == tops.size() && WindObstacles(tops, w, h - 1, cell)[0] == 0, "Mismatched sizes give an empty mask");
        // The map the fluid reads: walls as before, and for the air how open the ground around it is.
        const auto air = WindAirMap(tops, w, h, cell);
        auto open = [&](int x, int y) { return air[size_t(y) * w + x] / 127.f; };
        Check(air[size_t(20) * w + 10] == 255 && air[size_t(20) * w + 39] == 255, "The house faces are still walls in the air map");
        Check(open(25, 20) > .3f && open(25, 20) < .7f, "A street between two houses is only partly open: air also goes over the roofs");
        Check(open(100, 20) == 1.f, "The top of a broad plateau is fully open");
        Check(open(25, 20) < open(5, 20) && open(5, 20) < open(100, 20), "The more roofs around a place, the less open it is");
        bool fits = true;
        for (uint8_t m : air) fits = fits && (m == 255 || m <= 127);
        Check(fits, "Air cells stay in the map's low half, so the fluid still reads a wall as more than half");
        const auto mismatched = WindAirMap(tops, w, h - 1, cell);
        bool allOpen = mismatched.size() == tops.size();
        for (uint8_t m : mismatched) allOpen = allOpen && m == 127;
        Check(allOpen, "Mismatched sizes leave the air fully open rather than holding it still");
    }
    {
        // The noise the gust volume is torn by: spread over its range, and seamless where it repeats.
        const int n = 32;
        const auto v = GustNoiseVolume(n);
        Check(v.size() == size_t(n) * n * n, "The noise volume is size cubed");
        int lo = 255, hi = 0;
        double mean = 0;
        for (uint8_t b : v) { lo = (std::min)(lo, int(b)); hi = (std::max)(hi, int(b)); mean += b; }
        mean /= double(v.size());
        Check(lo < 40 && hi > 215, "The noise spans dark gaps and dense lumps");
        Check(mean > 70 && mean < 185, "The noise is neither mostly gap nor mostly lump");
        auto at = [&](int x, int y, int z) { return int(v[(size_t(z) * n + y) * n + x]); };
        double inner = 0, seamX = 0, seamY = 0, seamZ = 0;
        for (int a = 0; a < n; ++a)
            for (int b = 0; b < n; ++b) {
                inner += std::abs(at(n / 2, a, b) - at(n / 2 - 1, a, b));
                seamX += std::abs(at(0, a, b) - at(n - 1, a, b));
                seamY += std::abs(at(a, 0, b) - at(a, n - 1, b));
                seamZ += std::abs(at(a, b, 0) - at(a, b, n - 1));
            }
        Check(seamX <= inner * 1.6 + n * n && seamY <= inner * 1.6 + n * n && seamZ <= inner * 1.6 + n * n, "The noise repeats without a seam");
        Check(GustNoiseVolume(3).empty(), "A volume too small to hold the noise is empty");
    }
    std::cout << "Wind fluid math passed\n";
}
