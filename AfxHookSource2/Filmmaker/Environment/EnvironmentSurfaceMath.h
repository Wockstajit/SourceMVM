#pragma once
#include "EnvironmentShadowMath.h"
#include <array>
#include <queue>
namespace Filmmaker::Environment {
struct SurfaceHit {
    Vec3 position, normal;
    float distance = 0;
};
inline bool SurfaceRaycast(const OcclusionMesh &world, Vec3 origin, Vec3 direction, float range,
                           SurfaceHit &hit) {
    direction = Normalize(direction);
    bool found = false;
    float nearest = range;
    for (size_t i = 0; i + 2 < world.indices.size(); i += 3) {
        Vec3 a = world.positions[world.indices[i]], b = world.positions[world.indices[i + 1]],
             c = world.positions[world.indices[i + 2]];
        Vec3 e1 = b - a, e2 = c - a, p = Cross(direction, e2);
        float determinant = Dot(e1, p);
        if (std::abs(determinant) < 1e-7f)
            continue;
        float inverse = 1 / determinant;
        Vec3 t = origin - a;
        float u = Dot(t, p) * inverse;
        if (u < 0 || u > 1)
            continue;
        Vec3 q = Cross(t, e1);
        float v = Dot(direction, q) * inverse;
        if (v < 0 || u + v > 1)
            continue;
        float distance = Dot(e2, q) * inverse;
        if (distance <= .01f || distance >= nearest)
            continue;
        nearest = distance;
        hit = {origin + direction * distance, Normalize(Cross(e1, e2)), distance};
        if (Dot(hit.normal, direction) > 0)
            hit.normal = hit.normal * -1;
        found = true;
    }
    return found;
}
} // namespace Filmmaker::Environment
