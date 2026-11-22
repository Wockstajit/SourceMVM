// Behavior tests for Render/CameraExportMath.h: the rendered camera rebuilt as an After Effects camera.
#include "../../AfxHookSource2/Filmmaker/Render/CameraExportMath.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace Filmmaker::CameraExport;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void Column(const Mat3& m, int c, double out[3]) { for (int r = 0; r < 3; ++r) out[r] = m.m[r][c]; }

bool SameVec(const double a[3], const double b[3], double eps = 1e-6) {
	return Near(a[0], b[0], eps) && Near(a[1], b[1], eps) && Near(a[2], b[2], eps);
}

// Where a Source point lands on screen for a Source camera: x right, y down, in pixels.
bool ProjectSource(const Frame& f, int w, int h, const double p[3], double out[2]) {
	const Mat3 R = SourceRotation(f.angles[0], f.angles[1], f.angles[2]);
	const double d[3] = { p[0] - f.origin[0], p[1] - f.origin[1], p[2] - f.origin[2] };
	double fwd = 0, left = 0, up = 0;
	for (int r = 0; r < 3; ++r) { fwd += R.m[r][0] * d[r]; left += R.m[r][1] * d[r]; up += R.m[r][2] * d[r]; }
	if (fwd <= 0) return false;
	const double focal = (w * 0.5) / std::tan(Rad(HorizontalFovDeg(f.fov, w, h)) * 0.5);
	out[0] = w * 0.5 + focal * (-left / fwd);
	out[1] = h * 0.5 + focal * (-up / fwd);
	return true;
}

// The same point through the AE camera the export builds (AE's own projection: position, the
// Orientation matrix, zoom).
bool ProjectAe(const AeKey& k, int w, int h, const double aePoint[3], double out[2]) {
	const double e[3] = { Rad(k.orient[0]), Rad(k.orient[1]), Rad(k.orient[2]) };
	const Mat3 R = Mul(RotY(Rad(k.heading)), FromEulerXyz(e)); // the rig null, then the camera inside it
	const double d[3] = { aePoint[0] - k.pos[0], aePoint[1] - k.pos[1], aePoint[2] - k.pos[2] };
	double x = 0, y = 0, z = 0;
	for (int r = 0; r < 3; ++r) { x += R.m[r][0] * d[r]; y += R.m[r][1] * d[r]; z += R.m[r][2] * d[r]; }
	if (z <= 0) return false;
	out[0] = w * 0.5 + k.zoom * x / z;
	out[1] = h * 0.5 + k.zoom * y / z;
	return true;
}

void FovWidensWithTheAspect() {
	CHECK(Near(HorizontalFovDeg(90, 1440, 1080), 90.0));
	CHECK(Near(HorizontalFovDeg(90, 1920, 1080), 106.260205, 1e-5));
	Take t; t.width = 1440; t.height = 1080; t.frames.resize(1);
	CHECK(Near(AeKeys(t)[0].zoom, 720.0, 1e-6)); // 90 degrees across 1440 px: focal = half the width
}

void CameraAxesFollowSource() {
	// The AE camera's +z is the Source forward, +x the right, +y the down, all mapped through W.
	for (double pitch : { -60.0, 0.0, 35.0 })
		for (double yaw : { -170.0, 0.0, 90.0, 135.0 })
			for (double roll : { -20.0, 0.0, 15.0 }) {
				Frame f; f.angles[0] = pitch; f.angles[1] = yaw; f.angles[2] = roll;
				const Mat3 S = SourceRotation(pitch, yaw, roll), A = AeCameraMatrix(f);
				double fwd[3], left[3], up[3], ax[3], ay[3], az[3];
				Column(S, 0, fwd); Column(S, 1, left); Column(S, 2, up);
				Column(A, 0, ax); Column(A, 1, ay); Column(A, 2, az);
				double want[3];
				AePoint(fwd, want); CHECK(SameVec(az, want));
				const double right[3] = { -left[0], -left[1], -left[2] }, down[3] = { -up[0], -up[1], -up[2] };
				AePoint(right, want); CHECK(SameVec(ax, want));
				AePoint(down, want); CHECK(SameVec(ay, want));
				double e[3];
				EulerXyz(A, e);
				const Mat3 back = FromEulerXyz(e);
				for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) CHECK(Near(back.m[r][c], A.m[r][c]));
			}
}

