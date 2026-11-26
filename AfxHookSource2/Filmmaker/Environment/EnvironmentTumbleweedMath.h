#pragma once
// TUMBLEWEEDS: wind-blown dry brush (Russian thistle, Assets/environment/tumbleweed/tumbleweed.glb)
// that rolls, skips and bounces across the map's collision geometry.
//
// OFF-CAMERA SPAWNING. A tumbleweed never pops into existence in front of the camera: candidates are
// chosen upwind on the map's walkable nav mesh (or map bounds fallback) and accepted only when
// outside the camera frustum or solidly occluded behind map geometry, so they arrive naturally from
// around corners and alleys. Recycling happens only out of view as well.
//
// Engine-free and header-only so Automation/tests/tumbleweed-math-tests.cpp can verify frustum
// rejection, wall/ground interaction, no-slip rolling and seek resets without CS2.
#include "EnvironmentMath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace Filmmaker::Environment {

inline constexpr int kTumbleweedMax = 16;
inline constexpr float kTumbleweedMinRadius = 13.f;
inline constexpr float kTumbleweedMaxRadius = 21.f;
inline constexpr float kTumbleweedGravity = 720.f;
inline constexpr float kTumbleweedMinSpawnDist = 340.f;
inline constexpr float kTumbleweedMaxSpawnDist = 2300.f;
// THE COUNT IS THE MAP'S, NOT THE CAMERA'S. However many the slider asks for roll somewhere on the
// map; only a few are ever near the camera at once, and they come one at a time. (With every one of
// them spawned round the camera, ten tumbleweeds crowded every shot, the owner, 2026-11-23.)
inline constexpr float kTumbleweedNearReach = 1700.f; // "near the camera", across the ground
inline constexpr int kTumbleweedNearMost = 2;          // at most this many near the camera at once
// Seconds between one tumbleweed arriving near the camera and the next: further apart in first
// person, where the player is looking at one place, than for a cinematic camera.
inline constexpr float kTumbleweedPovGapLeast = 7.f, kTumbleweedPovGapMost = 15.f;
inline constexpr float kTumbleweedCineGapLeast = 4.f, kTumbleweedCineGapMost = 10.f;
// The first arrival after tumbleweeds are switched on (or after a seek) comes this soon.
inline constexpr float kTumbleweedFirstLeast = 1.5f, kTumbleweedFirstMost = 3.5f;

struct TumbleweedQuat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline TumbleweedQuat TumbleweedQuatNormalize(TumbleweedQuat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l > 1e-8f) return {q.x / l, q.y / l, q.z / l, q.w / l};
    return {0, 0, 0, 1};
}

