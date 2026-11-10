// Ground fog: the floor map the layer is measured from, the fluid grid over the map,
// the demo clock it steps on and the player bodies that push it (EnvironmentGroundFogMath.h); plus
// which transparent coverage counts as smoke and the fog's sky light (EnvironmentClimateMath.h).
#include <cstring>
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFogMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateMath.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *name) {
    if (!ok) { std::cerr << "FAILED: " << name << "\n"; std::exit(1); }
}
bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

struct Soup {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    // An axis-aligned horizontal rectangle at height z, wound to face up (or down).
    void Rect(float x0, float y0, float x1, float y1, float z, bool up = true) {
        const uint32_t base = uint32_t(positions.size());
        positions.push_back({x0, y0, z});
        positions.push_back({x1, y0, z});
        positions.push_back({x1, y1, z});
        positions.push_back({x0, y1, z});
        if (up) indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        else indices.insert(indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    // A ramp rising along x from z0 to z1.
    void Ramp(float x0, float x1, float y0, float y1, float z0, float z1) {
        const uint32_t base = uint32_t(positions.size());
        positions.push_back({x0, y0, z0});
        positions.push_back({x1, y0, z1});
        positions.push_back({x1, y1, z1});
        positions.push_back({x0, y1, z0});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    GroundFogFloorMap Build(const Soup *nav = nullptr) const {
        Vec3 low{1e9f, 1e9f, 1e9f}, high{-1e9f, -1e9f, -1e9f};
        for (const auto &p : positions) {
            low = {(std::min)(low.x, p.x), (std::min)(low.y, p.y), (std::min)(low.z, p.z)};
            high = {(std::max)(high.x, p.x), (std::max)(high.y, p.y), (std::max)(high.z, p.z)};
        }
        return nav ? BuildGroundFogFloorMap(positions, indices, low, high, {}, nav->positions, nav->indices)
                   : BuildGroundFogFloorMap(positions, indices, low, high);
    }
};

int main() {
    // ---- floor map ------------------------------------------------------------------------
    {
        Soup soup;
        soup.Rect(-512, -512, 512, 512, 0);    // the ground
        soup.Rect(100, -512, 400, 512, 128);   // a balcony over it
        soup.Rect(-40, -40, 8, 8, 64);         // a crate, 48 units across
        soup.Rect(-512, -512, 512, 512, 300, false); // a ceiling: faces down, never a floor
        const auto map = soup.Build();
        Check(map.Valid(), "a floor map is built from a triangle soup");
        Check(Near(GroundFogFloorBelow(map, -300, 200, 30), 0, 1), "open ground is the floor");
        Check(Near(GroundFogFloorBelow(map, 250, 0, 150), 128, 2), "fog on a balcony lies on the balcony");
        Check(Near(GroundFogFloorBelow(map, 250, 0, 60), 0, 1), "under the balcony the ground is the floor");
        Check(Near(GroundFogFloorBelow(map, 108, 0, 150), 128, 2), "a floor runs right up to its ledge");
        Check(Near(GroundFogFloorBelow(map, -16, -16, 80), 0, 1), "a crate stands out of the fog instead of carrying it");
        Check(GroundFogFloorBelow(map, -300, 200, 400) < 150, "a downward face (a ceiling) is not a floor");
        Check(GroundFogFloorBelow(map, 5000, 5000, 0) <= kGroundFogNoFloor * .5f, "outside the map there is no floor");
    }
    {
        Soup soup;
        soup.Rect(-512, -512, 0, 512, 0);
        soup.Ramp(0, 400, -512, 512, 0, 100);
        soup.Rect(400, -512, 912, 512, 100);
        const auto map = soup.Build();
        const float mid = GroundFogFloorBelow(map, 200, 0, 60);
        Check(Near(mid, 50, 3), "a ramp's floor is the ramp, so fog stays on stairs instead of sinking below them");
        Check(GroundFogFloorBelow(map, 100, 0, 30) < GroundFogFloorBelow(map, 300, 0, 80), "the floor rises with the ramp");
    }
    {
        // Fog lies only on the walkable ground: a roof is a piece of its own behind tall walls.
        Soup soup;
        soup.Rect(-1024, -1024, 1024, 1024, 0);
        soup.Rect(200, 200, 500, 500, 320);          // a roof, far above the ground around it
        soup.Rect(-800, -800, -500, -500, 30);       // a low platform, a step up from the ground
        soup.Rect(-800, 200, -300, 700, 200);        // an upper floor...
        soup.Ramp(-300, 300, 200, 700, 200, 200);    // ...and its walkway...
        const auto map = soup.Build();
        Check(GroundFogFloorBelow(map, 350, 350, 340) < 10, "a roof does not carry fog: the floor under it is the ground");
        Check(Near(GroundFogFloorBelow(map, -650, -650, 60), 30, 2), "a low platform joined to the ground keeps its fog");
        Soup upper;
        upper.Rect(-1024, -1024, 0, 1024, 0);
        upper.Ramp(0, 512, -1024, 1024, 0, 128);     // a long ramp up...
        upper.Rect(512, -1024, 1024, 1024, 128);     // ...to an upper level
        upper.Rect(600, 0, 700, 100, 500);           // and a separate roof above it
        const auto level = upper.Build();
        Check(Near(GroundFogFloorBelow(level, 800, -500, 150), 128, 2), "an upper level reached by a ramp keeps its fog");
        Check(Near(GroundFogFloorBelow(level, 650, 50, 520), 128, 2), "a roof above an upper level does not");
    }
    {
        // How much of the ray lies in front of an effect.
        const Vec3 camera{0, 0, 0}, ahead{1, 0, 0};
        float known = 0;
        std::vector<FogEffectAnchor> anchors{{{1000, 0, 0}, 150}};
        Check(std::abs(FogEffectShare(camera, ahead, 4000, anchors.data(), 1, known) - (1000 - 45) / 4000.f) < 1e-4f && known == 1,
              "A fire on the ray: the fog up to it lies in front of it, the rest behind");
        Check(FogEffectShare(camera, ahead, 500, anchors.data(), 1, known) == 1 && known == 1,
              "A fire past the surface counts as at the surface");
        Check(FogEffectShare(camera, {-1, 0, 0}, 4000, anchors.data(), 1, known) == 1 && known == 0,
              "An anchor behind the camera says nothing");
        Check(FogEffectShare(camera, {0, 1, 0}, 4000, anchors.data(), 1, known) == 1 && known == 0,
              "An anchor well off the ray says nothing");
        // The certainty fades across the anchor's edge instead of cutting off.
        float last = 2;
        bool smooth = true, reachedBoth = false;
        for (float miss = 40; miss <= 180; miss += 10) {
            const float length = std::sqrt(1000 * 1000 + miss * miss);
            FogEffectShare(camera, {1000 / length, miss / length, 0}, 4000, anchors.data(), 1, known);
            smooth = smooth && known <= last && last - known < .25f + (last == 2 ? 2 : 0);
            reachedBoth = reachedBoth || known == 0;
            last = known;
        }
        Check(smooth && reachedBoth, "How sure an anchor is falls off smoothly to nothing at its edge");
        // Two anchors on one ray share it, so one ending does not make the fog jump.
        anchors.push_back({{3000, 0, 0}, 150});
        const float both = FogEffectShare(camera, ahead, 4000, anchors.data(), 2, known);
        Check(both > 955 / 4000.f && both < 2955 / 4000.f, "Anchors on one ray average");
        anchors.clear();
        for (int i = 0; i < kFogEffectAnchors + 5; ++i) anchors.push_back({{float(kFogEffectAnchors + 5 - i) * 100, 0, 0}, 50});
        KeepNearestFogEffects(anchors, camera);
        Check(int(anchors.size()) == kFogEffectAnchors && anchors[0].position.x == 100, "A frame keeps the nearest anchors");
    }
    {
        // The nav file's layout, as CS2 writes it: a header of some length, then the two tables.
        std::vector<uint8_t> bytes;
        const auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(v >> (8 * i))); };
        const auto putF = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(v); };
        put32(0xFEEDFACE);
        put32(36);
        put32(1);
        for (int i = 0; i < 33; ++i) bytes.push_back(uint8_t(i == 0 ? 1 : 0)); // 145 bytes in, as on a v36 file
        while (bytes.size() < 141) bytes.push_back(0);
        put32(5);
        const float corners[5][3] = {{0, 0, 10}, {100, 0, 10}, {100, 100, 20}, {0, 100, 20}, {200, 50, 30}};
        for (const auto &c : corners) for (float f : c) putF(f);
        put32(2);
        bytes.push_back(4);
        for (uint32_t i : {0u, 1u, 2u, 3u}) put32(i);
        put32(0xFFFFFFFF);
        bytes.push_back(3);
        for (uint32_t i : {1u, 4u, 2u}) put32(i);
        put32(7);
        for (int i = 0; i < 40; ++i) bytes.push_back(0xAB); // the area data that follows
        std::vector<Vec3> points;
        std::vector<uint32_t> triangles;
        Check(ParseGroundFogNav(bytes.data(), bytes.size(), points, triangles), "a nav file's walkable polygons are found");
        Check(points.size() == 5 && points[4].x == 200 && points[2].z == 20, "its corners are read");
        Check(triangles.size() == 9 && triangles[3] == 0 && triangles[5] == 3 && triangles[6] == 1 && triangles[8] == 2,
              "a four-sided polygon becomes two triangles and a three-sided one stays one");
        auto broken = bytes;
        broken[0] = 0;
        Check(!ParseGroundFogNav(broken.data(), broken.size(), points, triangles) && points.empty(), "a file that is not a nav file is refused");
        broken = bytes;
        broken[141 + 4 + 60 + 4 + 1] = 99; // a polygon pointing past the corner list
        Check(!ParseGroundFogNav(broken.data(), broken.size(), points, triangles), "a table that does not check out is refused");
        Check(!ParseGroundFogNav(bytes.data(), 40, points, triangles), "a cut-off file is refused");
        Check(!ParseGroundFogNav(nullptr, 0, points, triangles), "no file is refused");
    }
    {
        // Where people walk decides where fog lies, not how the geometry happens to join up.
        Soup soup;
        soup.Rect(-1024, -1024, 0, 1024, 0);       // the street
        soup.Ramp(0, 100, -1024, 1024, 0, 30);     // a slope up...
        soup.Rect(100, -1024, 500, 1024, 30);      // ...onto a roof that joins the street by geometry alone
        soup.Rect(-900, 1200, -700, 1500, 300);    // a raised walkway geometry says is a separate scrap
        soup.Rect(-1024, -1200, -600, -1100, 0);   // a strip of ground the nav mesh stops short of
        Soup nav;
        nav.Rect(-1000, -1000, -16, 1000, 0);      // nav stops a player's width short of the walls
        nav.Rect(-884, 1216, -716, 1484, 300);     // and it covers the walkway
        nav.Rect(-400, -400, -100, -100, 700);     // a floor collision does not have at all
        const auto guess = soup.Build();
        const auto known = soup.Build(&nav);
        Check(GroundFogFloorBelow(guess, 300, 0, 60) > 20, "by geometry alone, a roof joined to the street carries fog");
        Check(GroundFogFloorBelow(guess, -800, 1350, 320) <= kGroundFogNoFloor * .5f, "by geometry alone, a raised walkway has none");
        Check(GroundFogFloorBelow(known, 300, 0, 60) <= kGroundFogNoFloor * .5f, "the nav mesh says nobody stands on that roof");
        Check(Near(GroundFogFloorBelow(known, -800, 1350, 320), 300, 2), "the nav mesh says people walk the raised walkway");
        Check(Near(GroundFogFloorBelow(known, -8, 0, 20), 0, 2), "a floor beside the nav mesh, against a wall, still counts");
        Check(GroundFogFloorBelow(known, -800, -1150, 20) <= kGroundFogNoFloor * .5f, "a floor far from the nav mesh does not");
        Check(Near(GroundFogFloorBelow(known, -250, -250, 720), 700, 2), "a place people walk has fog even with no collision floor there");
        // A crate people can jump on is in the nav mesh. Its top still stands out of the fog.
        Soup crates;
        crates.Rect(-1024, -1024, 1024, 1024, 0);
        crates.Rect(-40, -40, 40, 40, 64);
        Soup crateNav;
        crateNav.Rect(-1000, -1000, 1000, 1000, 0);
        crateNav.Rect(-24, -24, 24, 24, 64);
        Check(Near(GroundFogFloorBelow(crates.Build(&crateNav), 0, 0, 80), 0, 1), "a crate top in the nav mesh is still too small to carry fog");
    }
    {
        // Fog drains toward any real drop: thin at a ledge, full away from it, no hard cut.
        Soup soup;
        soup.Rect(-2048, -2048, 0, 2048, 0);     // the ground
        soup.Ramp(0, 600, -2048, 2048, 0, 30);   // a gentle rise, joined to the ground
        soup.Rect(600, -2048, 2048, 2048, 30);   // a broad terrace at its top...
        soup.Rect(1400, -2048, 2048, 2048, 150); // ...and a much higher block past it
        const auto map = soup.Build();
        float held = 0;
        GroundFogFloorBelow(map, -1000, 0, 20, 8, &held);
        Check(held == 1, "open ground far from any drop holds the full layer");
        float nearDrop = 0, midDrop = 0;
        GroundFogFloorBelow(map, 1400 + 12, 0, 170, 8, &nearDrop);
        GroundFogFloorBelow(map, 1400 + 56, 0, 170, 8, &midDrop);
        Check(nearDrop < .3f, "a floor right at a ledge holds almost no fog");
        Check(midDrop > nearDrop && midDrop < 1, "fog thins smoothly toward the ledge, with no hard cut");
        float pastDrain = 0;
        GroundFogFloorBelow(map, 1400 + 140, 0, 170, 8, &pastDrain);
        Check(pastDrain == 1, "a street beside a drop keeps its full layer a little way from the edge");
        float gentle = 0;
        GroundFogFloorBelow(map, 300, 0, 40, 8, &gentle);
        Check(gentle == 1, "a gentle slope is not a drop, and keeps the full layer");
    }
    {
        // Stairs drop as far as a ledge does, but they are walked down: fog stays on them and above them.
        Soup soup;
        soup.Rect(-2048, -2048, 0, 2048, 0);    // the street
        soup.Ramp(0, 200, -2048, 2048, 0, 120); // a flight of stairs, 120 units up in 200
        soup.Rect(200, -2048, 2048, 2048, 120); // the upper street
        const auto map = soup.Build();
        float top = 0, onStairs = 0, street = 0;
        GroundFogFloorBelow(map, 230, 0, 130, 8, &top);
        GroundFogFloorBelow(map, 100, 0, 70, 8, &onStairs);
        GroundFogFloorBelow(map, -60, 0, 10, 8, &street);
        Check(top == 1 && onStairs == 1 && street == 1, "stairs are not a ledge: fog stays on them, above them and below them");
    }
    {
        // No floor in between: fog must not float halfway up a wall.
        Soup soup;
        soup.Rect(-1024, -1024, 0, 1024, 0);
        soup.Rect(0, -1024, 1024, 1024, 300);
        const auto map = soup.Build();
        bool jump = true;
        for (float x = -96; x <= 160; x += 2) {
            const float h = GroundFogFloorBelow(map, x, 0, 320);
            jump = jump && (h < 40 || h > 260);
        }
        Check(jump, "the floor next to a tall ledge is one height or the other, never an average of them");
    }
    {
        std::vector<float> heights{500, 400, 300, 200, 100, 0};
        const auto column = GroundFogMergeColumn(heights, 24);
        Check(column[0] == 500 && column[1] == 400 && column[2] == 300 && column[3] == 0,
              "a column of six floors keeps the three highest and the ground");
        std::vector<float> slab{128, 120, 112};
        const auto one = GroundFogMergeColumn(slab, 24);
        Check(one[0] == 128 && one[1] <= kGroundFogNoFloor * .5f, "a slab's faces merge into one floor at its top");
        Check(GroundFogLayerBelow(column, 350) == 300 && GroundFogLayerBelow(column, -1) <= kGroundFogNoFloor * .5f,
              "the floor below a height is the highest floor at or under it");
    }
    Check(GroundFogProfile(0, 56) == 1 && GroundFogProfile(56, 56) == 0 && GroundFogProfile(-5, 56) == 1,
          "the layer is full at the floor and empty at its top");
    {
        float last = 2;
        bool falling = true;
        for (float h = 0; h <= 60; h += 2) { const float v = GroundFogProfile(h, 56); falling &= v <= last; last = v; }
        Check(falling, "the layer thins toward its top without bands");
    }

