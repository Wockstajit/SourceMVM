// AltEnvironmentMath.h: the alignment between a demo's map and its replacement world, the world
// offset that realises it, the top-down floor rasters and AUTO ALIGN.
#include "../../AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironmentMath.h"
#include "../../AfxHookSource2/Filmmaker/Platform/PngWrite.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace Filmmaker::AltEnvironment;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
bool Near(Vec3 a, Vec3 b, double eps) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

void TestYaw() {
    CHECK(Near(NormalizeYaw(190), -170, 1e-9));
    CHECK(Near(NormalizeYaw(-190), 170, 1e-9));
    CHECK(Near(NormalizeYaw(180), 180, 1e-9));
    CHECK(Near(NormalizeYaw(-180), 180, 1e-9));
    CHECK(Near(NormalizeYaw(720 + 45), 45, 1e-9));
    CHECK(NormalizeYaw(NAN) == 0);
    // Positive yaw turns +x towards +y.
    CHECK(Near(RotateYaw({1, 0, 5}, 90), Vec3{0, 1, 5}, 1e-12));
}

void TestAlignmentRoundTrip() {
    const Alignment a{812, -344, 64, 37};
    const Vec3 p{-1200.5, 640.25, 96};
    const Vec3 m = DemoToMap(a, p);
    CHECK(Near(MapToDemo(a, m), p, 1e-9));
    CHECK(IsIdentity(Alignment{}));
    CHECK(!IsIdentity(Alignment{0, 0, 0, 0.5}));
    CHECK(!IsIdentity(a));
}

// The engine moves the replacement world, not the players: a point of the replacement map has to
// be drawn where the demo's own coordinates put it.
void TestWorldOffset() {
    const Alignment a{812, -344, 64, 37};
    const WorldOffset w = WorldOffsetFor(a);
    CHECK(Near(w.yaw, -37, 1e-9));
    for (const Vec3 q : {Vec3{0, 0, 0}, Vec3{1500, -200, 32}, Vec3{-900, 2400, -64}}) {
        CHECK(Near(ApplyWorldOffset(w, q), MapToDemo(a, q), 1e-9));
        // ... so a player standing on demo point p sees the replacement floor under DemoToMap(p).
        const Vec3 p = MapToDemo(a, q);
        CHECK(Near(ApplyWorldOffset(w, DemoToMap(a, p)), p, 1e-9));
    }
    const WorldOffset none = WorldOffsetFor(Alignment{});
    CHECK(Near(none.pos, Vec3{}, 1e-12) && none.yaw == 0);
    // A demo's own prefab placement reads the same way: the prefab origin lands at the offset.
    const WorldOffset prefab{{2420.78, 2107.06, 134}, 147.02};
    CHECK(Near(ApplyWorldOffset(prefab, {}), prefab.pos, 1e-9));
}

void TestRotateAbout() {
    const Alignment a{100, 200, 8, 10};
    const Alignment b = RotateAbout(a, 640, -320, 25);
    CHECK(Near(b.yaw, 35, 1e-9));
    CHECK(Near(DemoToMap(a, {640, -320, 0}), DemoToMap(b, {640, -320, 0}), 1e-9));
    CHECK(b.z == a.z);
    CHECK(Near(RotateAbout(a, 0, 0, 350).yaw, 0, 1e-9));
}

// ---- synthetic maps -------------------------------------------------------------------------------

struct Mesh {
    std::vector<float> xyz;
    std::vector<uint32_t> idx;
    void Vertex(Vec3 p) { xyz.push_back(float(p.x)), xyz.push_back(float(p.y)), xyz.push_back(float(p.z)); }
    // An upward-facing rectangle, transformed by `a` (identity for the demo map itself).
    void Floor(double x0, double y0, double x1, double y1, double z, const Alignment &a = {}) {
        const uint32_t base = uint32_t(xyz.size() / 3);
        Vertex(DemoToMap(a, {x0, y0, z}));
        Vertex(DemoToMap(a, {x1, y0, z}));
        Vertex(DemoToMap(a, {x1, y1, z}));
        Vertex(DemoToMap(a, {x0, y1, z}));
        for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + k);
    }
    FloorRaster Raster(double cell) const { return BuildFloorRaster(xyz.data(), xyz.size() / 3, idx.data(), idx.size(), cell); }
};