inline TumbleweedQuat TumbleweedQuatMul(TumbleweedQuat a, TumbleweedQuat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline TumbleweedQuat TumbleweedQuatAxisAngle(Vec3 axis, float radians) {
    const float l = std::sqrt(Dot(axis, axis));
    if (l < 1e-7f || std::abs(radians) < 1e-7f) return {0, 0, 0, 1};
    const float h = radians * .5f;
    const float s = std::sin(h) / l;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}

// Row-major model matrix for a tumbleweed at `pos`, rotated by unit quaternion `q` and scaled by `radius`.
inline Matrix TumbleweedModelMatrix(Vec3 pos, TumbleweedQuat q, float radius) {
    q = TumbleweedQuatNormalize(q);
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    return {
        (1 - 2 * (yy + zz)) * radius, 2 * (xy - wz) * radius,       2 * (xz + wy) * radius,       pos.x,
        2 * (xy + wz) * radius,       (1 - 2 * (xx + zz)) * radius, 2 * (yz - wx) * radius,       pos.y,
        2 * (xz - wy) * radius,       2 * (yz + wx) * radius,       (1 - 2 * (xx + yy)) * radius, pos.z,
        0,                            0,                            0,                            1,
    };
}

struct TumbleweedRayHit {
    Vec3 position{}, normal{};
    float distance = 0;
};

using TumbleweedRaycast = std::function<bool(Vec3 origin, Vec3 direction, float range, TumbleweedRayHit &hit)>;

struct TumbleweedCamera {
    Vec3 position{};
    Matrix worldToClip{};
    bool valid = false;
    // First person: the game's own view from a player's eyes (playing, or spectating one). New
    // tumbleweeds then come out in front of the player. Otherwise (a free, path or attached camera)
    // they may come from any side.
    bool pov = false;
};
// Which way the camera looks across the ground (unit, z 0): the clip w row is the view axis.
inline Vec3 TumbleweedCameraForward(const TumbleweedCamera &cam) {
    const float x = cam.worldToClip[12], y = cam.worldToClip[13];
    const float l = std::sqrt(x * x + y * y);
    return l > 1e-6f ? Vec3{x / l, y / l, 0.f} : Vec3{1, 0, 0};
}

struct TumbleweedActor {
    Vec3 origin{};
    float radius = 18.f;
};

struct TumbleweedWind {
    float windX = 0, windY = 0;
    float gusts = 0;
};

struct TumbleweedBody {
    bool active = false;
    Vec3 pos{}, vel{}, omega{};
    Vec3 flowDir{}; // smoothed local airflow direction (channels down streets and around walls)
    TumbleweedQuat orientation{};
    float radius = 16.f;
    float age = 0.f, maxAge = 40.f, stuckTime = 0.f, hopCooldown = 0.f;
    uint32_t seed = 0;
    bool grounded = false;
};

struct TumbleweedSim {
    std::array<TumbleweedBody, kTumbleweedMax> bodies{};
    float nearTimer = 0.f; // until the next arrival near the camera may come
    float farTimer = 0.f;  // until the next one out on the map may be placed
    uint32_t rng = 0x51a7c93du;
    double lastTime = 0.0;
    bool initialized = false;
};

inline uint32_t TumbleweedNextRand(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    if (!state) state = 0x9e3779b9u;
    return state;
}

inline float TumbleweedRand01(uint32_t &state) {
    return float(TumbleweedNextRand(state) & 0xffffffu) / 16777216.f;
}

// Whether a sphere at `pos` with `radius` projects inside (or near the edge of) the camera frustum.
inline bool TumbleweedInFrustum(const TumbleweedCamera &cam, Vec3 pos, float radius) {
    if (!cam.valid) return false;
    const auto &m = cam.worldToClip;
    const float cx = m[0] * pos.x + m[1] * pos.y + m[2] * pos.z + m[3];
    const float cy = m[4] * pos.x + m[5] * pos.y + m[6] * pos.z + m[7];
    const float cw = m[12] * pos.x + m[13] * pos.y + m[14] * pos.z + m[15];
    if (cw <= 1.f) return false; // behind the camera
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    // Generous screen-space margin so outer twigs never spawn partially inside the frame.
    const float pad = (std::min)(1.2f, radius * 2.2f / cw) + .16f;
    return std::abs(ndcX) <= 1.f + pad && std::abs(ndcY) <= 1.f + pad;
}

// True when `pos` could be seen by the camera (inside frustum and unoccluded, or too close).
inline bool TumbleweedVisibleToCamera(const TumbleweedCamera &cam, Vec3 pos, float radius,
                                      const TumbleweedRaycast &raycast) {
    if (!cam.valid) return false;
    const Vec3 delta = pos - cam.position;
    const float dist = std::sqrt(Dot(delta, delta));
    if (dist < kTumbleweedMinSpawnDist) return true;
    if (!TumbleweedInFrustum(cam, pos, radius)) return false;
    if (!raycast) return true; // without collision, never risk spawning inside frustum
    // Check both centre and top of the bush: both must be hidden behind solid map geometry.
    const Vec3 targets[2] = {pos, pos + Vec3{0, 0, radius * .85f}};
    for (const Vec3 &target : targets) {
        const Vec3 to = target - cam.position;
        const float d = std::sqrt(Dot(to, to));
        if (d < 1.f) return true;
        TumbleweedRayHit hit;
        if (!raycast(cam.position, to * (1.f / d), d - radius * 1.6f, hit))
            return true; // unobstructed line of sight to this part of the tumbleweed
    }
    return false;
}

// Effective steady wind vector for tumbleweeds: follows weather wind, with a gentle desert breeze
// floor so tumbleweeds still slowly roll when wind speed is left near zero.
inline Vec3 TumbleweedBaseWind(const TumbleweedWind &w) {
    const float speed = std::sqrt(w.windX * w.windX + w.windY * w.windY);
    if (speed >= 55.f) return {w.windX, w.windY, 0.f};
    if (speed > 1.f) {
        const float s = 55.f / speed;
        return {w.windX * s, w.windY * s, 0.f};
    }
    // Default gentle breeze (east-northeast) when wind is completely zeroed.
    return {60.f, 18.f, 0.f};
}

// Spatial + temporal gust wave so tumbleweeds surge, lull and meander naturally.
inline Vec3 TumbleweedWindAt(Vec3 pos, double time, const TumbleweedWind &w, uint32_t seed) {
    const Vec3 base = TumbleweedBaseWind(w);
    const float speed = std::sqrt(base.x * base.x + base.y * base.y);
    const Vec3 along{base.x / speed, base.y / speed, 0.f};
    const Vec3 across{-along.y, along.x, 0.f};
    const float p1 = float(seed & 1023u) * .0061359f;
    const float p2 = float((seed >> 10) & 1023u) * .0061359f;
    const float alongCoord = pos.x * along.x + pos.y * along.y;
    const float acrossCoord = pos.x * across.x + pos.y * across.y;
    const float waveA = std::sin(alongCoord * .0045f - float(time) * 1.35f + p1);
    const float waveB = std::cos(acrossCoord * .0038f + float(time) * .78f + p2);
    const float gustMul = 1.f + (.25f + .65f * std::clamp(w.gusts, 0.f, 1.f)) * (.5f * waveA + .5f * waveA * waveB);
    const float crossSway = speed * (.16f + .22f * std::clamp(w.gusts, 0.f, 1.f)) *
                            std::sin(alongCoord * .0031f + float(time) * .95f + p2);
    return along * (speed * (std::max)(.28f, gustMul)) + across * crossSway;
}

// Whether a tumbleweed set down at `pos` will roll into the camera's sight: the points the steady
// wind carries it to, up to 800 units on, as far as the first wall in its way. `along` is how far it
// rolls before it is seen.
inline bool TumbleweedComesIntoView(const TumbleweedCamera &cam, Vec3 pos, float radius, Vec3 windDir,
                                    const TumbleweedRaycast &raycast, float &along) {
    static const float kAlong[6] = {120.f, 240.f, 360.f, 480.f, 640.f, 800.f};
    float open = kAlong[5];
    if (raycast) {
        TumbleweedRayHit wall;
        if (raycast(pos + Vec3{0, 0, radius * .15f}, windDir, kAlong[5], wall) && wall.normal.z < .55f)
            open = wall.distance - radius;
    }
    for (float t : kAlong) {
        if (t > open) break;
        if (TumbleweedVisibleToCamera(cam, pos + windDir * t, radius, raycast)) {
            along = t;
            return true;
        }
    }
    return false;
}

// Where a new tumbleweed goes. Near: one arriving near the camera, set down out of its sight so it
// rolls in. In first person it starts in front of the player (within about 70 degrees of where they
// look) and is chosen so the wind carries it out into view; for any other camera it comes from
// upwind, from whichever side. Far: one of the rest, out on the map well away from the camera.
// Every spot is on walkable ground, hidden from the camera and clear of the others. Returns false if
// no such spot is found this time (the caller tries again later rather than popping one into view).
enum class TumbleweedZone { Near, Far };
inline bool TumbleweedPickSpawn(const std::vector<Vec3> &navPositions, Vec3 mapLow, Vec3 mapHigh,
                                const TumbleweedCamera &cam, const TumbleweedWind &wind,
                                const TumbleweedRaycast &raycast,
                                const std::array<TumbleweedBody, kTumbleweedMax> &existing,
                                uint32_t &rng, float radius, Vec3 &outPos,
                                TumbleweedZone zone = TumbleweedZone::Near) {
    const Vec3 baseWind = TumbleweedBaseWind(wind);
    const float windLen = std::sqrt(baseWind.x * baseWind.x + baseWind.y * baseWind.y);
    const Vec3 windDir{baseWind.x / windLen, baseWind.y / windLen, 0.f};
    const Vec3 perpDir{-windDir.y, windDir.x, 0.f};
    const Vec3 forward = TumbleweedCameraForward(cam);
    const bool farZone = zone == TumbleweedZone::Far;
    const bool inFront = !farZone && cam.valid && cam.pov;

    bool found = false;
    float bestScore = -1e9f;
    Vec3 bestPos{};

    auto evaluateCandidate = [&](Vec3 raw, bool requireOpenSky) {
        // Snap onto solid floor right under raw candidate
        Vec3 pos = raw;
        if (raycast) {
            TumbleweedRayHit floorHit;
            if (!raycast({raw.x, raw.y, raw.z + 48.f}, {0, 0, -1}, 160.f, floorHit) || floorHit.normal.z < .55f)
                return;
            pos = {floorHit.position.x, floorHit.position.y, floorHit.position.z + radius * .95f};
            // Prefer open sky (outdoors) on the first pass so tumbleweeds don't spawn in closed rooms
            if (requireOpenSky) {
                TumbleweedRayHit roofHit;
                if (raycast(pos, {0, 0, 1}, 240.f, roofHit)) return;
            }
            // Keep clear of immediate walls (4 cardinal feelers) and require open rolling space downwind on pass 0
            const Vec3 dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
            const float wallClear = requireOpenSky ? (radius * 1.8f + 12.f) : (radius + 6.f);
            for (const Vec3 &d : dirs) {
                TumbleweedRayHit wHit;
                if (raycast(pos, d, wallClear, wHit)) return;
            }
            if (requireOpenSky) {
                TumbleweedRayHit fwdHit;
                if (raycast(pos, windDir, radius * 3.5f, fwdHit) && fwdHit.normal.z < .55f) return;
            }
        } else {
            pos.z += radius * .95f;
        }

        // Spacing from active tumbleweeds
        for (const auto &b : existing) {
            if (!b.active) continue;
            const Vec3 sep = pos - b.pos;
            if (Dot(sep, sep) < (farZone ? 300.f * 300.f : 130.f * 130.f)) return;
        }

        // HARD GUARANTEE: never spawn visible to the camera
        if (TumbleweedVisibleToCamera(cam, pos, radius, raycast)) return;

        float score = 0.f;
        if (!cam.valid) {
            score = TumbleweedRand01(rng);
        } else {
            const Vec3 rel = pos - cam.position;
            const float dist = std::sqrt(rel.x * rel.x + rel.y * rel.y);
            if (farZone) {
                // Out on the map, anywhere well away from the camera.
                if (dist < kTumbleweedNearReach + 250.f) return;
                score = TumbleweedRand01(rng);
            } else {
                if (dist < kTumbleweedMinSpawnDist || dist > (std::min)(kTumbleweedMaxSpawnDist, kTumbleweedNearReach)) return;
                float along = 0.f;
                const bool comes = TumbleweedComesIntoView(cam, pos, radius, windDir, raycast, along);
                if (inFront) {
                    // In front of the player, coming out from cover into what they are looking at.
                    const float facing = (rel.x * forward.x + rel.y * forward.y) / (std::max)(dist, 1.f);
                    if (facing < .34f) return;
                    const float distBonus = 1.f - std::abs(dist - 850.f) / 1000.f;
                    score = (comes ? 3.f - along / 400.f : 0.f) + facing * 1.2f + distBonus + TumbleweedRand01(rng) * .45f;
                } else {
                    // Upwind coordinate: negative means upwind of camera (wind will carry it into view!)
                    const float upAlong = rel.x * windDir.x + rel.y * windDir.y;
                    const float across = std::abs(rel.x * perpDir.x + rel.y * perpDir.y);
                    // Ideal: 450..1350 units upwind, within ~550 units laterally of the camera's corridor
                    const float upwindBonus = upAlong < 120.f ? (1.f - std::abs(upAlong + 800.f) / 1600.f) : -.8f;
                    const float corridorBonus = 1.f - (std::min)(across / 900.f, 1.f);
                    const float distBonus = 1.f - std::abs(dist - 950.f) / 1500.f;
                    score = upwindBonus * 2.2f + corridorBonus * 1.4f + distBonus + (comes ? 1.5f - along / 600.f : 0.f) +
                            TumbleweedRand01(rng) * .45f;
                }
            }
        }
        if (score > bestScore) {
            bestScore = score;
            bestPos = pos;
            found = true;
        }
    };

    const bool hasNav = !navPositions.empty();
    const float spanX = mapHigh.x - mapLow.x, spanY = mapHigh.y - mapLow.y;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        const bool requireOpenSky = (pass == 0);
        const int tries = hasNav ? 64 : 36;
        for (int t = 0; t < tries; ++t) {
            Vec3 cand{};
            if (hasNav) {
                const size_t idx = size_t(TumbleweedNextRand(rng)) % navPositions.size();
                cand = navPositions[idx];
            } else if (cam.valid && !farZone) {
                // No nav mesh (a unit test or a bare plane): a ring in front of the player, or upwind.
                const float out = 420.f + TumbleweedRand01(rng) * 1100.f;
                const float lateral = (TumbleweedRand01(rng) * 2.f - 1.f) * 700.f;
                const Vec3 ahead = inFront ? forward : windDir * -1.f, side{-ahead.y, ahead.x, 0.f};
                cand = {cam.position.x + ahead.x * out + side.x * lateral,
                        cam.position.y + ahead.y * out + side.y * lateral, cam.position.z};
            } else if (spanX > 1.f && spanY > 1.f) {
                cand = {mapLow.x + TumbleweedRand01(rng) * spanX, mapLow.y + TumbleweedRand01(rng) * spanY,
                        (mapLow.z + mapHigh.z) * .5f};
            } else {
                continue;
            }
            evaluateCandidate(cand, requireOpenSky);
        }
    }

    if (found) outPos = bestPos;
    return found;
}