    // ---- how the fluid shapes the layer ------------------------------------------------------
    {
        // Softness: from a sheet with a surface to mist with none.
        Check(GroundFogProfile(40, 56, 0) > .4f && GroundFogProfile(56, 56, 0) == 0, "a hard layer stops at its top");
        Check(GroundFogProfile(0, 56, 1) == 1, "soft or hard, fog is full at the floor");
        Check(GroundFogProfile(56, 56, 1) > 0 && GroundFogProfile(56 * 2.2f, 56, 1) == 0, "soft fog reaches above the top, and still ends");
        Check(GroundFogProfile(20, 56, 1) < GroundFogProfile(20, 56, 0), "soft fog is thinner low down, not just taller");
        bool falling = true;
        float last = 2;
        for (float h = 0; h <= 130; h += 2) {
            const float v = GroundFogProfile(h, 56, .85f);
            falling = falling && v <= last;
            last = v;
        }
        Check(falling, "soft fog only ever thins going up");
    }
    Check(GroundFogDensityShare(0) == 0 && GroundFogTopShare(0) == 0, "a cleared trail is a trench down to the floor");
    Check(GroundFogDensityShare(1) == 1 && Near(GroundFogTopShare(1), 1, 1e-5f), "fog at rest is the layer as authored");
    Check(GroundFogDensityShare(1.5f) > 1 && GroundFogTopShare(1.5f) > 1, "fog shoved aside is denser and stands taller");
    Check(GroundFogDensityShare(.5f) < .6f && GroundFogTopShare(.5f) < 1, "thinned fog is thinner and sits lower");
    Check(GroundFogTopShare(1.6f) == GroundFogTopShare(1.25f), "heaped fog cannot spike above a fixed share of the layer");
    {
        const auto fresh = GroundFogExplosion({1, 2, 3}, 0), late = GroundFogExplosion({1, 2, 3}, .5f);
        Check(fresh.push > 1000 && fresh.radius < late.radius, "an explosion starts as a hard push and grows");
        Check(late.push == 0 && late.radius > 300, "after the shock it leaves a clear hole");
    }