// Rooms and corridors at three heights, deliberately lopsided so only one yaw fits.
void Level(Mesh &m, const Alignment &a = {}) {
    m.Floor(0, 0, 1200, 800, 0, a);          // A
    m.Floor(1200, 300, 2600, 500, 0, a);     // B, corridor east
    m.Floor(2600, -600, 3400, 900, 96, a);   // C, raised room
    m.Floor(400, 800, 600, 2400, -64, a);    // D, corridor north
    m.Floor(-400, 2400, 1000, 3000, -64, a); // E
    m.Floor(2800, 600, 3000, 800, 200, a);   // F, platform over C
}

std::vector<Vec3> Feet() {
    std::vector<Vec3> f;
    for (int k = 0; k < 40; ++k) f.push_back({100.0 + k * 25, 400, 0});      // across A
    for (int k = 0; k < 30; ++k) f.push_back({1300.0 + k * 40, 400, 0});     // along B
    for (int k = 0; k < 20; ++k) f.push_back({500, 900.0 + k * 70, -64});    // up D
    for (int k = 0; k < 20; ++k) f.push_back({2700.0 + k * 30, 0, 96});      // in C
    return f;
}

void TestRaster() {
    Mesh m;
    m.Floor(0, 0, 512, 512, 0);
    m.Floor(0, 0, 256, 256, 64); // a box on the floor: its top wins
    // A wall and a ceiling face never count as floor.
    const uint32_t b = uint32_t(m.xyz.size() / 3);
    m.Vertex({600, 0, 0}), m.Vertex({600, 512, 0}), m.Vertex({600, 512, 128});
    m.idx.insert(m.idx.end(), {b, b + 1, b + 2});
    const uint32_t c = uint32_t(m.xyz.size() / 3);
    m.Vertex({0, 0, 300}), m.Vertex({0, 512, 300}), m.Vertex({512, 512, 300}); // clockwise from above: faces down
    m.idx.insert(m.idx.end(), {c, c + 1, c + 2});
    const FloorRaster r = m.Raster(16);
    CHECK(r.Valid());
    CHECK(r.cell == 16 && r.minX == 0 && r.minY == 0);
    CHECK(r.width == 38 && r.height == 33); // 0..600 and 0..512, inclusive end cell
    CHECK(Near(r.Sample(100, 100), 64, 1e-3));
    CHECK(Near(r.Sample(400, 400), 0, 1e-3));
    CHECK(std::isnan(r.Sample(560, 100)));  // beside the floor
    CHECK(Near(r.lowZ, 0, 1e-6) && Near(r.highZ, 64, 1e-6));
    // 32 x 32 cell centres, plus the three corner cells at x or y = 512 the corners mark.
    CHECK(r.FloorCount() == size_t(32 * 32 + 3));
    // Downsampling keeps the highest surface per coarse cell.
    const FloorRaster d = Downsample(r, 4);
    CHECK(d.cell == 64 && d.width == 10 && Near(d.Sample(100, 100), 64, 1e-3));
    CHECK(!BuildFloorRaster(nullptr, 0, nullptr, 0, 16).Valid());
    CHECK(CellFor(5000, 512, 8) == 10 && CellFor(1000, 512, 8) == 8);
}