// Local airflow channeling: real wind flows around buildings and down streets instead of blowing
// straight through masonry. When a wall lies ahead of the steady wind, bend the local air stream
// smoothly toward the more open alley/corner direction (or calm down in a dead-end alcove).
inline Vec3 TumbleweedChannelWind(const TumbleweedBody &b, Vec3 desiredAir,
                                  const TumbleweedRaycast &raycast, bool &nearWall) {
    nearWall = false;
    const float speed = std::sqrt(desiredAir.x * desiredAir.x + desiredAir.y * desiredAir.y);
    if (!raycast || speed < 1e-3f) return desiredAir;
    const Vec3 wDir{desiredAir.x / speed, desiredAir.y / speed, 0.f};
    const Vec3 wSide{-wDir.y, wDir.x, 0.f};
    const float lookDist = (std::max)(b.radius * 6.5f, 105.f);
    const Vec3 probePos{b.pos.x, b.pos.y, b.pos.z + b.radius * .15f};

    auto clearance = [&](Vec3 dir, float range) -> float {
        TumbleweedRayHit h;
        if (raycast(probePos, dir, range, h) && h.normal.z < .55f) return h.distance;
        return range;
    };

    const float fwdClear = clearance(wDir, lookDist);
    if (fwdClear < b.radius * 3.0f) nearWall = true;
    if (fwdClear >= lookDist * .95f) return desiredAir;

    // A wall blocks the straight wind ahead: feel left and right at ±45° and ±80° to find the
    // open street corridor. Tie-break consistently per tumbleweed so it never dithers left-right.
    const float pref = (b.seed & 1u) ? 1.f : -1.f;
    const Vec3 dL45 = Normalize(wDir * .707f + wSide * .707f);
    const Vec3 dR45 = Normalize(wDir * .707f - wSide * .707f);
    const Vec3 dL80 = Normalize(wDir * .174f + wSide * .985f);
    const Vec3 dR80 = Normalize(wDir * .174f - wSide * .985f);
    const float cL45 = clearance(dL45, lookDist);
    const float cR45 = clearance(dR45, lookDist);
    const float cL80 = clearance(dL80, lookDist);
    const float cR80 = clearance(dR80, lookDist);

    const float leftScore = cL45 * 1.15f + cL80 * .85f + (pref > 0.f ? 6.f : 0.f);
    const float rightScore = cR45 * 1.15f + cR80 * .85f + (pref < 0.f ? 6.f : 0.f);
    const bool goLeft = leftScore >= rightScore;
    const float best45 = goLeft ? cL45 : cR45;
    const float best80 = goLeft ? cL80 : cR80;
    const Vec3 steer = best45 >= best80 * .75f ? (goLeft ? dL45 : dR45) : (goLeft ? dL80 : dR80);
    const float bestClear = (std::max)(best45, best80);

    // Cornered dead-end alcove: calm eddy so the tumbleweed settles naturally instead of ramming walls.
    if (bestClear < b.radius * 2.4f) {
        nearWall = true;
        return steer * (speed * .18f);
    }

    const float block = 1.f - std::clamp((fwdClear - b.radius * 1.6f) / (lookDist - b.radius * 1.6f), 0.f, 1.f);
    const Vec3 blended = Normalize(wDir * (1.f - block) + steer * block);
    // Air bends around a corner with slightly reduced momentum compared to an unobstructed street.
    return blended * (speed * (1.f - .28f * block));
}