    // ---- the fluid grid -------------------------------------------------------------------
    {
        const auto grid = GroundFogGridForMap({-3500, -2800, -100}, {3900, 4300, 900});
        Check(grid.Valid() && grid.cell == kGroundFogMinCell, "a normal map gets the finest cells");
        Check(grid.originX <= -3500 && grid.originY <= -2800, "the grid starts at or before the map's corner");
        Check(grid.originX + grid.width * grid.cell >= 3900 && grid.originY + grid.height * grid.cell >= 4300,
              "the grid reaches past the map's far corner");
        const auto again = GroundFogGridForMap({-3500, -2800, -100}, {3900, 4300, 900});
        Check(grid == again, "the grid is fixed by the map, so a render does not depend on the camera");
        const auto huge = GroundFogGridForMap({-20000, -20000, 0}, {20000, 20000, 900});
        Check(huge.width <= kGroundFogMaxCells && huge.height <= kGroundFogMaxCells && huge.cell > kGroundFogMinCell,
              "a huge map gets coarser cells instead of a larger grid");
        Check(huge.originX + huge.width * huge.cell >= 20000, "coarser cells still reach the far side");
        const auto flat = GroundFogGridForMap({0, 0, 0}, {0, 100, 10});
        Check(!flat.Valid(), "a map with no extent has no grid");
        const auto around = GroundFogGridAround({1000, -500, 64});
        Check(around.Valid() && around.originX < 1000 && around.originX + around.width * around.cell > 1000 &&
              around.originY < -500 && around.originY + around.height * around.cell > -500,
              "without map data the grid surrounds the camera");
        Check(!GroundFogGridAround({NAN, 0, 0}).Valid(), "no camera, no grid");
    }