void TestShade() {
    Mesh m;
    m.Floor(0, 0, 160, 160, 0);
    m.Floor(0, 0, 64, 160, 100);
    const FloorRaster r = m.Raster(16);
    const auto px = ShadeFloor(r, {0, 0, 0, 255}, {255, 255, 255, 255}, {255, 0, 0, 255});
    CHECK(px.size() == size_t(r.width) * size_t(r.height) * 4);
    auto at = [&](int i, int j) { return px.data() + (size_t(r.height - 1 - j) * size_t(r.width) + size_t(i)) * 4; };
    CHECK(at(0, 0)[0] == 255 && at(0, 0)[1] == 0);          // outer edge
    CHECK(at(2, 5)[0] == 255 && at(2, 5)[1] == 255);        // high floor: light
    CHECK(at(7, 5)[0] == 0 && at(7, 5)[3] == 255);          // low floor: dark
    CHECK(at(3, 5)[1] == 0 || at(4, 5)[1] == 0);            // the 100-unit step is an edge
    std::vector<uint8_t> stamped = px;
    StampPoints(stamped, r, {{80, 80, 0}}, {0, 255, 0, 255});
    CHECK(at(5, 5)[0] == 0 && stamped[(size_t(r.height - 1 - 5) * size_t(r.width) + 5) * 4 + 1] == 255);
    // clearOnly leaves painted pixels alone and fills transparent ones.
    std::vector<uint8_t> layer = ShadeFloor(r, {0, 0, 0, 0}, {0, 0, 0, 0}, {255, 0, 0, 255});
    StampPoints(layer, r, {{8, 8, 0}, {80, 80, 0}}, {0, 0, 255, 128}, 0, true);
    const size_t edgeAt = (size_t(r.height - 1) * size_t(r.width)) * 4, inside = (size_t(r.height - 1 - 5) * size_t(r.width) + 5) * 4;
    CHECK(layer[edgeAt] == 255 && layer[edgeAt + 2] == 0);                 // the corner edge stays red
    CHECK(layer[inside + 2] == 255 && layer[inside + 3] == 128);           // the floor cell takes the stamp
}

// A cell keeps several floors: an arch over a street must not hide the street.
void TestLayers() {
    float l[FloorRaster::kLayers];
    for (float &v : l) v = NAN;
    KeepLayer(l, 0);
    KeepLayer(l, 200);
    KeepLayer(l, 10);   // within 16 of 0: the same floor, the higher height wins
    KeepLayer(l, 100);
    CHECK(l[0] == 200 && l[1] == 100 && l[2] == 10 && std::isnan(l[3]));
    KeepLayer(l, 300);
    KeepLayer(l, -50);  // a fifth floor: the lowest drops off
    CHECK(l[0] == 300 && l[1] == 200 && l[2] == 100 && l[3] == 10);
    KeepLayer(l, 105);  // merges into 100 and keeps the order
    CHECK(l[2] == 105 && l[3] == 10);

    Mesh m;
    m.Floor(0, 0, 512, 128, 0);     // a street
    m.Floor(128, 0, 256, 128, 160); // an arch over part of it
    const FloorRaster r = m.Raster(16);
    CHECK(Near(r.Sample(200, 64), 160, 1e-3));                 // the picture sees the arch
    CHECK(r.FloorNearAt(200, 64, 0, 48, 16));                  // the street under it is still there
    CHECK(!r.FloorNearAt(200, 64, 80, 48, 16));                // nothing at mid height
    const FloorRaster d = Downsample(r, 4);
    CHECK(d.FloorNearAt(200, 64, 0, 48, 16) && d.FloorNearAt(200, 64, 160, 1, 1));
    // Feet walking the street under the arch all count.
    std::vector<Vec3> feet;
    for (int k = 0; k < 20; ++k) feet.push_back({20.0 + k * 24, 64, 0});
    CHECK(Near(Evaluate(r, r, feet, Alignment{}).players, 1, 1e-9));
}

void TestEvaluate() {
    Mesh demo;
    Level(demo);
    const FloorRaster d = demo.Raster(16);
    const Fit same = Evaluate(d, d, Feet(), Alignment{});
    CHECK(Near(same.floor, 1, 1e-9) && Near(same.players, 1, 1e-9));
    const Fit off = Evaluate(d, d, Feet(), Alignment{0, 0, 200, 0});
    CHECK(off.floor < 0.2 && off.players < 0.3); // lifted 200 units: nothing matches
    double step = 0;
    CHECK(BestHeightStep(d, d, Alignment{}, 1, step) && Near(step, 0, 1e-6));
}

