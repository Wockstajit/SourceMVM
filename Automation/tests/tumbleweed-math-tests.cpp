#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentTumbleweedMath.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace Filmmaker::Environment;

static void Check(bool ok, const char *why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

// Simple perspective camera at `eye`, looking along +X (with +Z up, +Y left in Source2 right-handed coords).
// Projects +X into clip.w, -Y into clip.x (screen right = -Y), +Z into clip.y (screen up = +Z).
static TumbleweedCamera MakeLookPlusXCamera(Vec3 eye, float tanHalfFov = 1.f) {
    TumbleweedCamera c;
    c.valid = true;
    c.position = eye;
    const float inv = 1.f / tanHalfFov;
    // cx = inv * (-(y - eye.y)), cy = inv * (z - eye.z), cw = x - eye.x
    c.worldToClip = {
        0.f, -inv, 0.f,  inv * eye.y,
        0.f,  0.f, inv, -inv * eye.z,
        1.f,  0.f, 0.f, -eye.x,
        1.f,  0.f, 0.f, -eye.x,
    };
    return c;
}

// Raycast against flat ground z = 0 plus optional vertical walls / boxes.
struct Box {
    Vec3 lo, hi;
};

static bool RayBox(Vec3 o, Vec3 d, float maxDist, const Box &b, TumbleweedRayHit &best) {
    float tmin = 0.f, tmax = maxDist;
    Vec3 hitNormal{};
    for (int axis = 0; axis < 3; ++axis) {
        const float oi = axis == 0 ? o.x : (axis == 1 ? o.y : o.z);
        const float di = axis == 0 ? d.x : (axis == 1 ? d.y : d.z);
        const float lo = axis == 0 ? b.lo.x : (axis == 1 ? b.lo.y : b.lo.z);
        const float hi = axis == 0 ? b.hi.x : (axis == 1 ? b.hi.y : b.hi.z);
        if (std::abs(di) < 1e-7f) {
            if (oi < lo || oi > hi) return false;
        } else {
            float t1 = (lo - oi) / di;
            float t2 = (hi - oi) / di;
            float sign = -1.f;
            if (t1 > t2) { std::swap(t1, t2); sign = 1.f; }
            if (t1 > tmin) {
                tmin = t1;
                hitNormal = {axis == 0 ? sign : 0.f, axis == 1 ? sign : 0.f, axis == 2 ? sign : 0.f};
            }
            tmax = (std::min)(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    if (tmin <= 1e-4f || tmin >= best.distance) return false;
    best.distance = tmin;
    best.position = o + d * tmin;
    best.normal = hitNormal;
    return true;
}

static TumbleweedRaycast MakeWorldRay(const std::vector<Box> &boxes, bool hasFloor = true) {
    return [boxes, hasFloor](Vec3 o, Vec3 d, float range, TumbleweedRayHit &hit) -> bool {
        TumbleweedRayHit best{};
        best.distance = range;
        bool any = false;
        if (hasFloor && d.z < -1e-6f && o.z >= 0.f) {
            const float t = -o.z / d.z;
            if (t > 1e-4f && t < best.distance) {
                best.distance = t;
                best.position = o + d * t;
                best.normal = {0, 0, 1};
                any = true;
            }
        }
        for (const auto &b : boxes) {
            if (RayBox(o, d, best.distance, b, best)) any = true;
        }
        if (any) hit = best;
        return any;
    };
}

int main() {
    {
        // Quaternion + row-major model matrix orthogonality, uniform scale and translation.
        const TumbleweedQuat q = TumbleweedQuatAxisAngle({0, 1, 0}, 1.23f);
        const Matrix m = TumbleweedModelMatrix({10, 20, 30}, q, 18.f);
        Check(std::abs(m[3] - 10.f) < 1e-4f && std::abs(m[7] - 20.f) < 1e-4f && std::abs(m[11] - 30.f) < 1e-4f,
              "Model matrix stores world position in the last column");
        const Vec3 c0{m[0], m[4], m[8]}, c1{m[1], m[5], m[9]}, c2{m[2], m[6], m[10]};
        Check(std::abs(std::sqrt(Dot(c0, c0)) - 18.f) < 1e-3f &&
                  std::abs(std::sqrt(Dot(c1, c1)) - 18.f) < 1e-3f &&
                  std::abs(std::sqrt(Dot(c2, c2)) - 18.f) < 1e-3f,
              "Model matrix scales all three axes by the tumbleweed radius");
        Check(std::abs(Dot(c0, c1)) < 1e-2f && std::abs(Dot(c1, c2)) < 1e-2f && std::abs(Dot(c0, c2)) < 1e-2f,
              "Model matrix rotation axes remain orthogonal");
    }
    {
        // Off-camera frustum & wall-occlusion visibility rules.
        const TumbleweedCamera cam = MakeLookPlusXCamera({0, 0, 64});
        const std::vector<Box> tallWall = {{{300, -200, 0}, {320, 200, 300}}};
        const std::vector<Box> lowWall = {{{300, -200, 0}, {320, 200, 42}}};
        const auto openRay = MakeWorldRay({});
        const auto tallRay = MakeWorldRay(tallWall);
        const auto lowRay = MakeWorldRay(lowWall);

        Check(TumbleweedInFrustum(cam, {600, 0, 16}, 16.f), "Point straight ahead is inside frustum");
        Check(!TumbleweedInFrustum(cam, {-600, 0, 16}, 16.f), "Point behind camera is outside frustum");
        Check(!TumbleweedInFrustum(cam, {400, 900, 16}, 16.f), "Point far off to the side is outside frustum");

        Check(TumbleweedVisibleToCamera(cam, {-150, 0, 16}, 16.f, openRay),
              "Points closer than kTumbleweedMinSpawnDist are rejected even behind the camera");
        Check(!TumbleweedVisibleToCamera(cam, {-600, 0, 16}, 16.f, openRay),
              "Far point behind the camera is hidden and safe to spawn");
        Check(TumbleweedVisibleToCamera(cam, {600, 0, 16}, 16.f, openRay),
              "Unoccluded point in front of camera is visible (cannot spawn)");
        Check(!TumbleweedVisibleToCamera(cam, {600, 0, 16}, 16.f, tallRay),
              "Point solidly occluded behind a tall wall is hidden (can roll around the corner)");
        Check(TumbleweedVisibleToCamera(cam, {600, 0, 16}, 16.f, lowRay),
              "Point only half-hidden by a low wall is still considered visible");
    }
    {
        // Off-camera upwind spawner: picks upwind hidden nav spots, refuses if every spot is visible.
        const TumbleweedCamera cam = MakeLookPlusXCamera({0, 0, 64});
        const TumbleweedWind wind{140.f, 0.f, .5f}; // wind blows toward +X, so upwind is -X (behind camera)
        const auto ray = MakeWorldRay({});
        std::array<TumbleweedBody, kTumbleweedMax> empty{};
        uint32_t rng = 12345u;

        // Only visible points -> must fail instead of popping on screen
        const std::vector<Vec3> visibleOnly = {{600, 0, 0}, {750, 80, 0}, {900, -60, 0}};
        Vec3 out{};
        Check(!TumbleweedPickSpawn(visibleOnly, {-2000, -2000, 0}, {2000, 2000, 500}, cam, wind, ray, empty, rng, 16.f, out),
              "Spawner refuses to pop a tumbleweed when every nav spot is visible");

        // Mix of visible, upwind-hidden, and side-hidden points -> must pick a hidden upwind spot
        const std::vector<Vec3> mixed = {
            {650, 0, 0},    // visible (in front)
            {-750, 40, 0},  // hidden & directly upwind
            {500, 1100, 0}, // hidden & downwind off to the side
        };
        for (int trial = 0; trial < 20; ++trial) {
            Check(TumbleweedPickSpawn(mixed, {-2000, -2000, 0}, {2000, 2000, 500}, cam, wind, ray, empty, rng, 16.f, out),
                  "Spawner finds a hidden upwind spot");
            Check(!TumbleweedVisibleToCamera(cam, out, 16.f, ray),
                  "Every spawned tumbleweed is strictly hidden from the camera");
            Check(out.x < 0.f, "Spawner prefers the upwind corridor over downwind side spots");
        }
    }
    {
        // Ground rolling + no-slip quaternion spin along +X wind.
        const auto flatRay = MakeWorldRay({});
        const TumbleweedWind wind{160.f, 0.f, 0.f};
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {0, 0, 16.f * .92f};
        b.vel = {90.f, 0.f, 0.f};
        b.seed = 77u;
        b.hopCooldown = 10.f; // suppress random hops for deterministic rolling check
        uint32_t rng = 999u;
        for (int i = 0; i < 60; ++i)
            TumbleweedStepBody(b, 1.f / 60.f, double(i) / 60.0, wind, flatRay, {}, rng);
        Check(b.pos.x > 80.f && b.vel.x > 60.f, "Wind drives tumbleweed downwind along +X");
        Check(std::abs(b.pos.z - b.radius * .92f) < 1.f, "Rolling tumbleweed stays seated on the floor");
        Check(b.omega.y > 2.f, "No-slip rolling along +X on +Z floor spins around +Y");
        Check(std::abs(b.orientation.w) < .98f, "Rolling integrates quaternion orientation");
    }
    {
        // Wall collision & deflection: rolling straight into a wall at x = 100 never tunnels through
        // and NEVER launches crazily upward on wall contact.
        const auto wallRay = MakeWorldRay({{{100, -300, 0}, {140, 300, 250}}});
        const TumbleweedWind wind{200.f, 0.f, 0.f};
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {60, 0, 16.f * .92f};
        b.vel = {180.f, 0.f, 0.f};
        b.seed = 3u;
        b.hopCooldown = 10.f;
        uint32_t rng = 42u;
        bool bouncedOrDeflected = false;
        float maxVz = 0.f;
        for (int i = 0; i < 45; ++i) {
            TumbleweedStepBody(b, 1.f / 60.f, double(i) / 60.0, wind, wallRay, {}, rng);
            Check(b.pos.x <= 100.f - b.radius + 3.f, "Tumbleweed never tunnels through solid map wall");
            maxVz = (std::max)(maxVz, b.vel.z);
            if (b.vel.x < 0.f || std::abs(b.vel.y) > 15.f) bouncedOrDeflected = true;
        }
        Check(bouncedOrDeflected, "Wall impact deflects the tumbleweed smoothly along the wall");
        Check(maxVz < 15.f, "Wall impact never triggers crazy upward pogo-stick jumping");
    }
    {
        // Mid-height wall / crate (40 u tall, too high to be a curb): must NOT jump over it!
        const auto midWallRay = MakeWorldRay({{{100, -200, 0}, {160, 200, 40}}});
        const TumbleweedWind wind{170.f, 0.f, .6f};
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {70, 0, 16.f * .92f};
        b.vel = {140.f, 0.f, 0.f};
        b.grounded = true;
        b.seed = 9u;
        b.hopCooldown = 10.f;
        uint32_t rng = 321u;
        for (int i = 0; i < 45; ++i) {
            TumbleweedStepBody(b, 1.f / 60.f, double(i) / 60.0, wind, midWallRay, {}, rng);
            Check(b.vel.z < 15.f, "40-unit wall never triggers a curb jump");
        }
    }
    {
        // Street airflow channeling: with a wall ahead on the left and an open alley to the right (-Y),
        // local wind bends toward the open side instead of driving straight into the wall.
        const auto alleyRay = MakeWorldRay({
            {{120, -20, 0}, {160, 300, 200}},   // wall dead ahead and to the left (+Y)
            {{-100, 60, 0}, {160, 100, 200}},   // side wall closing +Y
        });
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {40, 0, 16.f * .92f};
        b.seed = 2u;
        bool nearWall = false;
        const Vec3 chan = TumbleweedChannelWind(b, {180.f, 0.f, 0.f}, alleyRay, nearWall);
        Check(chan.y < -30.f, "Channeled airflow bends toward the open alley (-Y) instead of slamming the wall");
    }
    {
        // Low curb hop: approaching a 10-unit-tall curb springs upward modestly to climb over it.
        const auto curbRay = MakeWorldRay({{{100, -200, 0}, {300, 200, 10}}});
        const TumbleweedWind wind{150.f, 0.f, 0.f};
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {78, 0, 16.f * .92f};
        b.vel = {130.f, 0.f, 0.f};
        b.grounded = true;
        b.hopCooldown = 10.f;
        uint32_t rng = 123u;
        bool hopped = false;
        float peakVz = 0.f;
        for (int i = 0; i < 30; ++i) {
            TumbleweedStepBody(b, 1.f / 60.f, double(i) / 60.0, wind, curbRay, {}, rng);
            peakVz = (std::max)(peakVz, b.vel.z);
            if (b.vel.z > 90.f) hopped = true;
        }
        Check(hopped, "Low curb triggers a realistic upward hop to climb onto the ledge");
        Check(peakVz <= 180.f, "Low curb hop stays modest and realistic rather than launching sky-high");
    }
    {
        // Player capsule bounce: does not clip through a standing player's legs.
        const auto flatRay = MakeWorldRay({});
        const TumbleweedWind wind{160.f, 0.f, 0.f};
        const std::vector<TumbleweedActor> actors = {{{60, 2, 0}, 18.f}};
        TumbleweedBody b;
        b.active = true;
        b.radius = 16.f;
        b.pos = {28, 2, 16.f * .92f};
        b.vel = {140.f, 0.f, 0.f};
        b.hopCooldown = 10.f;
        uint32_t rng = 55u;
        for (int i = 0; i < 20; ++i) {
            TumbleweedStepBody(b, 1.f / 60.f, double(i) / 60.0, wind, flatRay, actors, rng);
            const float dx = b.pos.x - actors[0].origin.x;
            const float dy = b.pos.y - actors[0].origin.y;
            Check(std::sqrt(dx * dx + dy * dy) >= actors[0].radius + b.radius - .5f,
                  "Tumbleweed stays outside player capsule radius");
        }
    }
    {
        // Roof shelter damping: under a roof accelerates noticeably slower than under open sky.
        const auto openRay = MakeWorldRay({});
        const auto roofRay = MakeWorldRay({{{-500, -500, 120}, {500, 500, 140}}});
        const TumbleweedWind wind{180.f, 0.f, 0.f};
        TumbleweedBody openBody, roofBody;
        openBody.active = roofBody.active = true;
        openBody.radius = roofBody.radius = 16.f;
        openBody.pos = roofBody.pos = {0, 0, 16.f * .92f};
        openBody.seed = roofBody.seed = 11u;
        openBody.hopCooldown = roofBody.hopCooldown = 10.f;
        uint32_t rng1 = 7u, rng2 = 7u;
        for (int i = 0; i < 60; ++i) {
            TumbleweedStepBody(openBody, 1.f / 60.f, double(i) / 60.0, wind, openRay, {}, rng1);
            TumbleweedStepBody(roofBody, 1.f / 60.f, double(i) / 60.0, wind, roofRay, {}, rng2);
        }
        Check(roofBody.pos.x < openBody.pos.x * .6f, "Overhead shelter dampens tumbleweed wind drive");
    }
    {
        // Full sim: the map's share is set out at once out of sight, the first one near the camera
        // comes a moment later, pause freezes, recycling waits until hidden, zero clears.
        const TumbleweedCamera cam = MakeLookPlusXCamera({0, 0, 64});
        const TumbleweedWind wind{140.f, 0.f, .5f};
        const auto flatRay = MakeWorldRay({});
        std::vector<Vec3> nav;
        for (int ix = -12; ix <= -5; ++ix)
            for (int iy = -4; iy <= 4; ++iy)
                nav.push_back({float(ix * 110), float(iy * 120), 0.f});
        for (int ix = -30; ix <= 30; ix += 3) // far out on the map, behind and beside the camera
            for (int iy : {-28, 28})
                nav.push_back({float(ix * 100), float(iy * 100), 0.f});
        const Vec3 lo{-4000, -4000, 0}, hi{4000, 4000, 500};

        auto nearAndAll = [&](const TumbleweedSim &s, int &nearCount, int &all) {
            nearCount = all = 0;
            for (const auto &b : s.bodies)
                if (b.active) { ++all; nearCount += TumbleweedNearCamera(cam, b) ? 1 : 0; }
        };
        TumbleweedSim sim;
        TumbleweedStepSim(sim, 10.0, 6, wind, cam, nav, lo, hi, flatRay, {});
        int nearCount = 0, all = 0;
        nearAndAll(sim, nearCount, all);
        Check(nearCount == 0 && all == 4, "The map's share (6 - 2 near) is set out at once, far from the camera");

        // Pause (same timestamp): every position and orientation stays byte-for-byte identical.
        const auto frozen = sim.bodies;
        TumbleweedStepSim(sim, 10.0, 6, wind, cam, nav, lo, hi, flatRay, {});
        for (int i = 0; i < kTumbleweedMax; ++i)
            Check(sim.bodies[size_t(i)].pos.x == frozen[size_t(i)].pos.x &&
                      sim.bodies[size_t(i)].orientation.w == frozen[size_t(i)].orientation.w,
                  "Paused demo freezes tumbleweeds completely");

        // Over a minute: never more than two near the camera, and arrivals spaced out.
        double t = 10.0;
        int mostNear = 0, arrivals = 0, lastNear = 0;
        double lastArrival = -100, closestGap = 1e9;
        for (int step = 0; step < 1800; ++step) {
            t += 1.0 / 30.0;
            TumbleweedStepSim(sim, t, 6, wind, cam, nav, lo, hi, flatRay, {});
            nearAndAll(sim, nearCount, all);
            Check(all <= 6, "Never more tumbleweeds than the slider asks for");
            mostNear = (std::max)(mostNear, nearCount);
            if (nearCount > lastNear) {
                ++arrivals;
                if (lastArrival > 0) closestGap = (std::min)(closestGap, t - lastArrival);
                lastArrival = t;
            }
            lastNear = nearCount;
        }
        Check(mostNear >= 1 && mostNear <= kTumbleweedNearMost, "At most two tumbleweeds near the camera at once");
        Check(arrivals >= 2, "Tumbleweeds keep arriving near the camera");
        Check(closestGap >= kTumbleweedCineGapLeast - .05, "Arrivals near the camera are spaced out, not bunched");

        // Out-of-view recycling: an expired body inside the camera's sight stays alive until out of view.
        sim.bodies[0].active = true;
        sim.bodies[0].pos = {600, 0, 16};
        sim.bodies[0].age = 999.f;
        sim.bodies[0].maxAge = 30.f;
        t += 1.0 / 30.0;
        TumbleweedStepSim(sim, t, 6, wind, cam, nav, lo, hi, flatRay, {});
        Check(sim.bodies[0].active, "Expired tumbleweed never vanishes while visible in the camera");
        sim.bodies[0].pos = {-900, 0, 16}; // move behind camera
        t += 1.0 / 30.0;
        TumbleweedStepSim(sim, t, 6, wind, cam, nav, lo, hi, flatRay, {});
        Check(!sim.bodies[0].active || sim.bodies[0].age < 1.f, "Expired tumbleweed recycles cleanly once out of view");

        // targetCount == 0 clears all bodies immediately.
        TumbleweedStepSim(sim, t + .1, 0, wind, cam, nav, lo, hi, flatRay, {});
        for (const auto &b : sim.bodies) Check(!b.active, "Setting tumbleweeds to zero clears all bodies");
    }
    {
        // First person: a new arrival starts hidden in front of the player and the wind carries it
        // into view. Wind blows across the view (+Y); a wall in front hides the spots behind it.
        TumbleweedCamera cam = MakeLookPlusXCamera({0, 0, 64});
        cam.pov = true;
        const TumbleweedWind wind{0.f, 150.f, .3f};
        const auto ray = MakeWorldRay({{{500, -700, 0}, {520, -60, 300}}}); // a wall ahead, left of centre
        std::array<TumbleweedBody, kTumbleweedMax> empty{};
        const std::vector<Vec3> spots = {
            {-800, 0, 0},    // behind the player: hidden, but not in front
            {800, -500, 0},  // in front, behind the wall, and the wind carries it out across the view
            {700, 900, 0},   // in front but off to the side, rolling away from the view
        };
        uint32_t rng = 7u;
        for (int trial = 0; trial < 10; ++trial) {
            Vec3 out{};
            Check(TumbleweedPickSpawn(spots, {-2000, -2000, 0}, {2000, 2000, 500}, cam, wind, ray, empty, rng, 16.f, out),
                  "First person finds a spot in front");
            Check(out.x > 0.f, "First person: tumbleweeds start in front of the player");
            Check(!TumbleweedVisibleToCamera(cam, out, 16.f, ray), "First person: the start is hidden");
            float along = 0.f;
            Check(TumbleweedComesIntoView(cam, out, 16.f, {0, 1, 0}, ray, along), "First person: the wind carries it into view");
        }
        // A cinematic camera may take the spot behind.
        cam.pov = false;
        const TumbleweedWind back{140.f, 0.f, .3f};
        Vec3 out{};
        Check(TumbleweedPickSpawn({{-800, 0, 0}}, {-2000, -2000, 0}, {2000, 2000, 500}, cam, back, MakeWorldRay({}), empty, rng, 16.f, out) && out.x < 0.f,
              "A cinematic camera takes a hidden spot on any side");
    }
    std::cout << "Tumbleweed math passed\n";
    return 0;
}