    // ---- the clock ------------------------------------------------------------------------
    {
        GroundFogClock clock;
        Check(GroundFogTick(clock, 10).reset, "the first frame starts the fog from rest");
        auto paused = GroundFogTick(clock, 10);
        Check(!paused.reset && paused.dt == 0 && paused.steps == 0, "a paused demo freezes the fog");
        auto play = GroundFogTick(clock, 10 + 1.0 / 64);
        Check(!play.reset && Near(play.dt, 1.f / 64, 1e-5f) && play.steps == 1, "a tick of playback is one step");
        auto slow = GroundFogTick(clock, 10 + 1.0 / 64 + 1.0 / 30);
        Check(slow.steps == 2, "a longer frame is split so a running player stays within a few cells per step");
        // The demo clock steps back by a few milliseconds during ordinary playback (issue seen live).
        const double now = 10 + 1.0 / 64 + 1.0 / 30;
        auto jitter = GroundFogTick(clock, now - .021);
        Check(!jitter.reset && jitter.dt == 0, "a small backward clock step is not a seek and simulates nothing");
        auto behind = GroundFogTick(clock, now - .01);
        Check(!behind.reset && behind.dt == 0, "time already simulated is not simulated twice");
        auto caught = GroundFogTick(clock, now + .01);
        Check(!caught.reset && Near(caught.dt, .01f, 1e-4f), "playback carries on from the latest time");
        Check(GroundFogTick(clock, 5).reset, "seeking backwards starts the fog from rest");
        Check(GroundFogTick(clock, 7).reset, "skipping ahead starts the fog from rest");
        Check(!GroundFogTick(clock, 7.01).reset, "playback continues after a seek");
    }

