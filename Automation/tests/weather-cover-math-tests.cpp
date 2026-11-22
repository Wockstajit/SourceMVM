#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentWeatherCoverMath.h"
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}
bool Near(float a, float b, float e = .001f) { return std::abs(a - b) < e; }
struct Mesh {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    void Quad(Vec3 corner, Vec3 u, Vec3 v) {
        const uint32_t base = uint32_t(positions.size());
        positions.insert(positions.end(), {corner, corner + u, corner + u + v, corner + v});
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + i);
    }
};
// Ground everywhere at z=0. A building over x 0..320, y 0..320 with a roof at z=128 and four walls.
// The wall at x=0 has a doorway `door` units wide, centred on y=160, 96 units tall. Everything
// west of x=0 is open street.
Mesh Building(float door) {
    Mesh m;
    m.Quad({-400, -100, 0}, {900, 0, 0}, {0, 520, 0});
    m.Quad({0, 0, 128}, {320, 0, 0}, {0, 320, 0});
    m.Quad({320, 0, 0}, {0, 320, 0}, {0, 0, 128});
    m.Quad({0, 0, 0}, {320, 0, 0}, {0, 0, 128});
    m.Quad({0, 320, 0}, {320, 0, 0}, {0, 0, 128});
    const float a = 160 - door / 2, b = 160 + door / 2;
    m.Quad({0, 0, 0}, {0, a, 0}, {0, 0, 128});
    m.Quad({0, b, 0}, {0, 320 - b, 0}, {0, 0, 128});
    if (door > 0) m.Quad({0, a, 96}, {0, door, 0}, {0, 0, 32});
    else m.Quad({0, a, 0}, {0, b - a, 0}, {0, 0, 128});
    return m;
}
struct Baked {
    Mesh mesh;
    SurfaceRayGrid rays;
    WeatherCoverBake bake;
    std::vector<uint8_t> texels;
    explicit Baked(Mesh m) : mesh(std::move(m)) {
        Check(BuildSurfaceRayGrid(mesh.positions, mesh.indices, rays), "The ray grid builds");
        // Offset from the walls by a few units: a wall exactly on a voxel face is no test of anything.
        bake.Resize(MakeWeatherCoverLayout({-394, -90, -27}, {502, 422, 229}));
        const WeatherCoverMesh view{&mesh.positions, &mesh.indices, &rays};
        WeatherCoverColumns(view, bake, 0, size_t(bake.layout.nx) * bake.layout.ny);
        WeatherCoverJoins(view, bake, 0, bake.layout.Cells());
        WeatherCoverSolve(bake, [](size_t count, auto body) { body(size_t(0), count); });
        WeatherCoverTraffic(bake, [](size_t count, auto body) { body(size_t(0), count); });
        texels = WeatherCoverTexels(bake);
    }
    // As a floor reads it: half a voxel above the ground.
    float Floor(float x, float y) const { return WeatherCoverSample(bake.layout, texels, {x, y, 8}); }
    float Traffic(float x, float y) const { return WeatherCoverSample(bake.layout, texels, {x, y, 8}, 1); }
};
int main() {
    {
        const auto l = MakeWeatherCoverLayout({0, 0, 0}, {1600, 800, 320});
        Check(l.nx == 100 && l.ny == 50 && l.nz == 20, "Voxels are 16 units across");
        const auto huge = MakeWeatherCoverLayout({-20000, -20000, -4000}, {20000, 20000, 4000});
        Check(huge.nx == kWeatherCoverMaxWide && huge.nz == kWeatherCoverMaxTall, "A huge map widens its voxels");
        Check(Near(l.Centre(3, 2, 1).x, 56) && l.Index(3, 2, 1) == (size_t(1) * 50 + 2) * 100 + 3, "Centres and indices");
        // The loss is the one that makes a plane wave keep kWeatherCoverStepKeep per 16 units.
        const float r = kWeatherCoverStepKeep, loss = WeatherCoverLoss(16);
        Check(Near(r, (1 - loss) * (r * r + 4 * r + 1) / 6, 1e-5f), "The loss gives the intended keep per step");
        Check(WeatherCoverLoss(32) > loss, "Wider voxels lose more per step");
        Check(WeatherCoverEncode(0) == 0 && WeatherCoverEncode(1) == 255, "The ends are stored exactly");
        Check(Near(WeatherCoverDecode(WeatherCoverEncode(.3f) / 255.f), .3f, .01f), "Eighth root and back");
        // Half way between a street (1) and a sealed room (0) the blend of roots decodes to under 1 %.
        Check(WeatherCoverDecode(.5f) < .01f, "A blend across a wall stays near nothing");
    }
    {
        const Baked wide(Building(160)), narrow(Building(48)), sealed(Building(0));
        Check(wide.Floor(-200, 160) > .98f && wide.Floor(-40, 20) > .98f, "Open street is fully exposed");
        const float in1 = wide.Floor(40, 160), in2 = wide.Floor(104, 160), in3 = wide.Floor(250, 160);
        Check(in1 > .25f && in1 < 1, "Just inside a wide doorway is still well exposed");
        Check(in2 < in1 * .7f && in3 < in2 * .3f, "Exposure falls with distance from the doorway");
        Check(in3 < .05f, "The back of the room is sheltered");
        Check(narrow.Floor(104, 160) < in2 * .6f, "A narrow doorway lets in less than a wide one");
        Check(wide.Floor(40, 290) < in1 * .5f, "Beside the doorway, behind the wall, is more sheltered than in front of it");
        // No leak through the wall: 8 units inside the sealed building, right behind the street.
        float worst = 0;
        for (float y = 20; y < 300; y += 7) worst = (std::max)(worst, sealed.Floor(8, y));
        Check(worst < .02f, "A sealed room beside the street stays sheltered up to its wall");
        Check(sealed.Floor(160, 160) == 0, "A sealed room holds nothing");
        // The floor fill: a lookup right at ground level outdoors still reads open air.
        Check(WeatherCoverSample(wide.bake.layout, wide.texels, {-200, 160, 1}) > .98f, "Ground level outdoors reads the air above it");
    }
    {
        // Traffic: high in and just outside the doorway, nothing out in the street or by a blank wall.
        const Baked door(Building(96)), sealed(Building(0));
        const float threshold = door.Traffic(-24, 160), street = door.Traffic(-300, 160), wall = door.Traffic(-24, 300);
        Check(threshold > .2f, "Just outside a doorway is walked on");
        Check(street < threshold * .1f, "Traffic is gone a short way down the street");
        Check(wall < threshold * .5f, "A blank wall beside the door is walked past less than the door");
        Check(door.Traffic(200, 160) > .9f, "Indoors is the source");
        Check(sealed.Traffic(-24, 160) < .02f, "A building with no way in sends no traffic out");
        Check(door.Traffic(-200, 160) == door.Traffic(-200, 160) && door.Traffic(-200, -60) < .05f, "Open ground away from any door has none");
    }
    {
        // A sealed room whose doorway wall stops two units short of the roof, with the crack on a row
        // of voxel centres (z = 125): one ray through the crack used to join the room to the street.
        Mesh m;
        m.Quad({-400, -100, 0}, {900, 0, 0}, {0, 520, 0});
        m.Quad({0, 0, 128}, {320, 0, 0}, {0, 320, 0});
        m.Quad({320, 0, 0}, {0, 320, 0}, {0, 0, 128});
        m.Quad({0, 0, 0}, {320, 0, 0}, {0, 0, 128});
        m.Quad({0, 320, 0}, {320, 0, 0}, {0, 0, 128});
        m.Quad({0, 0, 0}, {0, 320, 0}, {0, 0, 124});
        m.Quad({0, 0, 126}, {0, 320, 0}, {0, 0, 2});
        Baked cracked(m);
        Check(cracked.Floor(160, 160) < .01f && cracked.Floor(40, 160) < .02f, "A crack under the roof does not open a sealed room");
        Baked wide(Building(96));
        Check(wide.Floor(24, 160) > .25f, "A doorway still lets the open air in");
    }
    std::cout << "Weather cover math passed\n";
}
