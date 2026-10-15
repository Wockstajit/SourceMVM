#pragma once
#include "EnvironmentShadowMath.h"
#include <array>
namespace Filmmaker::Environment {
inline constexpr unsigned kPrecipitationInstanceBudget = 196608; // at full intensity
// PRECIPITATION: ONE DENSITY EVERYWHERE. Rain, snow and ash are a single world-anchored lattice of
// one cell size: a particle's position is a function of its cell and absolute time only, and the cell
// edge comes from the weather radius, never from the camera. The camera chooses which cells are drawn
// (a window around it), so particles never move with it.
//
// Beyond the particle window the SAME density continues as an analytic veil (WeatherVeilPS): each
// pixel integrates the expected coverage of that density along its view ray, counting only the
// stretches of the ray inside the map's precipitation region and open to the sky. Particles fade out
// over the window's edge exactly as the veil fades in, so density never changes with distance. An
// earlier design used nested bands 3x wider and 27x sparser per step: thick around the camera, thin
// further away, and a dense patch at every doorway seen from inside.
struct PrecipitationLayout {
    float cell = 0;                  // world cell edge; one particle per cell
    unsigned across = 0, layers = 0; // cells drawn per horizontal axis and vertically
    float reach = 0, reachUp = 0;    // particle window half-extents (horizontal, vertical)
    bool mapped = false;
    Vec3 low, high; // precipitation region: map XY bounds, map floor to the layer top
    unsigned Instances() const { return across * across * layers; }
};
inline constexpr float kPrecipitationCellPerRadius = .028f; // 25 units at the default radius 900
inline constexpr float kPrecipitationMapMargin = 256.f;     // outside the collision bounds
// Intensity (occupancy 0..1) sets the cell size, not the fraction of cells drawn: every cell holds a
// particle and a lighter fall uses larger cells, so the same density costs occupancy x the instances.
// (Drawing every cell and discarding most of them cost the full budget even at the lowest setting.)
inline PrecipitationLayout PrecipitationLattice(float radius, float occupancy, bool mapped, Vec3 mapLow, Vec3 mapHigh, float height) {
    PrecipitationLayout layout;
    const float full = (std::max)(radius, 64.f) * kPrecipitationCellPerRadius;
    // The window is fixed by the full-intensity lattice: twice as wide as tall, within the budget.
    unsigned n0 = unsigned(std::cbrt(float(kPrecipitationInstanceBudget) / 4));
    while (n0 > 2 && (2 * n0) * (2 * n0) * (n0 + 1) > kPrecipitationInstanceBudget) --n0;
    layout.reach = float(n0 - 1) * full;
    layout.reachUp = float(n0 / 2 - 1) * full;
    layout.cell = full / std::cbrt(std::clamp(occupancy, 1e-3f, 1.f));
    // The window starts at floor(camera / cell) - half: one cell of slack each side.
    const unsigned n = unsigned(std::ceil(layout.reach / layout.cell)) + 1, m = unsigned(std::ceil(layout.reachUp / layout.cell)) + 1;
    layout.across = 2 * n;
    layout.layers = 2 * m + 1;
    if (mapped && mapHigh.x > mapLow.x && mapHigh.y > mapLow.y && mapHigh.z >= mapLow.z) {
        layout.mapped = true;
        // Rain above the highest roof is what a camera looking up sees; keep half a layer of it.
        layout.low = {mapLow.x - kPrecipitationMapMargin, mapLow.y - kPrecipitationMapMargin, mapLow.z - 32};
        layout.high = {mapHigh.x + kPrecipitationMapMargin, mapHigh.y + kPrecipitationMapMargin,
                       mapHigh.z + (std::max)(height * .5f, 256.f)};
    }
    return layout;
}
// Mirrors of WeatherVS/WeatherPS, so the veil adds the coverage the particles would have added.
// Rain drop sizes: the size setting is the largest drop; most are smaller, down to a quarter of it
// (median about half). An earlier long-tailed spectrum reached 4x the setting and every drop read
// as the same bright white streak.
inline float RainDropVariation(float u) { return .25f + .75f * std::pow(u, 1.6f); }
// Motion blur: a streak's opacity is min(1, kRainMotionBlur * drop width / streak length).
// 40 left street-level rain invisible against lit walls; stay under ~66 so a default-length streak
// is never clamped (the veil's coverage must not depend on streak length).
inline constexpr float kRainMotionBlur = 60.f;
// Rain colour is the map's light at the drop times this: a little above the scene, so drops read
// as water catching light rather than white lines laid over it.
inline constexpr float kRainBrightness = 3.0f;
// WeatherPS rain profile gain: saturate(gain * (1-|x|)^2 (1-|y|)).
inline constexpr float kRainShapeGain = 2.4f;
// No single drop is drawn more opaque than this: the big streaks right at the lens read as solid
// white lines, while the field of smaller ones still needs its full brightness to show.
inline constexpr float kRainMaxOpacity = .4f;
inline float RainDropFall(float variation) { return (1 - std::exp(-1.1f * variation)) / (1 - std::exp(-1.1f)); }
// Expected coverage one occupied cell adds, as a world-space cross-section (units^2) seen broadside,
// before CL_particleOpacity. Type 1 rain, 2 snow, 3 ash.
inline float PrecipitationCrossSection(int type, float size, float streak) {
    // WeatherPS rain: saturate(kRainShapeGain * (1-|x|)^2 (1-|y|)) integrated over the [-1,1]^2 quad.
    static const float rainShape = [] {
        float s = 0;
        for (int a = 0; a < 64; ++a)
            for (int b = 0; b < 64; ++b) {
                float x = -1 + (a + .5f) / 32, y = -1 + (b + .5f) / 32;
                s += (std::min)(1.f, kRainShapeGain * (1 - std::abs(x)) * (1 - std::abs(x)) * (1 - std::abs(y)));
            }
        return s / (32.f * 32.f);
    }();
    constexpr int kSteps = 256;
    float sum = 0;
    for (int i = 0; i < kSteps; ++i) {
        float u = (i + .5f) / kSteps;
        if (type == 1) {
            float v = RainDropVariation(u), physical = size * .18f * v;
            float up = (std::max)(physical * 2, streak * size * RainDropFall(v));
            // Width x up half-extents, opacity scaled by physical / width and by the motion blur.
            sum += physical * up * rainShape * (std::min)(1.f, kRainMotionBlur * physical / up);
        } else if (type == 2) {
            float v = .35f + 1.55f * std::pow(u, 2.4f), r = size * v;
            sum += r * r * .7f; // soft clump with a bright core, flicker average (WeatherPS SNOW)
        } else {
            float v = .4f + 1.9f * std::pow(u, 1.8f), r = size * v;
            sum += r * r * .88f; // torn flake: area x mean translucency x mean flip width (WeatherPS ASH)
        }
    }
    return sum / kSteps;
}
// Ground splashes (EnvironmentSplashShaders.h): a lattice of impact cells in two nested bands, and
// per cell one spreading ring plus a crown of thrown droplets. The shader takes these as defines.
inline constexpr unsigned kSplashGrid = 48;
inline constexpr unsigned kSplashParts = 9; // 1 ring + 8 droplets
inline constexpr unsigned kSplashBands = 2;
inline constexpr float kSplashNear = 420.f, kSplashFar = 1300.f;
inline constexpr unsigned kSplashInstanceCount = kSplashBands*kSplashGrid*kSplashGrid*kSplashParts;
// Mirror of the shader's ray/sphere ordering, used by the smoke regressions.
inline float SmokeRayEntry(Vec3 camera,Vec3 direction,float extent,Vec3 center,float radius) {
    Vec3 offset=camera-center;float b=Dot(offset,direction),h=b*b-(Dot(offset,offset)-radius*radius);
    if(radius<=0||h<0)return -1;
    h=std::sqrt(h);if(-b+h<0)return -1;
    float t=(std::max)(0.f,-b-h);return t<=extent?t:-1;
}
// Integrate absorbance over the portion of the smoke traversed by this drop. In the
// dense interior even nearby drops are extinguished, while drops before entry stay clear.
inline float RainSmokeOpacity(float absorbance,float entry,float distance,bool inside) {
    if(absorbance<=0||entry<0||distance<=entry)return 0;
    float fraction=inside?std::clamp(distance/24.f,0.f,1.f):std::clamp((distance-entry)/80.f,0.f,1.f);
    return 1-std::exp(-absorbance*fraction);
}

inline Vec3 PrecipitationSourceDirection(float windX, float windY, float speed) {
    return Normalize({-windX, -windY, (std::max)(1.f, speed)});
}
// SETTLING precipitation (snow, ash) is sheltered by the open sky straight overhead, never by the
// wind: what lies on the ground is hours of fall from every gust. Sheltering it along the wind tilt
// (plus four gust tilts) cut open alleys into bare stripes -- one offset roof shadow per direction --
// that moved whenever the wind setting changed. Rain keeps the wind-driven shelter and gusts: a
// doorway facing the wind really is wet a short way in.
inline bool PrecipitationSettles(int type) { return type == 2 || type == 3; }
inline Vec3 PrecipitationShelterDirection(int type, float windX, float windY, float speed) {
    return PrecipitationSettles(type) ? Vec3{0, 0, 1} : PrecipitationSourceDirection(windX, windY, speed);
}
// GUSTS. Rain does not arrive along one exact line: gusts and turbulence spread its direction, most
// strongly along the wind. With a single shelter direction a doorway facing the wind stayed bone
// dry a unit inside the frame and arches sheltered like solid roofs. Four extra shelter maps look
// along directions tilted about the mean -- two along the wind, two across -- and a surface's
// exposure is the weighted share of directions that reach it, so rain reaches a short way into
// openings (further the stronger the wind) and deep interiors stay dry.
inline constexpr int kPrecipitationGusts = 4;
inline constexpr float kPrecipitationMeanWeight = .36f; // the rest split evenly over the gusts
inline float PrecipitationGustWeight() { return (1 - kPrecipitationMeanWeight) / kPrecipitationGusts; }
// Tilt angles (radians): along the wind 20 degrees, rising toward 38 as the wind slants the rain;
// across it 13 rising toward 22.
inline void PrecipitationGustAngles(float windX, float windY, float speed, float &along, float &across) {
    const float slant = std::atan2(std::sqrt(windX * windX + windY * windY), (std::max)(1.f, speed));
    const float t = std::clamp(slant / 0.7854f, 0.f, 1.f); // saturates at 45 degrees of mean slant
    along = (20 + 18 * t) * 0.0174533f;
    across = (13 + 9 * t) * 0.0174533f;
}
inline std::array<Vec3, kPrecipitationGusts> PrecipitationGustDirections(float windX, float windY, float speed) {
    const Vec3 s = PrecipitationSourceDirection(windX, windY, speed);
    // Horizontal wind axis (toward where the rain comes from) and its perpendicular; any axis when calm.
    Vec3 w{-windX, -windY, 0};
    const float wl = std::sqrt(w.x * w.x + w.y * w.y);
    w = wl > 1e-3f ? Vec3{w.x / wl, w.y / wl, 0} : Vec3{1, 0, 0};
    const Vec3 c{-w.y, w.x, 0};
    float along = 0, across = 0;
    PrecipitationGustAngles(windX, windY, speed, along, across);
    // Rotate s about the horizontal axis perpendicular to each tilt plane.
    auto tilt = [&](Vec3 toward, float angle) {
        const Vec3 t = Normalize(toward - s * Dot(toward, s)); // component of `toward` orthogonal to s
        const Vec3 d = s * std::cos(angle) + t * std::sin(angle);
        return Normalize({d.x, d.y, (std::max)(d.z, .05f)}); // never horizontal: rain still falls
    };
    return {tilt(w, along), tilt(w * -1, along), tilt(c, across), tilt(c * -1, across)};
}
// Fit fixed world bounds tightly in precipitation space, independent of the camera.
inline Matrix PrecipitationShadowMatrix(Vec3 low, Vec3 high, Vec3 sourceDirection) {
    Matrix view = ShadowView((low + high) * .5f, sourceDirection * -1);
    Vec3 minimum{1e9f, 1e9f, 1e9f}, maximum{-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < 8; ++i) {
        Vec3 p = Transform(view, {i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z});
        minimum = {(std::min)(minimum.x, p.x), (std::min)(minimum.y, p.y), (std::min)(minimum.z, p.z)};
        maximum = {(std::max)(maximum.x, p.x), (std::max)(maximum.y, p.y), (std::max)(maximum.z, p.z)};
    }
    minimum = minimum - Vec3{8, 8, 8};
    maximum = maximum + Vec3{8, 8, 8};
    Vec3 span = maximum - minimum;
    Matrix projection = {2 / span.x, 0,          0,          -(maximum.x + minimum.x) / span.x,
                         0,          2 / span.y, 0,          -(maximum.y + minimum.y) / span.y,
                         0,          0,          1 / span.z, -minimum.z / span.z,
                         0,          0,          0,          1};
    return Multiply(projection, view);
}
} // namespace Filmmaker::Environment