    // ---- the drift ------------------------------------------------------------------------
    {
        // A steady wind adds up to time x wind, so nothing changes for a fog nobody keys.
        GroundFogDrift d;
        GroundFogDriftTick(d, 1500, 12, 0);
        Check(Near(float(d.x), 18000.f, .01f), "the first frame starts at time x wind");
        for (int i = 1; i <= 64; ++i) GroundFogDriftTick(d, 1500 + i / 64.0, 12, 0);
        Check(Near(float(d.x), 1501 * 12.f, .01f), "a steady wind adds up to time x wind");
        // Drift speed 12 -> 45 late in a demo: the pattern moves on from where it is, at the new
        // speed. It used to jump by 1501 x 33 = 49,533 units in one frame.
        const double before = d.x;
        GroundFogDriftTick(d, 1501 + 1 / 64.0, 45, 0);
        Check(Near(float(d.x - before), 45.f / 64, .001f), "a faster wind only changes the next step");
        // A new direction turns the motion, not the place.
        const double x = d.x, y = d.y;
        GroundFogDriftTick(d, 1501 + 2 / 64.0, 0, 45);
        Check(Near(float(d.x), float(x), .001f) && Near(float(d.y - y), 45.f / 64, .001f), "a new direction turns the motion");
        // Paused, and the small backward steps of playback: the pattern holds.
        const double hx = d.x, hy = d.y;
        GroundFogDriftTick(d, 1501 + 2 / 64.0, 0, 45);
        GroundFogDriftTick(d, 1501 + 2 / 64.0 - .02, 0, 45);
        Check(d.x == hx && d.y == hy, "a paused demo and a small backward step hold the drift");
        // A seek starts again from time x wind, so a render that seeks to its start is repeatable.
        GroundFogDriftTick(d, 300, 20, 5);
        Check(Near(float(d.x), 6000.f, .01f) && Near(float(d.y), 1500.f, .01f), "a seek starts from time x wind");
        // No wrap: the noise is not periodic, so a wrap was a pop.
        GroundFogDrift far;
        GroundFogDriftTick(far, 7000, 200, 0);
        Check(Near(float(far.x), 1400000.f, 1.f), "a long demo at full wind does not wrap");
        GroundFogDrift bad;
        GroundFogDriftTick(bad, std::nan(""), 1, 1);
        Check(!bad.valid, "a bad clock changes nothing");
    }

