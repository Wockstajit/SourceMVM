#pragma once

// The dolly zoom ("Vertigo" shot) as camera path keys: the camera travels along its view toward or
// away from a subject while the field of view changes so the subject keeps the same size on screen,
// and only the background stretches or squeezes. `mirv_filmmaker marker dollyzoom` writes the keys
// (MarkerCommand.cpp); tested by Automation/tests/dolly-zoom-math-tests.cpp.
//
// The subject's width on screen is w = 2 d tan(fov / 2) at distance d, so holding it while d changes
// means tan(fov(t) / 2) = tan(fov0 / 2) * d0 / d(t). CS2's fov is the 4:3 horizontal angle; the real
// angle's tangent is that one times a constant (the aspect over 4:3), so the same rule holds on it.
//
// Pushing in widens the view (the background rushes away); pulling out narrows it (the background
// closes in). Keys sit every 1/keysPerSecond seconds, because the path interpolates each channel on
// its own and fov is not linear in the distance; a dense set keeps the subject's size steady between
// them.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Filmmaker::DollyZoom {

struct Pose {
	double x = 0, y = 0, z = 0;
	double pitch = 0, yaw = 0, roll = 0; // degrees
	double fov = 90;                     // CS2's fov
};

struct Key { int tick = 0; Pose pose; };

struct Options {
	double subjectDistance = 300; // units from the camera to what keeps its size
	double move = 150;            // units toward the subject (negative: away from it)
	double seconds = 3;
	double tickRate = 64;
	int startTick = 0;
	int keysPerSecond = 16;
	bool ease = true;             // slow in and out, as a dolly on a track
};

constexpr double kMinFov = 5, kMaxFov = 160, kMinDistance = 12;
constexpr double kPi = 3.14159265358979323846;

inline double Ease(double t, bool on) {
	t = (std::max)(0.0, (std::min)(1.0, t));
	return on ? t * t * (3 - 2 * t) : t;
}

// The fov that keeps a subject at `d0` seen with `fov0` the same size from `d`.
inline double FovAt(double fov0, double d0, double d) {
	const double t = std::tan(fov0 * kPi / 360.0) * d0 / (std::max)(d, 1e-6);
	return std::atan(t) * 360.0 / kPi;
}

// The keys of one dolly zoom from `start`. Empty with `why` set when it cannot be done: a move that
// would pass through the subject, or a view narrower or wider than CS2 can draw. The fov limits
// shorten the move rather than refuse it, and `why` then says so.
inline std::vector<Key> Build(const Pose& start, const Options& o, std::string* why = nullptr) {
	std::vector<Key> keys;
	auto fail = [&](const char* w) { if (why) *why = w; return std::vector<Key>{}; };
	if (!(o.subjectDistance > kMinDistance)) return fail("the subject is too close to the camera");
	if (!(o.seconds > 0.05) || !(o.tickRate > 0)) return fail("the shot needs a length");
	if (!(start.fov > kMinFov && start.fov < kMaxFov)) return fail("the starting field of view is outside 5 to 160");
	double move = o.move;
	const double d0 = o.subjectDistance;
	if (d0 - move < kMinDistance) return fail("the camera would reach the subject; move less or start farther away");
	// The widest and narrowest view CS2 draws bound the end distance.
	const double tan0 = std::tan(start.fov * kPi / 360.0);
	const double dNear = d0 * tan0 / std::tan(kMaxFov * kPi / 360.0); // pushing in to here hits 160
	const double dFar = d0 * tan0 / std::tan(kMinFov * kPi / 360.0);  // pulling out to here hits 5
	double d1 = d0 - move;
	bool clipped = false;
	if (d1 < dNear) { d1 = dNear; clipped = true; }
	if (d1 > dFar) { d1 = dFar; clipped = true; }
	move = d0 - d1;
	if (std::fabs(move) < 1) return fail("the move is too small to see");
	if (why) *why = clipped ? "shortened: the full move needs a field of view CS2 cannot draw" : "";

	const double p = start.pitch * kPi / 180, y = start.yaw * kPi / 180;
	const double fwd[3] = { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) };
	const int perSecond = (std::max)(2, o.keysPerSecond);
	const int n = (std::max)(2, (int)std::ceil(o.seconds * perSecond) + 1);
	const int totalTicks = (std::max)(1, (int)std::lround(o.seconds * o.tickRate));
	int lastTick = o.startTick - 1;
	for (int i = 0; i < n; ++i) {
		const double t = (double)i / (n - 1);
		const int tick = o.startTick + (int)std::lround(t * totalTicks);
		if (tick <= lastTick) continue; // two keys never share a tick
		lastTick = tick;
		const double travelled = move * Ease(t, o.ease);
		Key k;
		k.tick = tick;
		k.pose = start;
		k.pose.x = start.x + fwd[0] * travelled;
		k.pose.y = start.y + fwd[1] * travelled;
		k.pose.z = start.z + fwd[2] * travelled;
		k.pose.fov = FovAt(start.fov, d0, d0 - travelled);
		keys.push_back(k);
	}
	return keys;
}

// How well a point suits as the subject: its distance along the view, or a negative value when it
// is behind the camera or more than `coneDeg` off the middle of the frame.
inline double SubjectDistance(const Pose& cam, const double point[3], double coneDeg = 20) {
	const double p = cam.pitch * kPi / 180, y = cam.yaw * kPi / 180;
	const double fwd[3] = { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) };
	const double d[3] = { point[0] - cam.x, point[1] - cam.y, point[2] - cam.z };
	const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len < 1e-6) return -1;
	const double along = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
	if (along <= 0 || along / len < std::cos(coneDeg * kPi / 180)) return -1;
	return along;
}

} // namespace Filmmaker::DollyZoom