void TestAutoAlignIdentity() {
    Mesh demo, night;
    Level(demo);
    Level(night);
    night.Floor(-3000, -3000, -2000, -2000, 0); // the remake added a courtyard nobody uses
    const AutoAlignResult r = AutoAlign(demo.Raster(16), night.Raster(16), Feet());
    CHECK(r.ok && r.identity);
    CHECK(IsIdentity(r.alignment, 0.5, 0.01));
    CHECK(r.fit.floor > 0.99 && r.fit.players > 0.99);
}

void TestAutoAlignRecovers(const Alignment truth, bool feet) {
    Mesh demo, other;
    Level(demo);
    Level(other, truth);
    other.Floor(truth.x - 5000, truth.y - 5000, truth.x - 4200, truth.y - 4400, 0); // unrelated extra area
    const FloorRaster d = demo.Raster(16), t = other.Raster(16);
    int calls = 0;
    const AutoAlignResult r = AutoAlign(d, t, feet ? Feet() : std::vector<Vec3>{}, AutoAlignOptions{}, [&](double) { ++calls; });
    CHECK(r.ok && !r.identity && calls > 0);
    std::printf("  truth (%.0f %.0f %.0f %.1f) found (%.1f %.1f %.1f %.2f) floor %.3f players %.3f\n", truth.x, truth.y,
                truth.z, truth.yaw, r.alignment.x, r.alignment.y, r.alignment.z, r.alignment.yaw, r.fit.floor, r.fit.players);
    CHECK(Near(NormalizeYaw(r.alignment.yaw - truth.yaw), 0, 0.6));
    // Compare where the level's middle lands rather than the raw offset: a small yaw error moves
    // the offset of a point far from the origin.
    const Vec3 mid{1500, 1200, 0};
    CHECK(Near(DemoToMap(r.alignment, mid).x, DemoToMap(truth, mid).x, 24));
    CHECK(Near(DemoToMap(r.alignment, mid).y, DemoToMap(truth, mid).y, 24));
    CHECK(Near(r.alignment.z, truth.z, 4));
    CHECK(r.fit.floor > 0.9);
    if (feet) CHECK(r.fit.players > 0.95);
}
// Where a standing point lands: on a floor (any layer within reach), floating over one, under the map
// (floor only above), or outside it; the counts; and the map view's coloured demo layer.
void TestLanding() {
    Mesh target;
    target.Floor(0, 0, 1000, 1000, 0);
    target.Floor(0, 0, 500, 500, 200); // a balcony over the floor's corner
    const FloorRaster t = target.Raster(16);
    CHECK(LandingAt(t, {800, 800, 0}) == Landing::On);
    CHECK(LandingAt(t, {800, 800, 30}) == Landing::On);        // 30 over the floor: within the 48 reach
    CHECK(LandingAt(t, {800, 800, 100}) == Landing::Floating); // 100 over it
    CHECK(LandingAt(t, {800, 800, -100}) == Landing::Under);   // floor only above
    CHECK(LandingAt(t, {2000, 2000, 0}) == Landing::Outside);  // no floor in that place
    CHECK(LandingAt(t, {250, 250, 200}) == Landing::On);       // on the balcony
    CHECK(LandingAt(t, {250, 250, 0}) == Landing::On);         // and on the floor under it
    CHECK(LandingAt(t, {250, 250, 100}) == Landing::Floating); // between the two: a floor below
    const std::vector<Vec3> feet = {{100, 100, 0}, {900, 900, 0}, {900, 900, 120}, {3000, 0, 0}};
    const Landings same = CountLandings(t, feet, Alignment{});
    CHECK(same.on == 2 && same.floating == 1 && same.under == 0 && same.outside == 1 && same.Total() == 4);
    CHECK(Near(same.Share(same.on), 0.5, 1e-9));
    const Landings moved = CountLandings(t, feet, Alignment{5000, 0, 0, 0});
    CHECK(moved.outside == 4);

    // The picture: a demo floor that lands on the target is tinted green, one moved off it red, and
    // a walked foot draws on top in the solid colour.
    Mesh demo;
    demo.Floor(0, 0, 512, 512, 0);
    const FloorRaster d = demo.Raster(16);
    auto pixel = [&](const std::vector<uint8_t> &px, int i, int j) { return px.data() + (size_t(d.height - 1 - j) * size_t(d.width) + size_t(i)) * 4; };
    Landings counts;
    const std::vector<uint8_t> on = LandingPicture(d, t, {}, Alignment{}, &counts);
    const uint8_t *mid = pixel(on, d.width / 2, d.height / 2);
    CHECK(mid[0] == 74 && mid[1] == 222 && mid[2] == 128 && mid[3] == 72);
    const std::vector<uint8_t> off = LandingPicture(d, t, {{256, 256, 0}}, Alignment{5000, 0, 0, 0}, &counts);
    const uint8_t *foot = pixel(off, d.CellX(256), d.CellY(256));
    CHECK(foot[0] == 248 && foot[1] == 82 && foot[3] == 235);
    const uint8_t *beside = pixel(off, d.CellX(256) + 4, d.CellY(256));
    CHECK(beside[0] == 248 && beside[3] == 72);
    CHECK(counts.outside == 1 && counts.Total() == 1);
    const uint8_t *edge = pixel(off, 0, d.height / 2);
    CHECK(edge[0] == 236 && edge[3] == 220); // the demo map's outline stays light
}

