#include "../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-6) {
	return std::fabs(a - b) <= epsilon;
}
}

int main() {
	// One second of playback must produce one second of filtering at any render rate.
	for (const int fps : {60, 144, 240}) {
		for (const double speed : {1.0, 0.25}) {
			FollowRenderClock clock;
			FollowAdvanceRenderClock(clock, 0, 100.0, 0.0, false);
			double elapsed = 0.0;
			FollowVec3 filtered{};
			for (int frame = 1; frame <= fps; ++frame) {
				const auto delta = FollowAdvanceRenderClock(clock, frame, 100.0 + frame * speed / fps, 1.0 / fps, false);
				elapsed += delta.smoothing;
				filtered = FollowSmoothPosition(filtered, {10,0,0}, delta.smoothing, 0.1);
				const auto repeat = FollowAdvanceRenderClock(clock, frame, 100.0 + frame * speed / fps, 1.0 / fps, false);
				Check(!repeat.newFrame && Near(repeat.smoothing, 0), "duplicate view pass cannot advance Lock-on");
			}
			Check(Near(elapsed, speed), "Lock-on clock follows playback, independent of FPS");
			Check(Near(filtered.x, 10 * FollowHalfTimeAlpha(0.1, speed)), "Lock-on smoothing is frame-rate independent");
		}
	}
	FollowRenderClock renderClock;
	FollowAdvanceRenderClock(renderClock, 0, 100, 0.0, false);
	const auto frozenFrame = FollowAdvanceRenderClock(renderClock, 1, 100, 0.01, false);
	Check(Near(frozenFrame.smoothing, 0), "repeated playback time cannot double-count wall time");
	const auto pausedEdit = FollowAdvanceRenderClock(renderClock, 2, 100, 0.01, true);
	Check(Near(pausedEdit.smoothing, 0.01) && Near(pausedEdit.playback, 0), "paused edits settle without predicted motion");
	FollowAdvanceRenderClock(renderClock, 3, 99.995, 0.01, false);
	Check(Near(FollowAdvanceRenderClock(renderClock, 4, 100.01, 0.01, false).playback, 0.01), "pause-boundary rewind does not count time twice");
	Check(Near(FollowAdvanceRenderClock(renderClock, 5, 200, 0.01, false).playback, 0), "seek jump is not smoothing time");
	Check(Near(FollowAdvanceRenderClock(renderClock, 6, 200.01, 0.01, false).playback, 0.01), "render clock recovers after seek");
	// A captured camera must retain its complete local frame as a pitched/rolled bone turns.
	// This covers stairs, tilted hand bones, crossing +/-180, and vertical head poses.
	const FollowAngles capturedWorld{31, -147, 26}, captureBase{-23, 114, 78};
	const auto relative = FollowRelativeRotation(capturedWorld, captureBase);
	const auto restoredCamera = FollowComposeRotation(captureBase, relative);
	for (const FollowVec3 axis : {FollowVec3{1,0,0}, FollowVec3{0,0,1}})
		Check(FollowDistance(FollowRotateVector(axis, restoredCamera), FollowRotateVector(axis, capturedWorld)) < 1e-6,
			"capture preserves camera forward and roll on a tilted mount");
	for (const FollowAngles base : {captureBase, FollowAngles{60, 179, 160}, FollowAngles{-90, 45, 17}, FollowAngles{90, -120, -81}, FollowAngles{-15,-179,-175}, captureBase}) {
		const auto moved = FollowComposeRotation(base, relative);
		const auto recovered = FollowRelativeRotation(moved, base);
		const auto offset = FollowRotateVector({-24,12,8}, base);
		Check(FollowDistance(FollowInverseRotateVector(offset, base), {-24,12,8}) < 1e-6, "mount position survives turn and return");
		for (const FollowVec3 axis : {FollowVec3{1,0,0}, FollowVec3{0,0,1}}) {
			Check(FollowDistance(FollowRotateVector(axis, recovered), FollowRotateVector(axis, relative)) < 1e-6,
				"mount rotation survives turn and return without drifting");
			Check(FollowDistance(FollowRotateVector(axis, FollowComposeRotation(base, {})), FollowRotateVector(axis, base)) < 1e-6,
				"zero rotation trim preserves the mount frame including vertical poses");
		}
	}
	Check(Near(FollowPlaybackDelta(100, 100, 1.0 / 64), 0), "paused frames consume no hold time");
	Check(!FollowTickDiscontinuity(100, 99) && Near(FollowPlaybackDelta(100, 99, 1.0 / 64), 0), "pause interpolation rounding keeps the window");
	Check(Near(FollowPlaybackDelta(100, 101, 1.0 / 64), 1.0 / 64), "one tick consumes playback time");
	Check(Near(FollowPlaybackDelta(100, 104, 1.0 / 64), 4.0 / 64), "skipped render frames retain elapsed time");
	Check(Near(FollowPlaybackDelta(-1, 100, 1.0 / 64), 0), "first sample starts a fresh hold");
	Check(FollowTickDiscontinuity(100, 50) && Near(FollowPlaybackDelta(100, 50, 1.0 / 64), 0), "backward seek resets the hold");
	Check(FollowTickDiscontinuity(100, 1000) && Near(FollowPlaybackDelta(100, 1000, 1.0 / 64), 0), "forward seek is not playback");
	double hold = 0.0;
	for (int tick = 0; tick < 192; ++tick) {
		for (int pausedFrame = 0; pausedFrame < 10; ++pausedFrame)
			hold += FollowPlaybackDelta(tick, tick, 1.0 / 64);
		hold += FollowPlaybackDelta(tick, tick + 1, 1.0 / 64);
	}
	Check(Near(hold, 3.0), "3 second hold is independent of slow motion or paused frames");
	for (const FollowAngles pose : {FollowAngles{22, -173, 67}, FollowAngles{-71, 83, -110}, FollowAngles{0, 0, 0}}) {
		const auto forward = FollowRotateVector({1,0,0}, pose);
		const auto up = FollowRotateVector({0,0,1}, pose);
		const auto restored = FollowAnglesFromBasis(forward, up);
		Check(FollowDistance(forward, FollowRotateVector({1,0,0}, restored)) < 1e-6, "ragdoll mount keeps forward direction");
		Check(FollowDistance(up, FollowRotateVector({0,0,1}, restored)) < 1e-6, "ragdoll mount keeps roll through tilted poses");
	}
	Check(Near(FollowWrapDegrees(190.0), -170.0), "wrap +190");
	Check(Near(FollowWrapDegrees(-190.0), 170.0), "wrap -190");
	const FollowAngles east = FollowLookAt({0, 0, 0}, {100, 0, 0});
	Check(Near(east.pitch, 0.0) && Near(east.yaw, 0.0), "look east");
	const FollowAngles north = FollowLookAt({0, 0, 0}, {0, 100, 0});
	Check(Near(north.yaw, 90.0), "look north");
	const FollowAngles up = FollowLookAt({0, 0, 0}, {0, 0, 100});
	Check(Near(up.pitch, -90.0), "look up");
	const FollowAngles mounted = FollowAddAngles({10, 170, -175}, {5, 25, -20});
	Check(Near(mounted.pitch, 15.0) && Near(mounted.yaw, -165.0) && Near(mounted.roll, 165.0),
		"rigid attach angle trim wraps");
	const FollowVec3 nearVerticalCamera{100, 20, 68};
	const FollowVec3 nearVerticalTarget{100.001, 20, 60};
	const FollowAngles singularLook = FollowLookAt(nearVerticalCamera, nearVerticalTarget);
	const FollowAngles rigidNearVertical = FollowAddAngles({12, 45, 3}, {0, 10, 0});
	Check(std::fabs(singularLook.pitch) > 85.0 && Near(rigidNearVertical.yaw, 55.0),
		"rigid attach does not derive yaw from near-vertical look-at");

	Check(Near(FollowHalfTimeAlpha(1.0, 1.0), 0.5), "half-time alpha");
	const FollowAngles halfway = FollowSmoothAngles({0, 0, 0}, {0, 90, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(halfway.yaw, 45.0), "half-time angle smoothing");

	const FollowAngles wrapped = FollowSmoothAngles({0, 170, 0}, {0, -170, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(wrapped.yaw, 180.0), "shortest wrapped arc");

	const FollowAngles deadzone = FollowSmoothAngles({2, 3, 0}, {2.2, 3.2, 0}, 1.0, 0.1, 1.0, 0.0);
	Check(Near(deadzone.pitch, 2.0) && Near(deadzone.yaw, 3.0), "deadzone");

	const FollowAngles limited = FollowSmoothAngles({0, 0, 0}, {0, 180, 0}, 0.1, 0.0, 0.0, 30.0);
	Check(Near(std::fabs(limited.yaw), 3.0), "max turn speed");

	const FollowVec3 p = FollowSmoothPosition({0, 0, 0}, {10, 20, 30}, 1.0, 1.0);
	Check(Near(p.x, 5.0) && Near(p.y, 10.0) && Near(p.z, 15.0), "position smoothing");
	Check(Near(FollowDistance({0, 0, 0}, {3, 4, 0}), 5.0), "distance");

	const FollowVec3 front = FollowRotateVector({72, 0, 8}, {0, 0, 0});
	Check(Near(front.x, 72.0) && Near(front.y, 0.0) && Near(front.z, 8.0), "local front offset");
	const FollowVec3 yawedFront = FollowRotateVector({72, 0, 0}, {0, 90, 0});
	Check(Near(yawedFront.x, 0.0, 1e-5) && Near(yawedFront.y, 72.0, 1e-5), "local offset follows yaw");
	const FollowVec3 localMount{32, -12, 7};
	const FollowAngles mountAngles{15, 70, -20};
	const FollowVec3 worldMount = FollowRotateVector(localMount, mountAngles);
	const FollowVec3 recoveredMount = FollowInverseRotateVector(worldMount, mountAngles);
	Check(Near(recoveredMount.x, localMount.x, 1e-5)
		&& Near(recoveredMount.y, localMount.y, 1e-5)
		&& Near(recoveredMount.z, localMount.z, 1e-5),
		"local attach offset round-trips through inverse rotate");
	const FollowVec3 steppedOffset = FollowRotateVector({40, 0, 0}, {0, 0, 0});
	const FollowVec3 localRebase = FollowInverseRotateVector(steppedOffset, {0, 0, 0});
	const FollowVec3 freshOffset = FollowRotateVector(localRebase, {0, 90, 0});
	Check(Near(freshOffset.x, 0.0, 1e-5) && Near(freshOffset.y, 40.0, 1e-5),
		"render-time attach rebase rotates local offset with fresh mount orientation");
	const FollowVec3 target{100, 20, 60};
	const FollowVec3 camera{172, 20, 68};
	const FollowAngles backAtTarget = FollowLookAt(camera, target);
	Check(Near(backAtTarget.yaw, 180.0) && backAtTarget.pitch > 0.0, "front camera looks back at target");

	const FollowVec3 frozen = FollowSmoothPosition({10, 20, 30}, {100, 200, 300}, 0.0, 0.25);
	Check(Near(frozen.x, 10.0) && Near(frozen.y, 20.0) && Near(frozen.z, 30.0), "zero-dt position hold");
	const FollowAngles frozenAngles = FollowSmoothAngles({4, 5, 6}, {40, 50, 60}, 0.0, 0.25, 0.0, 0.0);
	Check(Near(frozenAngles.pitch, 4.0) && Near(frozenAngles.yaw, 5.0), "zero-dt angle hold");

	// A grenade thrown at 600 u/s forward and 300 up, falling at CS:S's 0.4 of sv_gravity 800.
	const FollowVec3 launch{600, 0, 300};
	Check(Near(FollowGrenadeFlightSeconds(launch, launch, 320.0), 0.0), "grenade just thrown");
	Check(Near(FollowGrenadeFlightSeconds(launch, {600, 0, -20}, 320.0), 1.0), "grenade a second into its flight");
	Check(FollowGrenadeFlightSeconds(launch, {270, 0, 50}, 320.0) < 0.0, "a bounce changed its sideways speed");
	Check(FollowGrenadeFlightSeconds(launch, {600, 0, 400}, 320.0) < 0.0, "rising faster than thrown is no flight");
	Check(FollowGrenadeFlightSeconds(launch, {600, 0, 0}, 0.0) < 0.0, "no gravity, no clock");

	std::cout << "FollowCameraMathTests: all checks passed\n";
	return 0;
}