// Integrate one tumbleweed for `dt` seconds with street-channeled wind, gusts, ground rolling,
// verified low-curb hops, calm wall grazing/settling, and player capsule deflection.
inline void TumbleweedStepBody(TumbleweedBody &b, float dt, double simTime, const TumbleweedWind &wind,
                               const TumbleweedRaycast &raycast,
                               const std::vector<TumbleweedActor> &actors, uint32_t &rng) {
    if (!b.active || !(dt > 0.f)) return;
    b.age += dt;
    b.hopCooldown = (std::max)(0.f, b.hopCooldown - dt);

    // Check shelter overhead (slows wind under roofs/bridges/rooms)
    float shelterMul = 1.f;
    if (raycast) {
        TumbleweedRayHit roof;
        if (raycast(b.pos, {0, 0, 1}, 220.f, roof)) shelterMul = .42f;
    }

    // Wind force channeled smoothly around walls and down corridors
    bool nearWall = false;
    const Vec3 rawAir = TumbleweedWindAt(b.pos, simTime, wind, b.seed) * shelterMul;
    const Vec3 chanAir = TumbleweedChannelWind(b, rawAir, raycast, nearWall);
    const float chanSpd = std::sqrt(chanAir.x * chanAir.x + chanAir.y * chanAir.y);
    if (chanSpd > 1e-3f) {
        const Vec3 wantDir{chanAir.x / chanSpd, chanAir.y / chanSpd, 0.f};
        if (Dot(b.flowDir, b.flowDir) < .1f) {
            b.flowDir = wantDir;
        } else {
            const float turnAlpha = 1.f - std::exp(-5.5f * dt);
            b.flowDir = Normalize(b.flowDir + (wantDir - b.flowDir) * turnAlpha);
        }
    }
    const Vec3 air = Dot(b.flowDir, b.flowDir) > .1f ? b.flowDir * chanSpd : chanAir;
    const float dragRate = b.grounded ? 1.85f : 1.1f;
    const Vec3 targetVel = air * .78f;
    b.vel.x += (targetVel.x - b.vel.x) * (1.f - std::exp(-dragRate * dt));
    b.vel.y += (targetVel.y - b.vel.y) * (1.f - std::exp(-dragRate * dt));
    b.vel.z -= kTumbleweedGravity * dt;

    // Cap extreme speeds so a tumbleweed never tunnels through thin walls
    const float hSpeed = std::sqrt(b.vel.x * b.vel.x + b.vel.y * b.vel.y);
    if (hSpeed > 340.f) {
        const float k = 340.f / hSpeed;
        b.vel.x *= k;
        b.vel.y *= k;
    }
    b.vel.z = std::clamp(b.vel.z, -520.f, 220.f);

    const Vec3 prevPos = b.pos;

    // Horizontal motion + wall/curb collision
    Vec3 horizStep{b.vel.x * dt, b.vel.y * dt, 0.f};
    const float horizDist = std::sqrt(horizStep.x * horizStep.x + horizStep.y * horizStep.y);
    if (raycast && horizDist > 1e-4f) {
        const Vec3 dir{horizStep.x / horizDist, horizStep.y / horizDist, 0.f};
        const Vec3 side{-dir.y, dir.x, 0.f};
        // Three forward feelers at centre height catch full walls, posts and corners
        const Vec3 feelers[3] = {
            dir,
            Normalize(dir * .82f + side * .57f),
            Normalize(dir * .82f - side * .57f),
        };
        const float probeLen = b.radius + horizDist + 2.5f;
        bool hitWall = false;
        bool lowOnlyHit = false;
        TumbleweedRayHit closest{};
        closest.distance = 1e9f;
        for (const Vec3 &fDir : feelers) {
            TumbleweedRayHit h;
            if (raycast(b.pos, fDir, probeLen, h) && h.normal.z < .55f && h.distance < closest.distance) {
                closest = h;
                hitWall = true;
            }
        }
        // Low forward feeler near the base of the brush catches curbs and stair risers (4..18 u)
        // only when centre height is clear.
        if (!hitWall && b.grounded) {
            TumbleweedRayHit lowHit;
            if (raycast(b.pos - Vec3{0, 0, b.radius * .55f}, dir, probeLen, lowHit) && lowHit.normal.z < .55f) {
                closest = lowHit;
                hitWall = true;
                lowOnlyHit = true;
            }
        }
        if (hitWall) {
            nearWall = true;
            // ONLY hop if it is genuinely a low climbable curb/stair step (3..18 u tall with a
            // walkable ledge above it and clear air at body height) — never jump at real walls!
            float stepHeight = 0.f;
            bool lowStep = false;
            if (lowOnlyHit && b.grounded && hSpeed > 35.f) {
                TumbleweedRayHit upperHit, ledgeHit;
                const bool headClear = !raycast(b.pos + Vec3{0, 0, b.radius * .65f}, dir, b.radius + 18.f, upperHit);
                if (headClear) {
                    const float curFloor = b.pos.z - b.radius * .92f;
                    const Vec3 overStep = b.pos + dir * (closest.distance + b.radius * .55f) + Vec3{0, 0, b.radius * .9f};
                    if (raycast(overStep, {0, 0, -1}, b.radius * 1.45f, ledgeHit) && ledgeHit.normal.z > .65f) {
                        stepHeight = ledgeHit.position.z - curFloor;
                        lowStep = stepHeight >= 3.f && stepHeight <= 18.f;
                    }
                }
            }
            if (lowStep) {
                // Modest, realistic hop sized just to climb the curb/step height.
                const float climbV = std::clamp(std::sqrt(2.f * kTumbleweedGravity * (stepHeight + 4.5f)), 95.f, 178.f);
                b.vel.z = (std::max)(b.vel.z, climbV);
                b.grounded = false;
                b.pos = b.pos + horizStep * .65f;
            } else {
                // Real wall: push out cleanly, absorb normal impact softly (no rubber-ball rebound,
                // NO upward jump!), and glide calmly along the wall tangent.
                const Vec3 wn = Normalize({closest.normal.x, closest.normal.y, 0.f});
                const float pen = (b.radius + 1.f) - closest.distance;
                if (pen > 0.f) b.pos = b.pos + wn * pen;
                const float vn = b.vel.x * wn.x + b.vel.y * wn.y;
                if (vn < 0.f) {
                    const float restitution = .08f + .06f * TumbleweedRand01(rng);
                    const float pushOff = (std::min)(14.f, -vn * restitution);
                    // Glide along the wall in the direction of the channeled wind / existing tangent
                    const Vec3 wallTan{-wn.y, wn.x, 0.f};
                    const float vt = b.vel.x * wallTan.x + b.vel.y * wallTan.y;
                    const float wt = air.x * wallTan.x + air.y * wallTan.y;
                    const float slideDir = std::abs(wt) > 8.f ? (wt > 0.f ? 1.f : -1.f)
                                           : (std::abs(vt) > 8.f ? (vt > 0.f ? 1.f : -1.f)
                                                                 : ((b.seed & 1u) ? 1.f : -1.f));
                    // Dampen sliding slightly so dry brush scrapes gently along walls instead of ricocheting
                    const float slideV = vt * .78f + slideDir * (std::min)(18.f, std::abs(vn) * .14f);
                    b.vel.x = wallTan.x * slideV + wn.x * pushOff;
                    b.vel.y = wallTan.y * slideV + wn.y * pushOff;
                    // Re-align smoothed airflow along the wall so it doesn't keep ramming back in
                    b.flowDir = wallTan * slideDir;
                }
            }
        } else {
            b.pos = b.pos + horizStep;
        }
    } else {
        b.pos = b.pos + horizStep;
    }

    // Player capsule deflection: roll softly around players' legs instead of clipping or launching
    for (const auto &actor : actors) {
        if (b.pos.z + b.radius < actor.origin.z || b.pos.z - b.radius > actor.origin.z + 74.f) continue;
        const float dx = b.pos.x - actor.origin.x;
        const float dy = b.pos.y - actor.origin.y;
        const float minD = actor.radius + b.radius;
        const float d2 = dx * dx + dy * dy;
        if (d2 < minD * minD && d2 > 1e-4f) {
            const float d = std::sqrt(d2);
            const float nx = dx / d, ny = dy / d;
            b.pos.x = actor.origin.x + nx * minD;
            b.pos.y = actor.origin.y + ny * minD;
            const float vn = b.vel.x * nx + b.vel.y * ny;
            if (vn < 0.f) {
                b.vel.x -= nx * 1.15f * vn;
                b.vel.y -= ny * 1.15f * vn;
                if (std::abs(vn) > 70.f) b.vel.z = (std::max)(b.vel.z, 26.f);
            }
        }
    }

    // Vertical motion + ceiling & ground contact
    if (raycast && b.vel.z > 0.f) {
        TumbleweedRayHit ceilHit;
        const float upDist = b.radius + b.vel.z * dt;
        if (raycast(b.pos, {0, 0, 1}, upDist, ceilHit)) {
            b.pos.z = ceilHit.position.z - b.radius;
            b.vel.z = -b.vel.z * .15f;
        } else {
            b.pos.z += b.vel.z * dt;
        }
    } else {
        b.pos.z += b.vel.z * dt;
    }

    Vec3 groundNormal{0, 0, 1};
    b.grounded = false;
    if (raycast) {
        TumbleweedRayHit floorHit;
        const float startLift = b.radius * .65f;
        const float downLen = startLift + b.radius * 1.15f;
        if (raycast({b.pos.x, b.pos.y, b.pos.z + startLift}, {0, 0, -1}, downLen, floorHit) &&
            floorHit.normal.z > .35f) {
            // Slight brush compression (0.92 * r) so twigs kiss the sand/floor visually
            const float restZ = floorHit.position.z + b.radius * .92f;
            if (b.pos.z <= restZ + 2.f) {
                b.pos.z = restZ;
                groundNormal = floorHit.normal;
                if (b.vel.z < -125.f) {
                    // Only rebound lightly after a real drop off a ledge/stairs — dry twigs absorb impact
                    b.vel.z = -b.vel.z * (.18f + .06f * TumbleweedRand01(rng));
                } else {
                    b.grounded = true;
                    b.vel.z = 0.f;
                    // Project velocity along floor slope
                    const float vn = Dot(b.vel, groundNormal);
                    if (vn < 0.f) b.vel = b.vel - groundNormal * vn;
                    // Rolling friction on twigs
                    const float rollDrag = std::exp(-.75f * dt);
                    b.vel.x *= rollDrag;
                    b.vel.y *= rollDrag;
                    // Rare, subtle gust-skip ONLY in open windy streets at brisk speed (never under
                    // roofs, never beside walls, never frantic rabbit-hopping).
                    const float spd = std::sqrt(b.vel.x * b.vel.x + b.vel.y * b.vel.y);
                    if (b.hopCooldown <= 0.f && !nearWall && shelterMul > .8f && spd > 135.f && wind.gusts > .35f) {
                        const float chance = (std::min)(.25f, dt * .14f * std::clamp(wind.gusts, 0.f, 1.f));
                        if (TumbleweedRand01(rng) < chance) {
                            b.vel.z = 52.f + TumbleweedRand01(rng) * 32.f;
                            b.grounded = false;
                            b.hopCooldown = 3.2f + TumbleweedRand01(rng) * 3.3f;
                        }
                    }
                }
            }
        }
    } else {
        // No collision geometry fallback: flat z = 0 plane
        if (b.pos.z <= b.radius * .92f) {
            b.pos.z = b.radius * .92f;
            b.vel.z = b.vel.z < -125.f ? -b.vel.z * .2f : 0.f;
            b.grounded = b.vel.z == 0.f;
        }
    }

    // Physical no-slip rolling orientation: omega = (n x v) / r on ground, free spin in air
    const Vec3 moved = b.pos - prevPos;
    const float movedDist = std::sqrt(moved.x * moved.x + moved.y * moved.y);
    if (b.grounded) {
        if (movedDist > 1e-4f) {
            const Vec3 vHoriz{moved.x / dt, moved.y / dt, 0.f};
            b.omega = Cross(groundNormal, vHoriz) * (1.f / (std::max)(b.radius, 1.f));
            // Subtle irregular wobble because a real tumbleweed isn't a lathe-turned sphere
            const float wobble = std::sin(b.age * 4.7f + float(b.seed & 255u)) * .22f;
            b.omega.z += wobble * (movedDist / dt) / (std::max)(b.radius, 1.f);
        } else {
            b.omega = b.omega * std::exp(-6.f * dt);
        }
    }
    const float omegaMag = std::sqrt(Dot(b.omega, b.omega));
    if (omegaMag > 1e-5f) {
        const TumbleweedQuat dq = TumbleweedQuatAxisAngle(b.omega, omegaMag * dt);
        b.orientation = TumbleweedQuatNormalize(TumbleweedQuatMul(dq, b.orientation));
    }

    // Track whether pinned in a corner with negligible motion
    if (movedDist / dt < 10.f) b.stuckTime += dt;
    else b.stuckTime = (std::max)(0.f, b.stuckTime - dt * 2.f);
}

