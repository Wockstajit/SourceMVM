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
    }
    std::cout << "Wind fluid math passed\n";
}
