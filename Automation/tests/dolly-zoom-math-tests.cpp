// Behavior tests for Movie/DollyZoomMath.h: the dolly zoom keys hold the subject's size on screen.
#include "../../AfxHookSource2/Filmmaker/Movie/DollyZoomMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::DollyZoom;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

Pose LookingAlongX() { Pose p; p.x = 100; p.y = 50; p.z = 64; p.pitch = 0; p.yaw = 0; p.fov = 60; return p; }

// The subject's size on screen: its width over the frame's width at its distance.
double ScreenShare(const Pose& cam, const double subject[3], double width = 32) {
	const double d = std::sqrt((subject[0] - cam.x) * (subject[0] - cam.x) + (subject[1] - cam.y) * (subject[1] - cam.y) + (subject[2] - cam.z) * (subject[2] - cam.z));
	return width / (2 * d * std::tan(cam.fov * kPi / 360));
}

void HoldsTheSubjectsSize() {
	const Pose start = LookingAlongX();
	const double subject[3] = { start.x + 400, start.y, start.z };
	Options o; o.subjectDistance = 400; o.move = 250; o.seconds = 3; o.tickRate = 64; o.startTick = 1000;
	std::string why;
	const auto keys = Build(start, o, &why);
	CHECK(keys.size() >= 40);
	CHECK(keys.front().tick == 1000 && keys.back().tick == 1000 + 192);
	const double share0 = ScreenShare(keys.front().pose, subject);
	for (const Key& k : keys) CHECK(Near(ScreenShare(k.pose, subject), share0, 1e-9));
	// Pushing in widens the view; the camera ends `move` units closer along its view.
	CHECK(keys.back().pose.fov > keys.front().pose.fov);
	CHECK(Near(keys.back().pose.x, start.x + 250, 1e-6) && Near(keys.back().pose.y, start.y, 1e-9));
	for (size_t i = 1; i < keys.size(); ++i) CHECK(keys[i].tick > keys[i - 1].tick);
	CHECK(why.empty());
}

void PullingOutNarrows() {
	const Pose start = LookingAlongX();
	Options o; o.subjectDistance = 300; o.move = -300;
	const auto keys = Build(start, o);
	CHECK(!keys.empty() && keys.back().pose.fov < start.fov);
	CHECK(Near(keys.back().pose.x, start.x - 300, 1e-6));
}

void KeepsToWhatCs2CanDraw() {
	Pose start = LookingAlongX(); start.fov = 120;
	Options o; o.subjectDistance = 300; o.move = 280;
	std::string why;
	const auto keys = Build(start, o, &why);
	CHECK(!keys.empty() && !why.empty());
	for (const Key& k : keys) CHECK(k.pose.fov <= kMaxFov + 1e-9);
	// Through the subject is refused.
	o.move = 400;
	CHECK(Build(start, o, &why).empty());
	o.move = 100; o.subjectDistance = 5;
	CHECK(Build(start, o, &why).empty());
}

void FollowsAPitchedView() {
	Pose start = LookingAlongX(); start.pitch = 30; start.yaw = 90;
	Options o; o.subjectDistance = 500; o.move = 200; o.ease = false;
	const auto keys = Build(start, o);
	const Pose& end = keys.back().pose;
	// Looking 30 degrees down along +y: the camera drops as it travels.
	CHECK(Near(end.x, start.x, 1e-6) && end.y > start.y && end.z < start.z);
	CHECK(Near(std::sqrt((end.x - start.x) * (end.x - start.x) + (end.y - start.y) * (end.y - start.y) + (end.z - start.z) * (end.z - start.z)), 200, 1e-6));
	// Linear: halfway in time is halfway along.
	const Key& mid = keys[keys.size() / 2];
	const double t = (double)(mid.tick - keys.front().tick) / (keys.back().tick - keys.front().tick);
	CHECK(Near((mid.pose.y - start.y) / (end.y - start.y), t, 1e-6));
}

void PicksTheSubjectAhead() {
	const Pose cam = LookingAlongX();
	const double ahead[3] = { cam.x + 300, cam.y + 20, cam.z };
	const double behind[3] = { cam.x - 300, cam.y, cam.z };
	const double aside[3] = { cam.x + 100, cam.y + 300, cam.z };
	CHECK(SubjectDistance(cam, ahead) > 290);
	CHECK(SubjectDistance(cam, behind) < 0);
	CHECK(SubjectDistance(cam, aside) < 0);
}

} // namespace

int main() {
	HoldsTheSubjectsSize();
	PullingOutNarrows();
	KeepsToWhatCs2CanDraw();
	FollowsAPitchedView();
	PicksTheSubjectAhead();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "DollyZoomMathTests: all passed\n";
	return 0;
}
