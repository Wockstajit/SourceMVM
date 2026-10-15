#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentRaySurfaceMath.h"
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    std::vector<RaySurfaceVertex> vertices = {
        {{0, 0, 0}, {0, 0, 1}, 0, 0}, {{4, 0, 0}, {0, 0, 1}, 1, 0}, {{0, 4, 0}, {0, 1, 1}, 0, 1}};
    std::vector<uint32_t> indices = {0, 1, 2}, materials = {42};
    RaySurface hit;
    Check(ResolveRaySurface(vertices, indices, materials, 0, .25f, .5f, hit), "Resolve triangle attributes");
    Check(hit.material == 42 && hit.position.x == 1 && hit.position.y == 2 && hit.u == .25f && hit.v == .5f,
          "Primitive material and barycentric attributes");
    Check(hit.normal.y > 0 && hit.normal.z > 0 && std::abs(Dot(hit.normal, hit.normal) - 1) < 1e-6f,
          "Smooth normalized shading normal");
    Check(!ResolveRaySurface(vertices, indices, materials, UINT32_MAX, 0, 0, hit), "Reject miss sentinel");
    Check(!ResolveRaySurface(vertices, indices, materials, 0, .8f, .8f, hit), "Reject invalid barycentrics");
    indices[2] = 99;
    Check(!ResolveRaySurface(vertices, indices, materials, 0, 0, 0, hit), "Reject bad vertex reference");
    indices[2] = 2;
    MaterialImage image;
    image.width = 2;
    image.height = 1;
    image.rgba = {255, 0, 0, 255, 0, 0, 255, 0};
    auto middle = SampleRayTexture(image, .5f, .5f);
    Check(std::abs(middle.color.x - .5f) < 1e-6f && std::abs(middle.color.z - .5f) < 1e-6f &&
              middle.alpha == .5f,
          "Bilinear linear color and alpha");
    auto repeated = SampleRayTexture(image, -.75f, .5f);
    Check(repeated.color.x == 1 && repeated.color.z == 0, "Negative UV repeat");
    auto clamped = SampleRayTexture(image, -2, .5f, true, false);
    Check(clamped.color.x == 1, "Clamped UV");
    image.rgba = {128, 128, 128, 255, 128, 128, 128, 255};
    auto color = SampleRayTexture(image, .5f, .5f), data = SampleRayTexture(image, .5f, .5f, false);
    Check(std::abs(color.color.x - .21586f) < .0001f && data.color.x > .5f,
          "Color transfer differs from linear data maps");
    std::cout << "Ray material math checks passed\n";
}
