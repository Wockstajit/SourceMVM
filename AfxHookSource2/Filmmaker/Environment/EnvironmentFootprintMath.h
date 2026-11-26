#pragma once
// FOOTPRINTS. Players leave marks where their feet go. The marks live in a square image fixed to
// the world around the camera (one texel is 0.75 units, so a boot is a few texels wide), addressed
// by wrapping, so it never moves with the camera; the edge that scrolls in is wiped.
//
// THE FEET ARE THE PLAYER'S OWN. Each frame the hook reads where both of a pawn's feet are (its
// ankle and toe bones) and how high each is off the ground. Deep snow is PRESSED by them: the image
// keeps, per texel, how far down the snow has been pushed, and a foot pushes it to its own height
// wherever it goes. A foot on the ground sinks a hole to the bed; a foot swinging forward low
// through the snow ploughs a shallow groove between two holes; a foot lifted clear leaves it alone.
// So the holes are where the feet were, the size of a boot, and two close together run into one.
// Thin snow shows a print where a foot came down. Falling snow fills everything back in from the
// bottom. Each foot that comes down is also kept for a second or two as ripples, and a splash, on
// wet ground.
//
// A BODY THAT FALLS presses the snow too. A dead player's bones are read the same way, and every
// part of the body (trunk, head, arms, legs) presses the snow down to its own underside wherever it
// is, as it drops and where it comes to lie: a hollow the shape of the body (PressBody).
//
// When a pawn's bones cannot be read its steps are worked out from how far it has moved along the
// ground instead: one every 34 units on alternate sides of its path, and both feet side by side
// where it sets off and where it stops.
#include "EnvironmentMath.h"
#include <cstdint>
#include <vector>

