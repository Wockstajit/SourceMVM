#pragma once
#include "../Relight/RelightLiteGridMath.h"
#include <algorithm>
#include <array>
#include <vector>
namespace Filmmaker::Environment {
// THE MAP'S OWN LAMPS IN WET GROUND. A puddle shows a placed light as a highlight (WaterLightGlint),
// and it should show the hanging lamp over it too. The map's lamps are the Relight scene's authored
// lights, read from the map data whether or not the Lighting card is on, so the composite gets the
// nearest of them here and draws each one's highlight with the placed lights' water term.
//
// Which lamps: bulbs (light_omni, light_omni2, light_spot) and shaded lamps (light_barn: Dust 2's
// hanging lamps are barn lights aimed down) the user has not switched off. Not the Scene Objects
// lights (the placed-light loop draws those), not effect lights (muzzle flashes and fires come and go
// each frame), and not light_rect: a map's rect lights are fill panels, an opening with nothing to
// see in a puddle. Their light and falloff are the lit programs' own (MakeLiteLamp, mirrored in the
// shader as LiteLampRadiance is), at `intensity`, the Map light power the native lighting uses (1
// while CS2 lights the map). The highlight is seen at the light's own position: the bulb.
//
// A SHADED LAMP'S BULB IS SEEN FROM THE SIDE. Its projection is the narrow beam it throws on the
// floor, but the bulb hangs below the shade, so a puddle beside the beam still shows it: anywhere
// below the shade, at the brightness and falloff the beam has from its eye. (Kept to the beam, the
// lamp could only show in water straight under it, where the camera never sees it.)
//
// kPuddleLampRows rows per lamp, in the shader's mapLamps[] order:
//   0: position (the bulb), squared source radius (the falloff's soft core)
//   1: linear radiance, kind (0 bulb, 1 shaded lamp)
//   2: bulb: forward, range window scale            | shaded: the beam's eye, 0
//   3: bulb: window bias, cone bias, cone scale, 0  | shaded: where it aims, 0
inline constexpr unsigned kPuddleLamps = 8;
inline constexpr unsigned kPuddleLampRows = 4;
// A bulb is small. An omni light with a source wider than this is a fill or a sky window (Dust 2's
// is 177 units across, a thousand units over the map): its highlight drew a glow on wet walls that
// nothing in the scene explained.
inline constexpr float kPuddleBulbRadius = 48;
// Past this from the camera a lamp's image in a puddle is under a pixel or two.
inline constexpr float kPuddleLampDistance = 3000;
struct PuddleLampTable {
    std::array<std::array<float, 4>, kPuddleLamps * kPuddleLampRows> rows{};
    unsigned count = 0;
};
inline bool PuddleLampCandidate(const Relight::Light& light) {
    return light.enabled && !light.sceneObject && !light.transient && !light.viewmodelEmitter &&
           light.kind != "light_rect";
}
inline PuddleLampTable PuddleLamps(const std::vector<Relight::Light>& lights, Vec3 camera, float intensity) {
    struct Pick { float score; Vec3 bulb; Relight::LiteLamp lamp; };
    std::vector<Pick> picks;
    for (const auto& light : lights) {
        if (!PuddleLampCandidate(light)) continue;
        Relight::LiteLamp lamp;
        if (!Relight::MakeLiteLamp(light, intensity, lamp)) continue;
        if (lamp.fixture && !(lamp.soft > 0)) continue; // no eye: no falloff to light the bulb by
        if (!lamp.fixture && lamp.soft > kPuddleBulbRadius * kPuddleBulbRadius) continue;
        const Vec3 d = light.position - camera;
        // How far the camera is from the lamp's reach: zero inside it, where its light is on screen.
        const float score = (std::max)(0.f, std::sqrt(Dot(d, d)) - lamp.reach);
        if (score < kPuddleLampDistance) picks.push_back({score, light.position, lamp});
    }
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score < b.score; });
    PuddleLampTable table;
    for (const auto& pick : picks) {
        if (table.count >= kPuddleLamps) break;
        const auto& l = pick.lamp;
        auto* r = &table.rows[table.count * kPuddleLampRows];
        r[0] = {pick.bulb.x, pick.bulb.y, pick.bulb.z, l.soft};
        r[1] = {l.radiance.x, l.radiance.y, l.radiance.z, l.fixture ? 1.f : 0.f};
        if (l.fixture) {
            r[2] = {l.origin.x, l.origin.y, l.origin.z, 0};
            r[3] = {l.aim.x, l.aim.y, l.aim.z, 0};
        } else {
            r[2] = {l.forward.x, l.forward.y, l.forward.z, l.windowScale};
            r[3] = {l.windowBias, l.coneBias, l.coneScale, 0};
        }
        ++table.count;
    }
    return table;
}
} // namespace Filmmaker::Environment