// Step the whole tumbleweed simulation to `simTime`. Handles pause (dt == 0), demo seeks (reset +
// pre-warm), and the two populations: at most kTumbleweedNearMost near the camera, arriving one at a
// time (kTumbleweed*Gap* apart), and the rest out on the map. Everything is set down and taken away
// only out of the camera's sight.
inline float TumbleweedArrivalGap(const TumbleweedCamera &cam, uint32_t &rng) {
    const float r = TumbleweedRand01(rng);
    return cam.pov ? kTumbleweedPovGapLeast + r * (kTumbleweedPovGapMost - kTumbleweedPovGapLeast)
                   : kTumbleweedCineGapLeast + r * (kTumbleweedCineGapMost - kTumbleweedCineGapLeast);
}
inline bool TumbleweedNearCamera(const TumbleweedCamera &cam, const TumbleweedBody &b) {
    if (!cam.valid) return true;
    const float dx = b.pos.x - cam.position.x, dy = b.pos.y - cam.position.y;
    return dx * dx + dy * dy <= kTumbleweedNearReach * kTumbleweedNearReach;
}
inline void TumbleweedStepSim(TumbleweedSim &sim, double simTime, int targetCount,
                              const TumbleweedWind &wind, const TumbleweedCamera &cam,
                              const std::vector<Vec3> &navPositions, Vec3 mapLow, Vec3 mapHigh,
                              const TumbleweedRaycast &raycast,
                              const std::vector<TumbleweedActor> &actors) {
    targetCount = std::clamp(targetCount, 0, kTumbleweedMax);
    if (targetCount == 0) {
        for (auto &b : sim.bodies) b = TumbleweedBody{};
        sim.initialized = false;
        sim.nearTimer = sim.farTimer = 0.f;
        return;
    }

    auto spawnOne = [&](TumbleweedZone zone) -> bool {
        int slot = -1;
        for (int i = 0; i < kTumbleweedMax; ++i)
            if (!sim.bodies[size_t(i)].active) { slot = i; break; }
        if (slot < 0) return false;
        const float radius = kTumbleweedMinRadius + TumbleweedRand01(sim.rng) * (kTumbleweedMaxRadius - kTumbleweedMinRadius);
        Vec3 pos{};
        if (!TumbleweedPickSpawn(navPositions, mapLow, mapHigh, cam, wind, raycast, sim.bodies, sim.rng, radius, pos, zone))
            return false;
        TumbleweedBody &b = sim.bodies[size_t(slot)];
        b = TumbleweedBody{};
        b.active = true;
        b.pos = pos;
        b.radius = radius;
        b.seed = TumbleweedNextRand(sim.rng);
        b.maxAge = 32.f + TumbleweedRand01(sim.rng) * 20.f;
        b.hopCooldown = .5f + TumbleweedRand01(sim.rng) * 1.5f;
        // Start rolling at ~65% wind speed so it enters with natural momentum
        const Vec3 wv = TumbleweedBaseWind(wind);
        b.vel = {wv.x * .65f, wv.y * .65f, 0.f};
        // Random starting orientation
        const Vec3 ax{TumbleweedRand01(sim.rng) - .5f, TumbleweedRand01(sim.rng) - .5f, TumbleweedRand01(sim.rng) - .5f};
        b.orientation = TumbleweedQuatAxisAngle(ax, TumbleweedRand01(sim.rng) * 6.2831853f);
        return true;
    };
    const int nearCap = (std::min)(targetCount, kTumbleweedNearMost);
    const int farCap = targetCount - nearCap;

    const bool seeked = !sim.initialized || simTime < sim.lastTime - .05 || simTime > sim.lastTime + 5.0;
    if (seeked) {
        for (auto &b : sim.bodies) b = TumbleweedBody{};
        sim.initialized = true;
        sim.lastTime = simTime;
        // Deterministic seed from rounded time so seeks/re-renders stay tidy
        sim.rng = uint32_t(std::floor(std::abs(simTime) * 10.0)) * 2654435761u ^ 0x51a7c93du;
        if (!sim.rng) sim.rng = 0x51a7c93du;
        // The map's share is set out at once (it is out of sight) and pre-warmed so it is already
        // rolling; the first one near the camera comes a moment later, on its own.
        for (int i = 0; i < farCap; ++i) spawnOne(TumbleweedZone::Far);
        constexpr float preDt = 1.f / 40.f;
        for (int s = 0; s < 60; ++s) {
            const double t = simTime - (60 - s) * double(preDt);
            for (auto &b : sim.bodies)
                if (b.active) TumbleweedStepBody(b, preDt, t, wind, raycast, actors, sim.rng);
        }
        sim.nearTimer = kTumbleweedFirstLeast + TumbleweedRand01(sim.rng) * (kTumbleweedFirstMost - kTumbleweedFirstLeast);
        sim.farTimer = .5f;
        return;
    }

    const float totalDt = float(simTime - sim.lastTime);
    sim.lastTime = simTime;
    if (!(totalDt > 0.f)) return; // paused demo: freeze tumbleweeds in place

    // Substep at <= 1/60s for rock-solid wall & ground collision
    constexpr float kMaxStep = 1.f / 60.f;
    const int steps = std::clamp(int(std::ceil(totalDt / kMaxStep)), 1, 8);
    const float dt = totalDt / float(steps);
    for (int s = 0; s < steps; ++s) {
        const double t = simTime - totalDt + (s + 1) * double(dt);
        for (auto &b : sim.bodies)
            if (b.active) TumbleweedStepBody(b, dt, t, wind, raycast, actors, sim.rng);
        // Gentle pairwise brush separation so tumbleweeds never clip through or stack on each other
        for (int i = 0; i < kTumbleweedMax; ++i) {
            auto &a = sim.bodies[size_t(i)];
            if (!a.active) continue;
            for (int j = i + 1; j < kTumbleweedMax; ++j) {
                auto &c = sim.bodies[size_t(j)];
                if (!c.active) continue;
                const float dz = std::abs(a.pos.z - c.pos.z);
                const float minD = (a.radius + c.radius) * .95f;
                if (dz > minD) continue;
                const float dx = c.pos.x - a.pos.x, dy = c.pos.y - a.pos.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= minD * minD || d2 < 1e-4f) continue;
                const float d = std::sqrt(d2);
                const float nx = dx / d, ny = dy / d;
                const float push = (minD - d) * .5f;
                a.pos.x -= nx * push; a.pos.y -= ny * push;
                c.pos.x += nx * push; c.pos.y += ny * push;
                const float relVn = (c.vel.x - a.vel.x) * nx + (c.vel.y - a.vel.y) * ny;
                if (relVn < 0.f) {
                    const float imp = -relVn * .55f;
                    a.vel.x -= nx * imp; a.vel.y -= ny * imp;
                    c.vel.x += nx * imp; c.vel.y += ny * imp;
                }
            }
        }
    }

    auto hidden = [&](const TumbleweedBody &b) { return !TumbleweedVisibleToCamera(cam, b.pos, b.radius, raycast); };
    // Recycle what fell out of the map, rolled off its edge, expired or got pinned in a corner, out of view.
    for (auto &b : sim.bodies) {
        if (!b.active) continue;
        const bool fellOut = b.pos.z < mapLow.z - 512.f;
        const bool offMap = b.pos.x < mapLow.x - 256.f || b.pos.x > mapHigh.x + 256.f || b.pos.y < mapLow.y - 256.f ||
                            b.pos.y > mapHigh.y + 256.f;
        const bool expired = b.age > b.maxAge || b.stuckTime > 9.f;
        if (fellOut || ((offMap || expired) && hidden(b))) b.active = false;
    }

    auto counts = [&](int &nearCount, int &farCount) {
        nearCount = farCount = 0;
        for (const auto &b : sim.bodies)
            if (b.active) (TumbleweedNearCamera(cam, b) ? nearCount : farCount) += 1;
    };
    // Retire one: the furthest from the camera of those that are hidden (and near or far as asked),
    // or, when the slider was lowered, the furthest of any.
    auto retire = [&](int want /* 1 near, 0 far, -1 any */, bool mustBeHidden) -> bool {
        int drop = -1;
        float bestDist = -1.f;
        for (int i = 0; i < kTumbleweedMax; ++i) {
            const auto &b = sim.bodies[size_t(i)];
            if (!b.active) continue;
            if (want >= 0 && TumbleweedNearCamera(cam, b) != (want == 1)) continue;
            if (mustBeHidden && !hidden(b)) continue;
            const float d = cam.valid ? std::sqrt(Dot(b.pos - cam.position, b.pos - cam.position)) : 0.f;
            if (d > bestDist) { bestDist = d; drop = i; }
        }
        if (drop < 0) return false;
        sim.bodies[size_t(drop)].active = false;
        return true;
    };

    int nearCount = 0, farCount = 0;
    counts(nearCount, farCount);
    // The slider was lowered: hidden ones go first, then the furthest.
    while (nearCount + farCount > targetCount && (retire(-1, true) || retire(-1, false))) counts(nearCount, farCount);
    // More near the camera than allowed (some of the map's share rolled in): the hidden extras go.
    while (nearCount > nearCap && retire(1, true)) counts(nearCount, farCount);

    // THE NEXT ARRIVAL NEAR THE CAMERA, one at a time. When the map's share already uses up the
    // count, a hidden one out on the map makes way for it.
    sim.nearTimer = (std::max)(0.f, sim.nearTimer - totalDt);
    if (nearCount < nearCap && sim.nearTimer <= 0.f) {
        if (nearCount + farCount >= targetCount) retire(0, true);
        if (spawnOne(TumbleweedZone::Near)) {
            sim.nearTimer = TumbleweedArrivalGap(cam, sim.rng);
            counts(nearCount, farCount);
        } else {
            sim.nearTimer = .4f; // retry soon once camera shifts or another spot opens
        }
    }
    // The rest roll about the map out of sight, topped up a few at a time.
    sim.farTimer = (std::max)(0.f, sim.farTimer - totalDt);
    if (farCount < farCap && nearCount + farCount < targetCount && sim.farTimer <= 0.f)
        sim.farTimer = spawnOne(TumbleweedZone::Far) ? .25f : 1.f;
}

} // namespace Filmmaker::Environment