    // ---- bodies ---------------------------------------------------------------------------
    {
        std::vector<GroundFogTrackedPawn> tracked;
        const float dt = 1.f / 64;
        auto first = GroundFogTrackBodies(tracked, {{3, {0, 0, 0}}}, dt, 22);
        Check(first.size() == 1 && first[0].vx == 0 && first[0].radius == 22, "a first sighting pushes nothing");
        auto second = GroundFogTrackBodies(tracked, {{3, {4, 0, 0}}}, dt, 22);
        Check(second[0].vx > 100 && second[0].vx < 256, "a walking player's push follows its speed, smoothed");
        auto third = GroundFogTrackBodies(tracked, {{3, {8, 0, 0}}}, dt, 22);
        Check(third[0].vx > second[0].vx && third[0].vx <= 256.5f, "the measured speed settles on the real one");
        auto teleport = GroundFogTrackBodies(tracked, {{3, {3000, 0, 0}}}, dt, 22);
        Check(teleport[0].vx == 0 && teleport[0].vy == 0, "a respawn or teleport pushes nothing");
        auto gone = GroundFogTrackBodies(tracked, {{7, {0, 0, 0}}}, dt, 22);
        Check(tracked.size() == 1 && tracked[0].key == 7 && gone[0].vx == 0, "players that left are forgotten");
        std::vector<std::pair<int, Vec3>> crowd;
        for (int i = 0; i < 20; ++i) crowd.push_back({i, {float(i), 0, 0}});
        Check(GroundFogTrackBodies(tracked, crowd, dt, 22).size() == size_t(kGroundFogBodies), "at most the bodies the GPU takes");
    }

