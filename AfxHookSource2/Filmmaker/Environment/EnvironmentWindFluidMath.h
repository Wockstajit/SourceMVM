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
} // namespace Filmmaker::Environment
