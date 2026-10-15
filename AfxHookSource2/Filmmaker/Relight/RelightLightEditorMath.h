#pragma once
#include "RelightMath.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Filmmaker::Relight {
// Light editor: the user walks the map, aims at a light and switches it off or on. The choice is
// remembered per map by a key that survives reloads: the light's authored identity when it has
// one, else its class and position rounded to whole units.
inline std::string LightEditorKey(const Light& light) {
    if (!light.identity.empty()) return light.identity;
    char text[96];
    std::snprintf(text, sizeof(text), "%s@%d,%d,%d", light.kind.c_str(), int(std::lround(light.position.x)),
                  int(std::lround(light.position.y)), int(std::lround(light.position.z)));
    return text;
}

// A light's marker on screen this frame.
struct LightMarker {
    std::string key;
    float x = 0, y = 0;     // pixels
    float distance = 0;     // world units from the camera
    bool visible = false;   // in front of the camera
};

// Projects a world point with a row-major world->clip matrix. False when it is behind the camera.
inline bool ProjectToScreen(const float* worldToClip, Vec3 p, float width, float height, float& x, float& y) {
    const float* m = worldToClip;
    const float cx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    const float cy = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    const float cw = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    if (!(cw > 1e-3f)) return false;
    x = (cx / cw * .5f + .5f) * width;
    y = (.5f - cy / cw * .5f) * height;
    return true;
}

// Marker opacity by distance: 1 near, easing to a faint floor far away (never fully hidden, so a
// distant light can still be found).
inline constexpr float LightMarkerNear = 400, LightMarkerFar = 2000, LightMarkerFaint = .12f;
inline float LightMarkerOpacity(float distance) {
    const float t = (distance - LightMarkerNear) / (LightMarkerFar - LightMarkerNear);
    const float k = t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t);
    return 1 - k * (1 - LightMarkerFaint);
}

// The aimed light:the visible marker nearest the screen centre within `radius` pixels; nearer
// lights win ties (within 8 px) so a lamp in front is picked over one straight behind it.
// Only lights within reach can be picked: the editor is for the room you are standing in, not a
// lamp across the map that happens to sit under the crosshair.
inline constexpr float LightPickReach = 600;
inline int AimedLightMarker(const std::vector<LightMarker>& markers, float width, float height, float radius = 48,
                            float reach = LightPickReach) {
    int best = -1;
    float bestOffset = 0;
    for (size_t i = 0; i < markers.size(); ++i) {
        const auto& m = markers[i];
        if (!m.visible || m.distance > reach) continue;
        const float dx = m.x - width * .5f, dy = m.y - height * .5f, offset = std::sqrt(dx * dx + dy * dy);
        if (offset > radius) continue;
        if (best < 0 || offset + 8 < bestOffset ||
            (std::fabs(offset - bestOffset) <= 8 && m.distance < markers[size_t(best)].distance)) {
            best = int(i);
            bestOffset = offset;
        }
    }
    return best;
}
} // namespace Filmmaker::Relight