    // ---- which transparent coverage is smoke ------------------------------------------------
    {
        const Vec3 camera{0, 0, 64};
        const Vec3 centers[] = {{1000, 0, 64}};
        const float radii[] = {200};
        float entry = 0;
        const float through = SmokeRayShare(camera, {1, 0, 0}, 4000, centers, radii, 1, entry);
        Check(through == 1 && Near(entry, 800, 1), "a ray through a smoke is that smoke's, from its near edge");
        const Vec3 side = Normalize({1000, 1000, 0});
        Check(SmokeRayShare(camera, side, 4000, centers, radii, 1, entry) == 0 && entry < 0,
              "coverage on a ray that misses every smoke is glass, fire or foliage, never smoke");
        Check(SmokeRayShare(camera, {1, 0, 0}, 500, centers, radii, 1, entry) == 0,
              "a smoke beyond the surface does not cover it");
        const Vec3 grazing = Normalize({1000, 270, 0});
        const float edge = SmokeRayShare(camera, grazing, 4000, centers, radii, 1, entry);
        Check(edge > 0 && edge < 1, "a ray grazing a smoke's billowing edge is partly smoke, without a hard cut");
        Check(SmokeRayShare(camera, {1, 0, 0}, 4000, centers, radii, 0, entry) == 1 && entry == 0,
              "with no smoke positions every covered ray is taken as smoke from the camera on");
    }

    // ---- the fog's sky light --------------------------------------------------------------
    {
        const Vec3 neutral = FogAmbientFromMapSky({0, 0, 0}, false);
        Check(Near(neutral.x, .45f, 1e-6f) && Near(neutral.y, .5f, 1e-6f) && Near(neutral.z, .6f, 1e-6f),
              "without map data the fog keeps its neutral sky light");
        const Vec3 warm = FogAmbientFromMapSky({1, .6f, .3f}, true);
        const auto luminance = [](Vec3 c) { return c.x * .2126f + c.y * .7152f + c.z * .0722f; };
        Check(warm.x > neutral.x && warm.z < neutral.z, "a warm map sky warms the fog");
        Check(Near(luminance(warm), luminance(neutral), .05f), "a map sky changes the fog's tint, not how bright it is");
        const Vec3 dark = FogAmbientFromMapSky({0, 0, 0}, true);
        Check(Near(dark.x, neutral.x, 1e-6f), "a black authored sky falls back to the neutral light");
    }
    std::cout << "Environment ground fog math passed\n";
}