// Two maps that share no layout: the demo's rooms over a field of narrow strips. The search still
// finds a best place, but it fits almost nothing, so it is no match and the alignment is not offered.
void TestAutoAlignRefusesUnrelated() {
    Mesh demo, strips;
    Level(demo);
    for (int k = 0; k < 8; ++k) strips.Floor(-2000.0 + k * 700, -3000, -1952.0 + k * 700, 4000, k * 48.0);
    const AutoAlignResult r = AutoAlign(demo.Raster(16), strips.Raster(16), Feet());
    std::printf("  unrelated: ok %d floor %.3f players %.3f\n", r.ok ? 1 : 0, r.fit.floor, r.fit.players);
    CHECK(!r.ok && !r.identity);
    CHECK(r.fit.floorCells > 0 && r.fit.Score() < AutoAlignOptions{}.minAccept);
}
// The search runs on several threads, merged in a fixed order: one thread and four give the same
// answer to the last bit, and every worker runs the init hook (the game's priority drop).
void TestAutoAlignThreads() {
    const Alignment truth{812, -344, 64, 37};
    Mesh demo, other;
    Level(demo);
    Level(other, truth);
    const FloorRaster d = demo.Raster(16), t = other.Raster(16);
    AutoAlignOptions one;
    one.threads = 1;
    AutoAlignOptions four;
    four.threads = 4;
    std::atomic<int> inits{0};
    four.workerInit = [&] { ++inits; };
    const AutoAlignResult a = AutoAlign(d, t, Feet(), one), b = AutoAlign(d, t, Feet(), four);
    CHECK(a.ok && b.ok);
    CHECK(a.alignment.x == b.alignment.x && a.alignment.y == b.alignment.y);
    CHECK(a.alignment.z == b.alignment.z && a.alignment.yaw == b.alignment.yaw);
    CHECK(a.fit.floor == b.fit.floor && a.fit.players == b.fit.players);
    CHECK(inits.load() >= 3); // three workers per pass beside the calling thread
}
// The PNG the top-down pictures are served as: chunks with valid CRCs, one IDAT of stored deflate
// blocks whose Adler-32 matches, and the pixels back out unchanged.
void TestPng(int width, int height) {
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4);
    for (size_t k = 0; k < rgba.size(); ++k) rgba[k] = uint8_t((k * 31 + k / 7) & 0xff);
    const std::string png = Filmmaker::EncodePngRgba(width, height, rgba.data());
    CHECK(png.size() > 8 && png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
    auto be32 = [&](size_t at) {
        return (uint32_t(uint8_t(png[at])) << 24) | (uint32_t(uint8_t(png[at + 1])) << 16) | (uint32_t(uint8_t(png[at + 2])) << 8) |
               uint32_t(uint8_t(png[at + 3]));
    };
    std::string idat, types;
    for (size_t at = 8; at + 12 <= png.size();) {
        const uint32_t len = be32(at);
        const std::string type = png.substr(at + 4, 4);
        types += type + " ";
        const uint32_t crc = Filmmaker::PngCrc(reinterpret_cast<const uint8_t *>(png.data() + at + 4), len + 4) ^ 0xffffffffu;
        CHECK(crc == be32(at + 8 + len));
        if (type == "IHDR") CHECK(be32(at + 8) == uint32_t(width) && be32(at + 12) == uint32_t(height));
        if (type == "IDAT") idat = png.substr(at + 8, len);
        at += 12 + len;
    }
    CHECK(types == "IHDR IDAT IEND ");
    // zlib header, then stored blocks
    CHECK(idat.size() > 6 && uint8_t(idat[0]) == 0x78);
    std::string raw;
    size_t i = 2;
    bool last = false;
    while (!last && i + 5 <= idat.size()) {
        last = (idat[i] & 1) != 0;
        CHECK((idat[i] & 6) == 0); // stored
        const uint16_t len = uint16_t(uint8_t(idat[i + 1]) | (uint8_t(idat[i + 2]) << 8));
        const uint16_t nlen = uint16_t(uint8_t(idat[i + 3]) | (uint8_t(idat[i + 4]) << 8));
        CHECK(uint16_t(~len) == nlen);
        raw += idat.substr(i + 5, len);
        i += 5 + len;
    }
    CHECK(last && i + 4 == idat.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) a = (a + c) % 65521u, b = (b + a) % 65521u;
    const uint32_t adler = (uint32_t(uint8_t(idat[i])) << 24) | (uint32_t(uint8_t(idat[i + 1])) << 16) |
                           (uint32_t(uint8_t(idat[i + 2])) << 8) | uint32_t(uint8_t(idat[i + 3]));
    CHECK(adler == ((b << 16) | a));
    CHECK(raw.size() == size_t(height) * (size_t(width) * 4 + 1));
    bool same = true;
    for (int y = 0; y < height && same; ++y) {
        same = raw[size_t(y) * (size_t(width) * 4 + 1)] == 0; // filter: none
        for (int x = 0; x < width * 4 && same; ++x)
            same = uint8_t(raw[size_t(y) * (size_t(width) * 4 + 1) + 1 + size_t(x)]) == rgba[size_t(y) * size_t(width) * 4 + size_t(x)];
    }
    CHECK(same);
}
} // namespace

int main() {
    TestPng(3, 2);
    TestPng(300, 220); // several 64 KB stored blocks
    CHECK(Filmmaker::EncodePngRgba(0, 5, nullptr).empty());
    TestYaw();
    TestAlignmentRoundTrip();
    TestWorldOffset();
    TestRotateAbout();
    TestRaster();
    TestShade();
    TestLayers();
    TestEvaluate();
    TestAutoAlignIdentity();
    TestAutoAlignRecovers(Alignment{812, -344, 64, 37}, true);
    TestAutoAlignRecovers(Alignment{-2000, 1500, -128, -90}, false);
    TestAutoAlignRecovers(Alignment{300, 260, 0, 0}, true);
    TestAutoAlignRecovers(Alignment{4096, 4096, 512, 163.5}, true);
    TestAutoAlignThreads();
    TestAutoAlignRefusesUnrelated();
    TestLanding();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("alt-environment math: all checks passed\n");
    return 0;
}
