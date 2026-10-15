#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockGoToMath.h"
#include <cmath>
#include <cstdio>
#include <limits>

// Deadlock camera editor's "Go To" action: place the free camera behind and above a chosen hero,
// looking at them. See DeadlockGoToMath.h for why the formulas look the way they do -- in short,
// Source right = yaw - 90, positive pitch looks down, and a degenerate zero/zero/zero request must
// not hand back atan2(0, 0)'s world-yaw-0 instead of the hero's own facing.
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* what) {
        if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
    };

    using Deadlock::GoToVec3;
    using Deadlock::GoToPose;
    using Deadlock::SolveGoToPose;
    using Deadlock::GoToWrapDegrees;
    using Deadlock::GoToPositionError;
    using Deadlock::GoToLookErrorDegrees;
    using Deadlock::kGoToDistanceMax;

    constexpr double kEps = 1e-6;
    const GoToVec3 eye{100.0, -200.0, 64.0};

    // --- Behind the hero, on the XY plane, for the four axis-aligned yaws. ---------------------
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 0.0, 0.0);
        check(pose.ok, "yaw 0: solves");
        check(pose.position.x < eye.x - kEps, "yaw 0 (faces +X): camera is behind on -X");
        check(std::abs(pose.position.y - eye.y) < 1e-6, "yaw 0: camera does not drift on Y");
    }
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, 90.0, 150.0, 0.0, 0.0);
        check(pose.ok, "yaw 90: solves");
        check(pose.position.y < eye.y - kEps, "yaw 90 (faces +Y): camera is behind on -Y");
        check(std::abs(pose.position.x - eye.x) < 1e-6, "yaw 90: camera does not drift on X");
    }
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, 180.0, 150.0, 0.0, 0.0);
        check(pose.ok, "yaw 180: solves");
        check(pose.position.x > eye.x + kEps, "yaw 180 (faces -X): camera is behind on +X");
    }
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, -90.0, 150.0, 0.0, 0.0);
        check(pose.ok, "yaw -90: solves");
        check(pose.position.y > eye.y + kEps, "yaw -90 (faces -Y): camera is behind on +Y");
    }

    // --- Above the hero means positive pitch (positive pitch looks down). ----------------------
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 60.0, 0.0);
        check(pose.ok, "up 60: solves");
        check(pose.position.z > eye.z + kEps, "camera is above the eye");
        check(pose.pitch > 0.0, "camera above the hero pitches down (positive pitch)");
    }
    {
        // Same up height, but the hero is further away: the ANGLE down should shrink, not the sign.
        const GoToPose closePose = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 50.0, 60.0, 0.0);
        const GoToPose farPose = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 1000.0, 60.0, 0.0);
        check(closePose.pitch > farPose.pitch && farPose.pitch > 0.0, "further back at the same height looks down less steeply");
    }

    // --- The look direction always points at the eye, well within 0.1 degrees. -----------------
    const double backs[] = {0.0, 25.0, 150.0, 900.0};
    const double ups[] = {0.0, 10.0, 60.0, 500.0};
    const double laterals[] = {-80.0, 0.0, 40.0};
    const double yaws[] = {0.0, 37.5, 90.0, 145.0, 180.0, -180.0, -90.0, -12.0};
    for (double yaw : yaws) {
        for (double back : backs) {
            for (double up : ups) {
                for (double lateral : laterals) {
                    const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, yaw, back, up, lateral);
                    check(pose.ok, "sweep: solves for every combination");
                    check(std::isfinite(pose.position.x) && std::isfinite(pose.position.y) &&
                          std::isfinite(pose.position.z) && std::isfinite(pose.pitch) &&
                          std::isfinite(pose.yaw), "sweep: never produces NaN/inf");
                    const double lookError = GoToLookErrorDegrees(pose, eye);
                    check(lookError < 0.1, "sweep: look direction points at the eye within 0.1 degrees");
                    check(pose.roll == 0.0, "sweep: roll is always 0");
                    // The solver is deterministic: re-solving the same inputs lands on the same spot.
                    check(GoToPositionError(pose, eye.x, eye.y, eye.z, yaw, back, up, lateral) < kEps,
                          "sweep: position matches re-solving the same request");
                }
            }
        }
    }

    // --- Yaw wrap at +-180 is correct. -----------------------------------------------------------
    auto angleDiff = [](double a, double b) { return std::abs(GoToWrapDegrees(a - b)); };
    check(angleDiff(GoToWrapDegrees(370.0), 10.0) < kEps, "wrap: 370 -> 10");
    check(angleDiff(GoToWrapDegrees(-370.0), -10.0) < kEps, "wrap: -370 -> -10");
    check(angleDiff(GoToWrapDegrees(540.0), 180.0) < kEps, "wrap: 540 -> +-180");
    check(angleDiff(GoToWrapDegrees(-540.0), 180.0) < kEps, "wrap: -540 -> +-180");
    // The seam lands on +180 from either side, exactly: (-180, 180] excludes -180.
    check(GoToWrapDegrees(180.0) == 180.0, "wrap: 180 stays 180");
    check(GoToWrapDegrees(-180.0) == 180.0, "wrap: -180 becomes 180");
    check(GoToWrapDegrees(540.0) == 180.0 && GoToWrapDegrees(-540.0) == 180.0, "wrap: +-540 land on 180");
    check(GoToWrapDegrees(-190.0) > 169.999 && GoToWrapDegrees(-190.0) < 170.001, "wrap: -190 -> 170");
    {
        // Both a hero facing exactly +180 and one facing exactly -180 are the same direction; the
        // resulting look yaw must land on that same direction either way, with no glitch.
        const GoToPose p180 = SolveGoToPose(eye.x, eye.y, eye.z, 180.0, 150.0, 0.0, 0.0);
        const GoToPose pNeg180 = SolveGoToPose(eye.x, eye.y, eye.z, -180.0, 150.0, 0.0, 0.0);
        check(angleDiff(p180.yaw, 180.0) < 1e-4, "yaw 180: look yaw wraps to +-180 cleanly");
        check(angleDiff(pNeg180.yaw, 180.0) < 1e-4, "yaw -180: look yaw wraps to +-180 cleanly");
        check(angleDiff(p180.position.x, pNeg180.position.x) < kEps,
              "yaw 180 and -180 place the camera at the same spot");
    }

    // --- Degenerate inputs: no NaN, and a graceful fallback to the hero's own facing. -----------
    {
        const GoToPose pose = SolveGoToPose(eye.x, eye.y, eye.z, 37.0, 0.0, 0.0, 0.0);
        check(pose.ok, "back=0, up=0, lateral=0: still reports ok");
        check(std::isfinite(pose.pitch) && std::isfinite(pose.yaw), "degenerate case: angles are finite, not NaN");
        check(std::abs(pose.position.x - eye.x) < kEps && std::abs(pose.position.y - eye.y) < kEps &&
              std::abs(pose.position.z - eye.z) < kEps, "degenerate case: camera sits on the eye");
        check(pose.pitch == 0.0, "degenerate case: falls back to a level pitch");
        check(angleDiff(pose.yaw, 37.0) < kEps, "degenerate case: falls back to looking along the hero's own yaw");
    }
    {
        // Non-finite inputs fail closed rather than propagating NaN into a camera write.
        const double nan = std::nan("");
        const GoToPose pose = SolveGoToPose(nan, eye.y, eye.z, 0.0, 150.0, 60.0, 0.0);
        check(!pose.ok, "NaN eye position: fails closed");
        check(pose.position.x == 0.0 && pose.position.y == 0.0 && pose.position.z == 0.0 &&
              pose.pitch == 0.0 && pose.yaw == 0.0, "NaN eye position: output is zeroed, not NaN");
        const GoToPose badYaw = SolveGoToPose(eye.x, eye.y, eye.z, nan, 150.0, 60.0, 0.0);
        check(!badYaw.ok, "NaN hero yaw: fails closed");
        const double inf = std::numeric_limits<double>::infinity();
        const GoToPose infBack = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, inf, 60.0, 0.0);
        check(!infBack.ok, "infinite back distance: fails closed, not silently clamped");
    }

    // --- Negative and huge distances clamp instead of misbehaving. ------------------------------
    {
        const GoToPose negBack = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, -50.0, 0.0, 0.0);
        const GoToPose zeroBack = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 0.0, 0.0, 0.0);
        check(negBack.ok && std::abs(negBack.position.x - zeroBack.position.x) < kEps,
              "negative back distance clamps to 0, same as an explicit 0");
        const GoToPose huge = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 1e9, 0.0, 0.0);
        const GoToPose atMax = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, kGoToDistanceMax, 0.0, 0.0);
        check(huge.ok && std::isfinite(huge.position.x), "an absurd back distance still produces a finite pose");
        check(std::abs(huge.position.x - atMax.position.x) < kEps, "an absurd back distance clamps to the documented max");
    }

    // --- Lateral offset moves the camera along the hero's right vector (right = yaw - 90). ------
    {
        // Facing +X (yaw 0): right = (sin 0, -cos 0, 0) = (0, -1, 0) -- lateral moves purely on Y.
        const GoToPose base = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 0.0, 0.0);
        const GoToPose right = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 0.0, 50.0);
        check(std::abs(right.position.x - base.position.x) < kEps, "yaw 0: lateral offset does not move X");
        check(right.position.y < base.position.y - kEps, "yaw 0: positive lateral moves along (0,-1,0)");
    }
    {
        // Facing +Y (yaw 90): right = (sin 90, -cos 90, 0) = (1, ~0, 0) -- lateral moves purely on X.
        const GoToPose base = SolveGoToPose(eye.x, eye.y, eye.z, 90.0, 150.0, 0.0, 0.0);
        const GoToPose right = SolveGoToPose(eye.x, eye.y, eye.z, 90.0, 150.0, 0.0, 50.0);
        check(std::abs(right.position.y - base.position.y) < kEps, "yaw 90: lateral offset does not move Y");
        check(right.position.x > base.position.x + kEps, "yaw 90: positive lateral moves along (1,0,0)");
    }
    {
        // A negative lateral offset moves the opposite way (the hero's left).
        const GoToPose base = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 0.0, 0.0);
        const GoToPose left = SolveGoToPose(eye.x, eye.y, eye.z, 0.0, 150.0, 0.0, -50.0);
        check(left.position.y > base.position.y + kEps, "yaw 0: negative lateral moves the other way");
    }

    std::printf("Deadlock go-to math: %d failures\n", failures);
    return failures ? 1 : 0;
}