void PointsLandWhereCs2DrewThem() {
	// A world point seen by the Source camera and by the exported AE camera hits the same pixel.
	Take t; t.width = 1920; t.height = 1080;
	Frame f;
	f.origin[0] = 1200; f.origin[1] = -340; f.origin[2] = 96;
	f.angles[0] = 12; f.angles[1] = 140; f.angles[2] = 6; f.fov = 70;
	t.frames.push_back(f);
	for (bool recentre : { false, true }) {
		t.options.recentre = recentre;
		t.options.scale = recentre ? 0.5 : 1.0;
		const AeKey k = AeKeys(t)[0];
		if (recentre) CHECK(Near(k.pos[0], 0) && Near(k.pos[1], 0) && Near(k.pos[2], 0));
		const double points[][3] = { { 900, -100, 64 }, { 1000, -500, 200 }, { 700, -300, 0 } };
		for (const auto& p : points) {
			double s[2], a[2], ae[3], base[3] = { 0, 0, 0 };
			AePoint(p, ae);
			if (recentre) AePoint(f.origin, base);
			for (int i = 0; i < 3; ++i) ae[i] = (ae[i] - base[i]) * t.options.scale;
			const bool sv = ProjectSource(f, t.width, t.height, p, s);
			const bool av = ProjectAe(k, t.width, t.height, ae, a);
			CHECK(sv && av);
			CHECK(Near(s[0], a[0], 1e-6) && Near(s[1], a[1], 1e-6));
		}
	}
}

void TurnsKeySmoothly() {
	// A pan through yaw 180 and a look straight down must not key a jump.
	Take t;
	for (int i = 0; i <= 40; ++i) {
		Frame f; f.angles[1] = 160 + i; f.angles[0] = i * 2.2; t.frames.push_back(f);
	}
	const auto keys = AeKeys(t);
	for (size_t i = 1; i < keys.size(); ++i) {
		CHECK(std::fabs(keys[i].heading - keys[i - 1].heading) < 10.0);
		for (int a = 0; a < 3; ++a) CHECK(std::fabs(keys[i].orient[a] - keys[i - 1].orient[a]) < 10.0);
	}
	// A level pan is all heading: the camera's own Orientation stays still, far from AE's lock at Y 90.
	Take pan;
	for (int i = 0; i < 720; i += 10) { Frame f; f.angles[1] = i; pan.frames.push_back(f); }
	const auto pk = AeKeys(pan);
	for (size_t i = 0; i < pk.size(); ++i)
		for (int a = 0; a < 3; ++a) CHECK(std::fabs(pk[i].orient[a]) < 1e-6);
	CHECK(Near(pk.back().heading - pk.front().heading, -710.0, 1e-6)); // two turns key as two turns
}

void ScriptCarriesEveryFrame() {
	Take t; t.fps = 59.94; t.name = "de_\"inferno\" take";
	for (int i = 0; i < 5; ++i) { Frame f; f.origin[0] = i * 10.0; t.frames.push_back(f); }
	const std::string s = AfterEffectsScript(t);
	size_t rows = 0;
	for (size_t at = s.find("\n["); at != std::string::npos; at = s.find("\n[", at + 1)) ++rows;
	CHECK(rows == 5);
	CHECK(s.find("var FPS = 59.940000;") != std::string::npos);
	CHECK(s.find("\"de_\\\"inferno\\\" take\"") != std::string::npos);
	CHECK(s.find("NO_AUTO_ORIENT") != std::string::npos);
}

} // namespace

int main() {
	FovWidensWithTheAspect();
	CameraAxesFollowSource();
	PointsLandWhereCs2DrewThem();
	TurnsKeySmoothly();
	ScriptCarriesEveryFrame();
	if (g_failed) { std::cerr << g_failed << " check(s) failed\n"; return 1; }
	std::cout << "CameraExportMathTests: all passed\n";
	return 0;
}