namespace Filmmaker::Environment {
inline constexpr int kFootSize = 2048;       // texels per side (a power of two): marks are kept 768 units from the camera each way
inline constexpr float kFootTexel = .75f;    // world units per texel
inline constexpr float kFootStride = 34.f;   // ground covered between one print and the next
inline constexpr float kFootSide = 6.5f;     // a foot's distance from the middle of the path
inline constexpr float kFootLength = 6.2f, kFootWidth = 2.7f; // a print's half extents
inline constexpr float kFootBootLength = 7.f, kFootBootWidth = 3.f; // half extents of the hole a boot presses in deep snow
inline constexpr float kFootWall = 5.f;      // how far the wall of a hole slopes out beyond the boot: a gentle dish, not a punched hole (two legs' furrows run together)
inline constexpr float kSnowDepth = 14.f;    // how deep the deepest snow lies, in units (about shin deep)
inline constexpr float kSandDepth = 9.f;     // how deep drifted sand lies, in units (about ankle deep)
inline constexpr float kSnowReliefMost = 1.35f; // the snow lies in mounds: up to this much of its depth (SnowRelief in the shader)
inline constexpr float kFootClear = kSnowDepth * kSnowReliefMost; // a sole lifted over the highest mound presses nothing
inline constexpr float kFootDown = 1.5f;     // a sole this near the ground has come down
inline constexpr float kFootAnkle = 3.2f, kFootToe = .3f; // how far above the sole the ankle and toe bones sit (measured standing)
inline constexpr float kFootLayer = 4.f;     // height is kept in steps of this, wrapping at 256
inline constexpr int kFootTile = 32;         // active simulation tile size in texels
inline constexpr int kFootTiles = kFootSize / kFootTile; // 64 x 64 active tiles across the image
inline constexpr float kFootBermScale = .35f; // max berm ridge height as a fraction of deep-pit scale (in signed height M = kFootBermScale*berm - press)
inline constexpr int kFootSteps = 32;        // recent steps kept for ripples and splashes
inline constexpr int kFootLive = 24;         // feet shown by the foot view: two a pawn (Frame::kMaxActors pawns)
// A fallen body, as the bones it is read by and the parts between them.
enum BodyBone { BB_pelvis, BB_spine, BB_chest, BB_head, BB_armL, BB_elbowL, BB_handL, BB_armR, BB_elbowR, BB_handR, BB_legL, BB_kneeL, BB_ankleL, BB_legR, BB_kneeR, BB_ankleR, kBodyBones };
inline constexpr const char *kBodyBoneNames[kBodyBones] = {"pelvis", "spine_1", "spine_3", "head_0", "arm_upper_l", "arm_lower_l", "hand_l", "arm_upper_r", "arm_lower_r", "hand_r",
                                                           "leg_upper_l", "leg_lower_l", "ankle_l", "leg_upper_r", "leg_lower_r", "ankle_r"};
struct BodyPart {
    int from, to;
    float radius; // how thick the body is around the line between the two bones
};
inline constexpr BodyPart kBodyParts[] = {{BB_pelvis, BB_spine, 6.5f}, {BB_spine, BB_chest, 7}, {BB_chest, BB_head, 5.5f}, {BB_head, BB_head, 5},
                                          {BB_chest, BB_armL, 5},      {BB_armL, BB_elbowL, 3.2f}, {BB_elbowL, BB_handL, 3}, {BB_chest, BB_armR, 5},
                                          {BB_armR, BB_elbowR, 3.2f},  {BB_elbowR, BB_handR, 3},   {BB_pelvis, BB_legL, 5.5f}, {BB_legL, BB_kneeL, 4.2f},
                                          {BB_kneeL, BB_ankleL, 3.4f}, {BB_pelvis, BB_legR, 5.5f}, {BB_legR, BB_kneeR, 4.2f}, {BB_kneeR, BB_ankleR, 3.4f}};
inline constexpr float kBodyRest = 3.f; // the lowest bone of a body, standing or lying, is about this far above the ground
struct FallenBones {
    Vec3 bone[kBodyBones]{};
    bool has[kBodyBones]{};
};
// The ground a body is on, from the body alone: a little under its lowest bone. (A dead pawn's own
// origin stays where it died, which may be a ledge away from where the body ends up.)
inline bool BodyGround(const FallenBones &body, float &ground) {
    bool any = false;
    for (int i = 0; i < kBodyBones; ++i)
        if (body.has[i]) {
            ground = any ? (std::min)(ground, body.bone[i].z - kBodyRest) : body.bone[i].z - kBodyRest;
            any = true;
        }
    return any;
}

struct FootTrack {
    Vec3 last{};
    float travelled = 0, heading = 0;
    bool left = false, valid = false;
    bool moving = false;  // it was walking at the last look
    bool planted = false; // both feet are already marked where it stands
};
struct FootStep {
    Vec3 position{};
    float heading = 0; // radians, direction of travel
};
inline constexpr float kFootRest = 8.f; // slower than this (units a second) is standing
// Moves a pawn's track to `origin`, `dt` seconds of the demo's clock after the last look. Returns
// how many feet came down (into `steps`): one for a stride, and two side by side where the pawn
// stands, when it sets off from a place it has not marked yet and when it comes to a stop. So a
// trail begins and ends with a pair of feet, and has single prints between. `grounded` says
// whether it was on the ground between the two positions (a jump, a fall or a teleport leaves no
// drag line).
inline int AdvanceFootTrack(FootTrack &track, Vec3 origin, float dt, FootStep steps[2], bool &grounded) {
    grounded = false;
    if (!track.valid) {
        track = {};
        track.last = origin;
        track.valid = true;
        return 0;
    }
    if (dt <= 0) return 0; // a paused demo: nobody moved
    const Vec3 before = track.last;
    const Vec3 move = origin - before;
    const float along = std::sqrt(move.x * move.x + move.y * move.y);
    track.last = origin;
    if (along > 96 || std::abs(move.z) > along * 1.5f + 3) {
        track.travelled = 0;
        track.moving = track.planted = false;
        return 0;
    }
    auto pair = [&](Vec3 at) {
        const float c = std::cos(track.heading), s = std::sin(track.heading);
        steps[0] = {{at.x - s * kFootSide, at.y + c * kFootSide, at.z}, track.heading};
        steps[1] = {{at.x + s * kFootSide, at.y - c * kFootSide, at.z}, track.heading};
        track.planted = true;
        track.travelled = 0;
        return 2;
    };
    if (along / dt < kFootRest) {
        if (!track.moving) return 0;
        track.moving = false;
        return pair(origin);
    }
    grounded = true;
    const float dx = move.x / along, dy = move.y / along;
    track.heading = std::atan2(dy, dx);
    if (!track.moving) {
        track.moving = true;
        if (!track.planted) return pair(before);
        track.travelled = 0;
    }
    track.travelled += along;
    if (track.travelled < kFootStride) return 0;
    track.travelled = std::fmod(track.travelled, kFootStride);
    track.planted = false;
    const float side = track.left ? kFootSide : -kFootSide;
    track.left = !track.left;
    steps[0] = {{origin.x - dy * side, origin.y + dx * side, origin.z}, track.heading};
    return 1;
}
// Where a foot's sole is against the ground its pawn stands on, from its ankle and toe bones (the
// lower of the two decides: a heel coming down, or a toe pushing off). About 0 standing, but player
// models differ by a unit or so in where those bones sit, so it is read against the foot's own
// lowest level (FootState::base).
// A toe bone dips a few units under the ground as the foot pushes off; that is a foot on the
// ground, not a lower standing level.
inline float FootSole(float ankleZ, float toeZ, float groundZ) { return (std::min)(ankleZ - kFootAnkle - groundZ, (std::max)(toeZ - kFootToe - groundZ, 0.f)); }
// How far down a sole at that height pushes the deepest snow, 0..1 (1 is to its bed). The leg
// above the foot goes through the snow too, so wherever a foot passes, the snow is cut down to the
// height of its sole: a slow walk, with the feet hardly lifted, drags a furrow nearly to the bed
// from one hole to the next; a run, with the feet lifted high, cuts only the deepest mounds.
inline float FootPress(float lift) { return std::clamp(1 - lift / kFootClear, 0.f, 1.f); }
// One foot of a pawn, from frame to frame.
inline constexpr float kFootBaseMost = 2.f, kFootBaseLeast = -.5f; // a model's bones are never further out than this
struct FootState {
    Vec3 last{};
    float lift = 1e9f;
    bool valid = false;
    float base = kFootBaseMost; // the lowest this foot's sole goes: where it stands
    bool planted = false;       // it is set down: low and not travelling
};
// How high the sole is above where this foot stands. The standing level is the lowest the sole has
// been, within what a model's bones can account for (a toe pointed down in a landing dips far
// lower and is not standing), and creeps back up three units a second so a dip does not hold it.
inline float FootLift(FootState &foot, float sole, float dt) {
    foot.base = std::clamp((std::min)(foot.base + 3 * dt, sole), kFootBaseLeast, kFootBaseMost);
    return (std::max)(sole - foot.base, 0.f);
}
// WHEN A FOOT IS SET DOWN. Low is not enough: a running foot drops to the ground while it is
// still flying forward, and comes to rest a boot's length further on some hundredths of a second
// later (measured with the foot view: the sole crossed 1.5 units at 380 units a second). A foot is
// set down when it is low AND has stopped travelling over the ground, against how fast its pawn
// moves; that is the frame and the place it is planted. It is taken up again when it lifts or
// travels. Returns whether it was set down by this look (never on the first look at a foot).
inline bool FootSetDown(FootState &foot, float lift, float footSpeed, float pawnSpeed) {
    const float still = (std::max)(40.f, .35f * pawnSpeed);
    if (!foot.planted && lift < kFootDown + 1.5f && footSpeed < still) {
        foot.planted = true;
        return foot.valid;
    }
    if (foot.planted && (lift > kFootDown + 2.5f || footSpeed > still * 2)) foot.planted = false;
    return false;
}
// HOW HARD A FOOT COMES DOWN, for the splash it throws and the ripples it rings (FootSplashVS takes
// how much water, how fast and how high from it): about 0.55 for a walk, 0.85 for a run, and a
// landing from a jump or a drop 1.5 and up with how fast the pawn was falling (a jump in place lands
// at about 300 units a second; 2.6 at 700 and over). Before 2026-11-18 every step threw a run's
// splash: a walk's read as too big and a landing's as too small.
inline float FootSmooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline float FootImpact(float pawnSpeed, float landingSpeed) {
    const float stride = .5f + .35f * FootSmooth(80, 250, pawnSpeed);
    const float landing = landingSpeed > 150 ? 1.5f + 1.1f * FootSmooth(300, 700, landingSpeed) : 0.f;
    return (std::max)(stride, landing);
}
// A pawn leaving the ground and coming back to it, from its height and its on-ground flag: how fast
// it was falling when it landed, kept for the steps of the landing (kFootLandWindow), and the frame
// it jumped (left the ground going up), when its feet kick the water up too.
inline constexpr float kFootLandWindow = .3f;
inline constexpr float kFootJumpRise = 120.f; // units a second upward that make leaving the ground a jump
struct FootLanding {
    float lastZ = 0;
    bool seen = false, airborne = false;
    float fall = 0;      // the fastest it fell while off the ground
    float landed = 0;    // how fast it was falling at its last landing
    float since = 1e9f;  // seconds since then
};
inline bool AdvanceFootLanding(FootLanding &l, float z, bool grounded, float dt) {
    if (!(dt > 0)) return false;
    const float vz = l.seen ? std::clamp((z - l.lastZ) / dt, -1500.f, 1500.f) : 0.f;
    const bool wasSeen = l.seen;
    l.lastZ = z;
    l.seen = true;
    l.since += dt;
    bool jumped = false;
    if (!grounded) {
        jumped = wasSeen && !l.airborne && vz > kFootJumpRise;
        l.airborne = true;
        l.fall = (std::max)(l.fall, -vz);
    } else if (l.airborne) {
        l.airborne = false;
        l.landed = l.fall;
        l.since = 0;
        l.fall = 0;
    }
    return jumped;
}
inline float FootLandingSpeed(const FootLanding &l) { return l.since <= kFootLandWindow ? l.landed : 0.f; }
// How ragged the edge of a hole in deep snow is at a texel, -0.11..0.11 of its width: broad smooth
// lumps, fixed to the world, so no two holes break the same way. (A crumble finer than the lumps,
// one value a texel, drew the edge as a jagged stair.)
inline float FootRough(int x, int y) {
    auto hash = [](int a, int b) {
        uint32_t h = uint32_t(a) * 73856093u ^ uint32_t(b) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return float(h & 0xffff) / 65535.f;
    };
    const int cell = 6; // texels a lump
    const int cx = int(std::floor(float(x) / cell)), cy = int(std::floor(float(y) / cell));
    float fx = float(x - cx * cell) / cell, fy = float(y - cy * cell) / cell;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const float lump = (hash(cx, cy) * (1 - fx) + hash(cx + 1, cy) * fx) * (1 - fy) + (hash(cx, cy + 1) * (1 - fx) + hash(cx + 1, cy + 1) * fx) * fy;
    return (lump - .5f) * .22f;
}
// Finer granular clump noise (0..1, ~2-texel scale) so displaced berms break into loose granules
// rather than reading as a smooth molded ring around the boot.
inline float FootGrainNoise(int x, int y) {
    auto hash = [](int a, int b) {
        uint32_t h = uint32_t(a) * 2654435761u ^ uint32_t(b) * 2246822519u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return float(h & 0xffff) / 65535.f;
    };
    const int cell = 2;
    const int cx = int(std::floor(float(x) / cell)), cy = int(std::floor(float(y) / cell));
    float fx = float(x - cx * cell) / cell, fy = float(y - cy * cell) / cell;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    return (hash(cx, cy) * (1 - fx) + hash(cx + 1, cy) * fx) * (1 - fy) + (hash(cx, cy + 1) * (1 - fx) + hash(cx + 1, cy + 1) * fx) * fy;
}
inline uint8_t FootLayer(float z) { return uint8_t(int(std::floor(z / kFootLayer)) & 255); }
// Granular physical simulation parameters for smooth continuous per-frame refill and slumping.
struct FootGranularParams {
    float printRate = 0.f;  // thin print fill rate (fraction/sec)
    float pressRate = 0.f;  // pressed pit fill rate (fraction/sec)
    float bermRate = 0.f;   // displaced berm decay rate (fraction/sec)
    float meltRate = 0.f;   // blast/fire crater refill rate (fraction/sec)
    float repose = 0.055f;  // max stable height difference per texel before grains avalanche
    float slumpRate = 0.f;  // lateral avalanche relaxation rate (1/sec)
    float windDirX = 0.f;   // normalized horizontal wind direction
    float windDirY = 0.f;
    float windBias = 0.f;   // downwind saltation asymmetry [0..0.9]
};
inline FootGranularParams MakeFootGranularParams(bool sandLies, bool snowing, bool sandWeather, float windX, float windY, float gusts) {
    FootGranularParams p;
    const float wLen = std::sqrt(windX * windX + windY * windY);
    if (wLen > 1e-3f) {
        p.windDirX = windX / wLen;
        p.windDirY = windY / wLen;
    }
    const float windFactor = std::clamp(wLen / 220.f, 0.f, 1.4f);
    const float gustFactor = std::clamp(gusts, 0.f, 1.f);
    if (sandLies) {
        // Dry granular sand: continuous ~22s refill in calm air down to ~8-9s in a sandstorm,
        // lower angle of repose (~33 deg), rapid slumping and wind-driven downwind saltation.
        const float pace = (4.f + 3.5f * windFactor + 2.5f * gustFactor + (sandWeather ? 2.f : 0.f)) / (255.f * 0.35f);
        p.printRate = pace;
        p.pressRate = pace * 0.55f;
        p.bermRate = pace * 1.15f;
        p.meltRate = ((std::min)(14.f, 4.f + 3.5f * windFactor + 2.5f * gustFactor + (sandWeather ? 2.f : 0.f) + 2.f)) / (255.f * 0.35f);
        p.repose = 0.048f;
        p.slumpRate = 2.4f + 1.8f * windFactor + 1.4f * gustFactor + (sandWeather ? 1.2f : 0.f);
        p.windBias = std::clamp(0.28f * windFactor + 0.32f * gustFactor + (sandWeather ? 0.25f : 0.f), 0.f, 0.85f);
    } else {
        // Cohesive snow: holds crisp edges at higher angle of repose (~50 deg), light creep in
        // wind, and fills continuously (~45s prints, ~90s deep holes) while snowing.
        const float snowFill = snowing ? (2.f / (255.f * 0.35f)) : 0.f;
        p.printRate = snowFill;
        p.pressRate = snowFill * 0.5f;
        p.bermRate = snowing ? (snowFill * 0.85f) : (0.12f * windFactor / 255.f);
        p.meltRate = (snowing ? 3.f : 2.f) / (255.f * 0.35f);
        p.repose = 0.088f;
        p.slumpRate = 0.35f + 0.55f * windFactor + (snowing ? 0.25f : 0.f);
        p.windBias = std::clamp(0.15f * windFactor + 0.2f * gustFactor, 0.f, 0.5f);
    }
    return p;
}
// Four 16-bit channels a texel (DXGI_FORMAT_R16G16B16A16_UNORM):
//   t[0] R: print depth (thin snow/sand), 0..65535
//   t[1] G: how far a fire or a blast has cleared it (Melt), 0..65535
//   t[2] B: (layer << 8) | berm8, packing height layer in upper byte and displaced berm in lower byte
//   t[3] A: how far down deep snow/sand has been pressed here, 0..65535
// A matching 16-bit `bermFine` array preserves smooth sub-byte berm dynamics across frames.
struct FootImage {
    std::vector<uint16_t> texels = std::vector<uint16_t>(size_t(kFootSize) * kFootSize * 4, 0);
    std::vector<uint16_t> bermFine = std::vector<uint16_t>(size_t(kFootSize) * kFootSize, 0);
    std::vector<uint8_t> activeTiles = std::vector<uint8_t>(size_t(kFootTiles) * kFootTiles, 0);
    double fillCarry[4] = {0.0, 0.0, 0.0, 0.0}; // fractional 16-bit refill accumulators: print, melt, press, berm
    int cellX = 0, cellY = 0; // the camera's texel, where the window was last centred
    bool centred = false;
    static int Wrap(int v) { return v & (kFootSize - 1); }
    static int WrapTile(int v) { return v & (kFootTiles - 1); }
    static size_t CellIdx(int x, int y) { return size_t(Wrap(y)) * kFootSize + Wrap(x); }
    uint16_t *At(int x, int y) { return &texels[CellIdx(x, y) * 4]; }
    const uint16_t *At(int x, int y) const { return &texels[CellIdx(x, y) * 4]; }
    static uint8_t ToByte8(uint16_t v) { return uint8_t((uint32_t(v) * 255u + 32767u) / 65535u); }
    uint8_t Print8(int x, int y) const { return ToByte8(At(x, y)[0]); }
    uint8_t Melt8(int x, int y) const { return ToByte8(At(x, y)[1]); }
    uint8_t Layer(int x, int y) const { return uint8_t(At(x, y)[2] >> 8); }
    uint8_t Berm8(int x, int y) const { return uint8_t(At(x, y)[2] & 0xffu); }
    uint8_t Press8(int x, int y) const { return ToByte8(At(x, y)[3]); }
    float PrintF(int x, int y) const { return At(x, y)[0] / 65535.f; }
    float PressF(int x, int y) const { return At(x, y)[3] / 65535.f; }
    float BermF(int x, int y) const { return bermFine[CellIdx(x, y)] / 65535.f; }
    static void PackBlue(uint16_t *t, uint8_t layer, uint16_t berm16) {
        t[2] = uint16_t((uint16_t(layer) << 8) | uint16_t(ToByte8(berm16)));
    }
    int ActiveTileCount() const {
        int n = 0;
        for (uint8_t a : activeTiles) n += (a != 0);
        return n;
    }
    void MarkActiveTile(int tx, int ty) {
        activeTiles[size_t(WrapTile(ty)) * kFootTiles + WrapTile(tx)] = 1;
    }
    void MarkActiveBox(int x0, int y0, int x1, int y1) {
        const int tx0 = int(std::floor(float(x0) / kFootTile)), ty0 = int(std::floor(float(y0) / kFootTile));
        const int tx1 = int(std::floor(float(x1) / kFootTile)), ty1 = int(std::floor(float(y1) / kFootTile));
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) MarkActiveTile(tx, ty);
    }
    // WHAT CHANGED SINCE IT WAS LAST SENT TO THE GPU. The image is 32 MB in 16-bit RGBA; a boot
    // changes a small box or active simulation tile run. Each change notes its rectangle here (split
    // where it wraps), and only those are uploaded. `all` is the whole image: a start, a wipe, or
    // too many pieces.
    struct Box { int x0, y0, x1, y1; }; // inclusive, inside the image
    std::vector<Box> changed;
    bool all = true;
    void Sent() { changed.clear(); all = false; }
    void Touch(int x0, int y0, int x1, int y1) { // any texel coordinates, x0 <= x1, y0 <= y1
        if (all) return;
        if (changed.size() > 256 || x1 - x0 >= kFootSize || y1 - y0 >= kFootSize) { all = true; changed.clear(); return; }
        int xs[2][2], ys[2][2], nx = 1, ny = 1;
        xs[0][0] = Wrap(x0); xs[0][1] = Wrap(x1);
        if (xs[0][0] > xs[0][1]) { xs[1][0] = 0; xs[1][1] = xs[0][1]; xs[0][1] = kFootSize - 1; nx = 2; }
        ys[0][0] = Wrap(y0); ys[0][1] = Wrap(y1);
        if (ys[0][0] > ys[0][1]) { ys[1][0] = 0; ys[1][1] = ys[0][1]; ys[0][1] = kFootSize - 1; ny = 2; }
        for (int a = 0; a < nx; ++a)
            for (int b = 0; b < ny; ++b) changed.push_back({xs[a][0], ys[b][0], xs[a][1], ys[b][1]});
    }
    void Clear() {
        std::fill(texels.begin(), texels.end(), uint16_t(0));
        std::fill(bermFine.begin(), bermFine.end(), uint16_t(0));
        std::fill(activeTiles.begin(), activeTiles.end(), uint8_t(0));
        std::fill(std::begin(fillCarry), std::end(fillCarry), 0.0);
        all = true;
        changed.clear();
    }
    // Follows the camera: texels that leave the window on one side come back on the other, wiped.
    // Returns whether anything was wiped.
    bool Centre(Vec3 camera) {
        const int x = int(std::floor(camera.x / kFootTexel)), y = int(std::floor(camera.y / kFootTexel));
        if (!centred || std::abs(x - cellX) >= kFootSize || std::abs(y - cellY) >= kFootSize) {
            const bool had = centred;
            Clear();
            cellX = x;
            cellY = y;
            centred = true;
            return had;
        }
        const int half = kFootSize / 2;
        bool wiped = false;
        for (int step = x > cellX ? 1 : -1; cellX != x; cellX += step, wiped = true) {
            const int column = step > 0 ? cellX + half : cellX - half;
            for (int row = 0; row < kFootSize; ++row) {
                const size_t idx = CellIdx(column, row);
                std::fill_n(&texels[idx * 4], 4, uint16_t(0));
                bermFine[idx] = 0;
            }
            Touch(column, 0, column, kFootSize - 1);
        }
        for (int step = y > cellY ? 1 : -1; cellY != y; cellY += step, wiped = true) {
            const int row = step > 0 ? cellY + half : cellY - half;
            for (int column = 0; column < kFootSize; ++column) {
                const size_t idx = CellIdx(column, row);
                std::fill_n(&texels[idx * 4], 4, uint16_t(0));
                bermFine[idx] = 0;
            }
            Touch(0, row, kFootSize - 1, row);
        }
        return wiped;
    }
    bool Inside(float x, float y, float margin) const {
        const float reach = (kFootSize / 2) * kFootTexel - margin;
        return std::abs(x - (cellX + .5f) * kFootTexel) < reach && std::abs(y - (cellY + .5f) * kFootTexel) < reach;
    }
    bool Press(Vec3 at, float heading, float press, bool print, float bermStrength = 1.f) {
        return Stamp(at, heading, kFootBootLength, kFootBootWidth, press, print ? kFootLength : 0, kFootWidth, bermStrength);
    }
    bool PressRound(Vec3 at, float radius, float press, float bermStrength = 1.f) {
        return Stamp(at, 0, radius, radius, press, press > .85f ? radius * .9f : 0, radius * .9f, bermStrength);
    }
    bool PressBody(const FallenBones &body, float ground, float bermStrength = 1.f) {
        bool changed = false;
        for (const BodyPart &part : kBodyParts) {
            if (!body.has[part.from] || !body.has[part.to]) continue;
            const Vec3 a = body.bone[part.from], d = body.bone[part.to] - a;
            const int count = (std::max)(1, int(std::ceil(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / 2.f)));
            for (int i = 0; i <= count; ++i) {
                const float t = float(i) / count;
                const Vec3 at{a.x + d.x * t, a.y + d.y * t, a.z + d.z * t};
                const float press = FootPress((std::max)(at.z - part.radius - ground, 0.f));
                if (press > .02f) changed |= PressRound({at.x, at.y, ground}, part.radius, press, bermStrength);
            }
        }
        return changed;
    }
    bool Melt(Vec3 at, float radius, float amount, bool add) {
        if (!Inside(at.x, at.y, radius + 2) || amount <= 0) return false;
        bool changed = false;
        const int reach = int(std::ceil(radius / kFootTexel));
        const int cx = int(std::floor(at.x / kFootTexel)), cy = int(std::floor(at.y / kFootTexel));
        const uint8_t layer = FootLayer(at.z);
        for (int y = cy - reach; y <= cy + reach; ++y)
            for (int x = cx - reach; x <= cx + reach; ++x) {
                const float dx = (x + .5f) * kFootTexel - at.x, dy = (y + .5f) * kFootTexel - at.y;
                float w = std::clamp((1 - std::sqrt(dx * dx + dy * dy) / radius) * 1.6f, 0.f, 1.f);
                w = w * w * (3 - 2 * w);
                if (w <= 0) continue;
                const size_t idx = CellIdx(x, y);
                uint16_t *t = &texels[idx * 4];
                const int most = int(w * 65535.f + .5f);
                const int step16 = (std::max)(1, int(amount * 65535.f + .5f));
                const int now = add ? (std::max)(int(t[1]), (std::min)(most, int(t[1]) + step16))
                                    : (std::max)(int(t[1]), int(amount * most));
                if (now == t[1]) continue;
                t[1] = uint16_t(now);
                if (!t[0] && !t[3] && !bermFine[idx]) PackBlue(t, layer, 0);
                changed = true;
            }
        if (changed) {
            Touch(cx - reach, cy - reach, cx + reach, cy + reach);
            MarkActiveBox(cx - reach, cy - reach, cx + reach, cy + reach);
        }
        return changed;
    }
    // An oval with half extents `length` along `heading` and `width` across it; the print in thin
    // snow/sand is the smaller oval `printLength` by `printWidth`. Around the depressed oval, displaced
    // material builds a granular berm (bermFine) that collapses if stepped over later.
    bool Stamp(Vec3 at, float heading, float length, float width, float press, float printLength, float printWidth, float bermStrength = 1.f) {
        const float bermReach = kFootWall + 2.5f;
        const float around = (std::max)(length, width) + bermReach;
        if (!Inside(at.x, at.y, around + 2)) return false;
        bool changed = false;
        const float c = std::cos(heading), s = std::sin(heading);
        const int reach = int(std::ceil((around + 1) / kFootTexel));
        const int cx = int(std::floor(at.x / kFootTexel)), cy = int(std::floor(at.y / kFootTexel));
        const uint8_t layer = FootLayer(at.z);
        for (int y = cy - reach; y <= cy + reach; ++y)
            for (int x = cx - reach; x <= cx + reach; ++x) {
                const float dx = (x + .5f) * kFootTexel - at.x, dy = (y + .5f) * kFootTexel - at.y;
                const float along = dx * c + dy * s, across = -dx * s + dy * c;
                float depth = 0;
                if (printLength > 0) {
                    const float u = along / printLength, v = across / printWidth;
                    depth = std::clamp((1 - (u * u + v * v)) * 2.2f, 0.f, 1.f);
                }
                const float hu = along / length, hv = across / width;
                const float r = std::sqrt(hu * hu + hv * hv);
                const float beyond = (r - 1) * (r > 1e-4f ? std::sqrt(along * along + across * across) / r : width);
                float wall = std::clamp(1 - beyond / kFootWall + FootRough(x, y), 0.f, 1.f);
                wall = wall * wall * (3 - 2 * wall);
                const float pressed = press * wall;

                float berm = 0.f;
                if (bermStrength > 0.f && beyond > 0.1f && beyond < bermReach) {
                    // Peak ridge just past the boot lip (~2.0u out), biased slightly ahead along heading.
                    const float normB = (beyond - 0.1f) / (bermReach - 0.1f);
                    const float hump = std::sin(std::clamp(normB, 0.f, 1.f) * 3.14159265f);
                    const float plowBias = 0.85f + 0.30f * std::clamp(along / (std::max)(length, 1.f), -1.f, 1.f);
                    const float grit = 0.65f + 0.70f * FootGrainNoise(x, y);
                    berm = std::clamp(press * bermStrength * hump * hump * plowBias * grit * (1.f - wall * 0.75f), 0.f, 1.f);
                }

                if (depth <= 0 && pressed <= 0 && berm <= 0) continue;
                const size_t idx = CellIdx(x, y);
                uint16_t *t = &texels[idx * 4];
                const uint16_t d = uint16_t(std::clamp(depth, 0.f, 1.f) * 65535.f);
                const uint16_t h = uint16_t(std::clamp(pressed, 0.f, 1.f) * 65535.f + .5f);
                const uint16_t b = uint16_t(std::clamp(berm, 0.f, 1.f) * 65535.f + .5f);
                const uint8_t curLayer = uint8_t(t[2] >> 8);
                uint16_t curBerm = bermFine[idx];
                const bool hasMark = (t[0] || t[3] || curBerm);
                const int apart = std::abs(int(int8_t(uint8_t(layer - curLayer))));

                auto bermCap = [](uint16_t pv) -> uint16_t {
                    if (pv >= 6553u) return 0; // >= 10% pressed pit crushes displaced ridge flat
                    return uint16_t((uint32_t(6553u - pv) * 65535u) / 6553u);
                };

                if (hasMark && apart * kFootLayer > 7) {
                    if (h <= t[3] && !(h == t[3] && d > t[0])) continue;
                    t[0] = d;
                    t[3] = h;
                    const uint16_t crushed = (std::min)(b, bermCap(h));
                    bermFine[idx] = crushed;
                    PackBlue(t, layer, crushed);
                    changed = true;
                    continue;
                }

                bool rowChanged = false;
                uint8_t nextLayer = hasMark ? curLayer : layer;
                if (d > t[0]) { t[0] = d; nextLayer = layer; rowChanged = true; }
                if (h > t[3]) {
                    t[3] = h;
                    nextLayer = layer;
                    const uint16_t cap = bermCap(t[3]);
                    if (curBerm > cap) curBerm = cap;
                    rowChanged = true;
                }
                const uint16_t bAllowed = (std::min)(b, bermCap(t[3]));
                if (bAllowed > curBerm) {
                    curBerm = bAllowed;
                    if (!t[0] && !t[3]) nextLayer = layer;
                    rowChanged = true;
                }
                if (!rowChanged) continue;
                bermFine[idx] = curBerm;
                PackBlue(t, nextLayer, curBerm);
                changed = true;
            }
        if (changed) {
            Touch(cx - reach, cy - reach, cx + reach, cy + reach);
            MarkActiveBox(cx - reach, cy - reach, cx + reach, cy + reach);
        }
        return changed;
    }
    // A foot moving from a to b, pressing `from` of the way down at a and `to` at b.
    bool Sweep(Vec3 a, Vec3 b, float heading, float from, float to, float bermStrength = 1.f) {
        bool changed = false;
        const float dx = b.x - a.x, dy = b.y - a.y;
        const int count = (std::max)(1, int(std::ceil(std::sqrt(dx * dx + dy * dy) / 1.5f)));
        for (int i = 1; i <= count; ++i) {
            const float t = float(i) / count, press = from + (to - from) * t;
            if (press > .02f) changed |= Press({a.x + dx * t, a.y + dy * t, a.z + (b.z - a.z) * t}, heading, press, false, bermStrength);
        }
        return changed;
    }
    void Print(const FootStep &step, float bermStrength = 1.f) { Press(step.position, step.heading, 1, true, bermStrength); }
    // Continuous per-frame granular physics & smooth refill across active tiles:
    //   1. Angle-of-repose slumping + wind saltation exchanges mass between steep neighbors
    //   2. Smooth 16-bit fractional-carry refill restores prints, pits, berms, and craters smoothly
    bool StepGranular(float dt, const FootGranularParams &p) {
        dt = std::clamp(dt, 0.f, .1f);
        if (dt <= 0.f) return false;
        const float windStrength = std::clamp(p.windBias, 0.f, .9f);
        const float slumpC = (p.slumpRate > 0.f) ? (std::min)(.14f, p.slumpRate * dt) : 0.f;
        const bool doSlump = slumpC > 1e-6f;

        auto takeCarry = [&](int slot, float rate) -> uint16_t {
            if (rate <= 0.f) return 0;
            fillCarry[slot] += double(rate) * double(dt) * 65535.0;
            const int step = int(std::floor(fillCarry[slot]));
            if (step <= 0) return 0;
            fillCarry[slot] -= double(step);
            return uint16_t((std::min)(step, 65535));
        };
        const uint16_t dPrint = takeCarry(0, p.printRate);
        const uint16_t dMelt = takeCarry(1, p.meltRate);
        const uint16_t dPress = takeCarry(2, p.pressRate);
        const uint16_t dBerm = takeCarry(3, p.bermRate);

        auto cellH = [&](size_t idx) -> float {
            return kFootBermScale * (bermFine[idx] / 65535.f) - (texels[idx * 4 + 3] / 65535.f);
        };
        auto setH = [&](size_t idx, float m, uint8_t layer) {
            m = std::clamp(m, -1.f, kFootBermScale);
            if (m >= 0.f) {
                texels[idx * 4 + 3] = 0;
                bermFine[idx] = uint16_t(std::lround(std::clamp(m / kFootBermScale, 0.f, 1.f) * 65535.f));
            } else {
                texels[idx * 4 + 3] = uint16_t(std::lround(std::clamp(-m, 0.f, 1.f) * 65535.f));
                bermFine[idx] = 0;
            }
            PackBlue(&texels[idx * 4], layer, bermFine[idx]);
        };

        std::vector<uint8_t> dirtyTiles(size_t(kFootTiles) * kFootTiles, 0);
        bool anyDirty = false;
        std::vector<std::pair<int, int>> activeList;
        activeList.reserve(128);
        for (int ty = 0; ty < kFootTiles; ++ty)
            for (int tx = 0; tx < kFootTiles; ++tx)
                if (activeTiles[size_t(ty) * kFootTiles + tx]) activeList.push_back({tx, ty});

        if (doSlump && !activeList.empty()) {
            const int ox[4] = {1, -1, 0, 0};
            const int oy[4] = {0, 0, 1, -1};
            for (int parity = 0; parity < 2; ++parity) {
                for (const auto &[tx, ty] : activeList) {
                    const int x0 = tx * kFootTile, y0 = ty * kFootTile;
                    for (int ly = 0; ly < kFootTile; ++ly) {
                        const int y = y0 + ly;
                        for (int lx = (ly & 1) ^ parity; lx < kFootTile; lx += 2) {
                            const int x = x0 + lx;
                            const size_t iA = size_t(y) * kFootSize + x;
                            uint16_t *tA = &texels[iA * 4];
                            uint16_t bA = bermFine[iA];
                            const bool hasA = (tA[3] > 0 || bA > 0);
                            const uint8_t lA = uint8_t(tA[2] >> 8);
                            for (int d = 0; d < 4; ++d) {
                                const int nx = Wrap(x + ox[d]), ny = Wrap(y + oy[d]);
                                const size_t iB = size_t(ny) * kFootSize + nx;
                                uint16_t *tB = &texels[iB * 4];
                                uint16_t bB = bermFine[iB];
                                if (!hasA && tB[3] == 0 && bB == 0) continue;
                                const uint8_t lB = uint8_t(tB[2] >> 8);
                                const bool hasAnyA = (tA[0] || tA[3] || bA);
                                const bool hasAnyB = (tB[0] || tB[3] || bB);
                                if (hasAnyA && hasAnyB && std::abs(int(int8_t(uint8_t(lA - lB)))) * kFootLayer > 7) continue;

                                const float hA = cellH(iA), hB = cellH(iB);
                                const float diff = hA - hB;
                                const float absDiff = std::abs(diff);
                                if (absDiff <= p.repose) continue;

                                // Grains avalanche from higher cell -> lower cell. Wind boosts transfer
                                // traveling downwind from a high edge into a pit or leeward slope.
                                const float dirSign = diff > 0.f ? 1.f : -1.f;
                                const float alignedWind = (float(ox[d]) * p.windDirX + float(oy[d]) * p.windDirY) * dirSign;
                                const float windMult = (std::max)(0.25f, 1.f + windStrength * alignedWind);
                                const float flow = dirSign * slumpC * (absDiff - p.repose) * windMult * 0.5f;

                                const uint8_t sharedLayer = (hasAnyA && hasAnyB) ? (abs(hB) > abs(hA) ? lB : lA) : (hasAnyA ? lA : lB);
                                setH(iA, hA - flow, sharedLayer);
                                setH(iB, hB + flow, sharedLayer);
                                bA = bermFine[iA];

                                dirtyTiles[size_t(ty) * kFootTiles + tx] = 1;
                                const int ntx = nx / kFootTile, nty = ny / kFootTile;
                                dirtyTiles[size_t(nty) * kFootTiles + ntx] = 1;
                                activeTiles[size_t(nty) * kFootTiles + ntx] = 1;
                                anyDirty = true;
                            }
                        }
                    }
                }
            }
        }

        // Smooth decay + tile retirement sweep over all currently active tiles (including spilled neighbors).
        for (int ty = 0; ty < kFootTiles; ++ty) {
            for (int tx = 0; tx < kFootTiles; ++tx) {
                const size_t tid = size_t(ty) * kFootTiles + tx;
                if (!activeTiles[tid]) continue;
                bool tileHasWork = false;
                bool tileChanged = dirtyTiles[tid] != 0;
                const int x0 = tx * kFootTile, y0 = ty * kFootTile;
                for (int ly = 0; ly < kFootTile; ++ly) {
                    const int y = y0 + ly;
                    uint16_t *row = &texels[(size_t(y) * kFootSize + x0) * 4];
                    uint16_t *bRow = &bermFine[size_t(y) * kFootSize + x0];
                    for (int lx = 0; lx < kFootTile; ++lx, row += 4, ++bRow) {
                        const uint16_t r0 = row[0], r1 = row[1], r3 = row[3], b0 = *bRow;
                        if (!(r0 | r1 | r3 | b0)) continue;
                        if (dPrint && row[0]) { row[0] = uint16_t(row[0] > dPrint ? row[0] - dPrint : 0); tileChanged = true; }
                        if (dMelt && row[1]) { row[1] = uint16_t(row[1] > dMelt ? row[1] - dMelt : 0); tileChanged = true; }
                        if (dPress && row[3]) { row[3] = uint16_t(row[3] > dPress ? row[3] - dPress : 0); tileChanged = true; }
                        if (dBerm && *bRow) {
                            *bRow = uint16_t(*bRow > dBerm ? *bRow - dBerm : 0);
                            PackBlue(row, uint8_t(row[2] >> 8), *bRow);
                            tileChanged = true;
                        }
                        if (row[0] | row[1] | row[3] | *bRow) {
                            tileHasWork = true;
                        } else {
                            row[2] = 0; // clean layer when completely refilled
                            tileChanged = true;
                        }
                    }
                }
                if (!tileHasWork) activeTiles[tid] = 0;
                if (tileChanged) {
                    dirtyTiles[tid] = 1;
                    anyDirty = true;
                }
            }
        }

        if (anyDirty) {
            for (int ty = 0; ty < kFootTiles; ++ty) {
                int runStart = -1;
                for (int tx = 0; tx <= kFootTiles; ++tx) {
                    const bool dirty = tx < kFootTiles && dirtyTiles[size_t(ty) * kFootTiles + tx] != 0;
                    if (dirty && runStart < 0) runStart = tx;
                    else if (!dirty && runStart >= 0) {
                        Touch(runStart * kFootTile, ty * kFootTile, tx * kFootTile - 1, (ty + 1) * kFootTile - 1);
                        runStart = -1;
                    }
                }
            }
        }
        return anyDirty;
    }
    void Fill(uint8_t amount) { FillRows(amount, 0, kFootSize); }
    void FillRows(uint8_t amount, int first, int count, uint8_t back = 0) {
        const uint16_t a16 = uint16_t(amount) * 257u;
        const uint16_t b16 = uint16_t(back) * 257u;
        const uint16_t slow = amount ? uint16_t((std::max)(1, int(amount) / 2) * 257) : uint16_t(0);
        count = (std::min)(count, kFootSize);
        for (int r = 0; r < count; ++r) {
            const int wy = Wrap(first + r);
            uint16_t *row = At(0, wy);
            uint16_t *bRow = &bermFine[size_t(wy) * kFootSize];
            for (int x = 0; x < kFootSize; ++x, row += 4, ++bRow) {
                row[0] = uint16_t(row[0] > a16 ? row[0] - a16 : 0);
                row[1] = uint16_t(row[1] > b16 ? row[1] - b16 : 0);
                row[3] = uint16_t(row[3] > slow ? row[3] - slow : 0);
                *bRow = uint16_t(*bRow > a16 ? *bRow - a16 : 0);
                if (row[0] | row[1] | row[3] | *bRow) PackBlue(row, uint8_t(row[2] >> 8), *bRow);
                else row[2] = 0;
            }
        }
        if (count >= kFootSize) { all = true; changed.clear(); }
        else if (count > 0) Touch(0, first, kFootSize - 1, first + count - 1);
    }
};
// How fast footprints and blast craters fill back in on each 0.35 s sweep of the image.
// Falling snow fills a full print in ~45 s (mark = 2). Dry sand slumps and blows back in faster:
// ~22 s in calm air, ~11-15 s in a steady breeze or gusts, ~8-9 s in a sandstorm.
struct FootFillRate {
    uint8_t mark = 0; // subtracted from prints (row[0]); deep holes (row[3]) take half
    uint8_t back = 2; // subtracted from cleared melt/blast layer (row[1])
};
inline FootFillRate FootFillStep(bool sandLies, bool snowing, bool sandWeather, float windSpeed, float gusts) {
    if (sandLies) {
        const float windFactor = std::clamp(windSpeed / 220.f, 0.f, 1.4f);
        const float gustFactor = std::clamp(gusts, 0.f, 1.f);
        const float pace = 4.f + 3.5f * windFactor + 2.5f * gustFactor + (sandWeather ? 2.f : 0.f);
        const uint8_t mark = uint8_t(std::clamp(int(std::lround(pace)), 4, 12));
        return {mark, uint8_t((std::min)(14, int(mark) + 2))};
    }
    return {uint8_t(snowing ? 2 : 0), uint8_t(snowing ? 3 : 2)};
}
} // namespace Filmmaker::Environment
