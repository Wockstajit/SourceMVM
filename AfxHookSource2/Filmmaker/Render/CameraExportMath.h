#pragma once

// The rendered camera as an After Effects script (`camera_aftereffects.jsx` in the take folder,
// written by Render/RenderSessionDriver.cpp when the camera sidecar is on). Running it makes a comp
// the render's size, frame rate and length with a camera keyed on every frame, so 3D layers sit in
// the shot without a 3D camera track. Engine-free; tested by Automation/tests/camera-export-math-tests.cpp.
//
// Source's frame: x forward, y left, z up, right-handed. A camera's rotation is
// Rz(yaw) * Ry(pitch) * Rx(roll), columns forward, left, up (pitch > 0 looks down).
// The FOV CS2 reports is the horizontal angle of a 4:3 frame; the picture's real horizontal angle
// widens with the aspect (HorizontalFovDeg).
//
// After Effects: x right, y down, z into the picture. A world point goes through W:
// (x, y, z) -> (x, -z, y) * scale, then minus the origin when recentred. An AE camera looks down its
// local +z with +x right and +y down, so its rotation is W * R * C, C mapping its (x, y, z) to
// Source's (-left, -up, forward). Zoom is (width / 2) / tan(hfov / 2) pixels.
//
// The rotation is split over a rig, because AE's Orientation (matrix Rx * Ry * Rz) locks at a Y of
// +-90 degrees, which a camera panning across the map passes all the time: a null carries the
// position and the heading as its Y Rotation (unwrapped, so a full turn keys a full turn), and the
// camera, parented to it at [0, 0, 0], carries the rest as Orientation, which stays near 0.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Filmmaker {
namespace CameraExport {

struct Frame {
	double origin[3] = { 0, 0, 0 };
	double angles[3] = { 0, 0, 0 }; // pitch, yaw, roll in degrees, as CS2 reports them
	double fov = 90.0;              // CS2's 4:3 horizontal FOV in degrees
};

struct Options {
	double scale = 1.0;    // AE pixels per Source unit
	bool recentre = true;  // the first frame's camera sits at AE's origin, so new layers land near it
};

struct Take {
	std::vector<Frame> frames;
	int width = 1920, height = 1080;
	double fps = 30.0;
	std::string name = "SOURCE:MVM camera";
	Options options;
};

constexpr double kPi = 3.14159265358979323846;
inline double Rad(double d) { return d * kPi / 180.0; }
inline double Deg(double r) { return r * 180.0 / kPi; }

struct Mat3 { double m[3][3]; };

inline Mat3 Mul(const Mat3& a, const Mat3& b) {
	Mat3 r{};
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
	return r;
}

inline Mat3 RotX(double a) { const double c = std::cos(a), s = std::sin(a); return { { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } } }; }
inline Mat3 RotY(double a) { const double c = std::cos(a), s = std::sin(a); return { { { c, 0, s }, { 0, 1, 0 }, { -s, 0, c } } }; }
inline Mat3 RotZ(double a) { const double c = std::cos(a), s = std::sin(a); return { { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } } }; }

// Columns forward, left, up in Source world space.
inline Mat3 SourceRotation(double pitchDeg, double yawDeg, double rollDeg) {
	return Mul(Mul(RotZ(Rad(yawDeg)), RotY(Rad(pitchDeg))), RotX(Rad(rollDeg)));
}

// The picture's real horizontal FOV: CS2 widens its 4:3 angle to the frame's aspect.
inline double HorizontalFovDeg(double engineFovDeg, int width, int height) {
	if (height <= 0 || width <= 0) return engineFovDeg;
	const double ratio = ((double)width / height) / (4.0 / 3.0);
	return Deg(2.0 * std::atan(ratio * std::tan(Rad(engineFovDeg) * 0.5)));
}

// A Source point in AE world space (before scale and recentring).
inline void AePoint(const double src[3], double out[3]) {
	out[0] = src[0]; out[1] = -src[2]; out[2] = src[1];
}

// The AE camera's rotation: its local axes (columns) in AE world space.
inline Mat3 AeCameraMatrix(const Frame& f) {
	static const Mat3 W = { { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } } };
	static const Mat3 C = { { { 0, 0, 1 }, { -1, 0, 0 }, { 0, -1, 0 } } }; // columns: -left, -up, forward
	return Mul(Mul(W, SourceRotation(f.angles[0], f.angles[1], f.angles[2])), C);
}

// R = Rx(a) * Ry(b) * Rz(c) -> (a, b, c), radians. After Effects' Orientation.
inline void EulerXyz(const Mat3& R, double out[3]) {
	const double sb = R.m[0][2];
	out[1] = std::asin(sb < -1 ? -1 : sb > 1 ? 1 : sb);
	if (std::fabs(sb) < 0.9999999) {
		out[0] = std::atan2(-R.m[1][2], R.m[2][2]);
		out[2] = std::atan2(-R.m[0][1], R.m[0][0]);
	} else {
		out[0] = std::atan2(R.m[2][1], R.m[1][1]);
		out[2] = 0;
	}
}

inline Mat3 FromEulerXyz(const double e[3]) { return Mul(Mul(RotX(e[0]), RotY(e[1])), RotZ(e[2])); }

// Moves each angle by whole turns to the value nearest the previous frame's, so a camera turning
// through 180 degrees keys a smooth curve instead of a jump of a full turn. Also tries the other
// Euler solution (a + pi, pi - b, c + pi), the same rotation, and keeps whichever lies closer.
inline void Unwrap(double e[3], const double prev[3]) {
	auto nearest = [](double v, double p) { return p + std::remainder(v - p, 2 * kPi); };
	const double a[3] = { nearest(e[0], prev[0]), nearest(e[1], prev[1]), nearest(e[2], prev[2]) };
	const double b[3] = { nearest(e[0] + kPi, prev[0]), nearest(kPi - e[1], prev[1]), nearest(e[2] + kPi, prev[2]) };
	auto dist = [&](const double* v) { return std::fabs(v[0] - prev[0]) + std::fabs(v[1] - prev[1]) + std::fabs(v[2] - prev[2]); };
	const double* best = dist(b) < dist(a) ? b : a;
	e[0] = best[0]; e[1] = best[1]; e[2] = best[2];
}

// Per-frame After Effects values: the rig null's position (px) and Y Rotation (degrees), the
// camera's Orientation inside it (degrees) and Zoom (px).
struct AeKey { double pos[3]; double heading; double orient[3]; double zoom; };

inline std::vector<AeKey> AeKeys(const Take& t) {
	std::vector<AeKey> keys;
	keys.reserve(t.frames.size());
	double base[3] = { 0, 0, 0 };
	if (t.options.recentre && !t.frames.empty()) AePoint(t.frames[0].origin, base);
	const double scale = t.options.scale > 0 ? t.options.scale : 1.0;
	double prev[3] = { 0, 0, 0 };
	double prevHeading = 0;
	for (size_t i = 0; i < t.frames.size(); ++i) {
		const Frame& f = t.frames[i];
		AeKey k{};
		double p[3];
		AePoint(f.origin, p);
		for (int a = 0; a < 3; ++a) k.pos[a] = (p[a] - base[a]) * scale;
		const Mat3 A = AeCameraMatrix(f);
		// The heading turns the null's +z toward the camera's forward seen from above
		// (RotY(h) * (0, 0, 1) = (sin h, 0, cos h)).
		double h = std::atan2(A.m[0][2], A.m[2][2]);
		if (i) h = prevHeading + std::remainder(h - prevHeading, 2 * kPi);
		prevHeading = h;
		k.heading = Deg(h);
		double e[3];
		EulerXyz(Mul(RotY(-h), A), e);
		if (i) Unwrap(e, prev);
		for (int a = 0; a < 3; ++a) { prev[a] = e[a]; k.orient[a] = Deg(e[a]); }
		k.zoom = (t.width * 0.5) / std::tan(Rad(HorizontalFovDeg(f.fov, t.width, t.height)) * 0.5);
		keys.push_back(k);
	}
	return keys;
}

inline std::string Num(double v, int digits) {
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.*f", digits, std::fabs(v) < 1e-9 ? 0.0 : v);
	return buf;
}

// Keeps a name safe inside a JavaScript double-quoted string.
inline std::string Quoted(const std::string& s) {
	std::string r = "\"";
	for (unsigned char c : s) {
		if (c == '\\' || c == '"') { r += '\\'; r += (char)c; }
		else if (c < 0x20) r += ' ';
		else r += (char)c;
	}
	return r + "\"";
}

// File > Scripts > Run Script File in After Effects. Frame 0 of the new comp is the take's first
// frame, so the rendered plates go in at the start.
inline std::string AfterEffectsScript(const Take& t) {
	const std::vector<AeKey> keys = AeKeys(t);
	std::string s;
	s += "// SOURCE:MVM camera for After Effects: File > Scripts > Run Script File.\n";
	s += "// It adds a comp the render's size, rate and length with this take's camera keyed on every frame.\n";
	s += "// Put the rendered plates in at frame 0. 3D layers then sit in the shot with no camera track.\n";
	s += "(function () {\n";
	s += "var NAME = " + Quoted(t.name) + ";\n";
	s += "var FPS = " + Num(t.fps, 6) + ";\n";
	s += "var WIDTH = " + std::to_string(t.width) + ", HEIGHT = " + std::to_string(t.height) + ";\n";
	s += "// " + Num(t.options.scale, 4) + " px per Source unit" + (t.options.recentre ? "; the first frame's camera is at [0, 0, 0]" : "; Source's map coordinates") + ".\n";
	s += "// rig x, y, z (px), rig heading (Y Rotation, degrees), camera orientation x, y, z (degrees), zoom (px)\n";
	s += "var KEYS = [\n";
	for (const AeKey& k : keys) {
		s += "[" + Num(k.pos[0], 3) + "," + Num(k.pos[1], 3) + "," + Num(k.pos[2], 3) + "," + Num(k.heading, 4) + ","
			+ Num(k.orient[0], 4) + "," + Num(k.orient[1], 4) + "," + Num(k.orient[2], 4) + "," + Num(k.zoom, 3) + "],\n";
	}
	s += "];\n";
	s += R"JS(if (!KEYS.length) { alert("SOURCE:MVM: this take has no camera frames."); return; }
app.beginUndoGroup("SOURCE:MVM camera");
var comp = app.project.items.addComp(NAME, WIDTH, HEIGHT, 1.0, KEYS.length / FPS, FPS);
var rig = comp.layers.addNull(KEYS.length / FPS);
rig.name = NAME + " rig";
rig.threeDLayer = true;
rig.enabled = false;
var cam = comp.layers.addCamera(NAME, [WIDTH / 2, HEIGHT / 2]);
cam.autoOrient = AutoOrientType.NO_AUTO_ORIENT;
var rt = rig.property("ADBE Transform Group"), ct = cam.property("ADBE Transform Group");
rt.property("ADBE Anchor Point").setValue([0, 0, 0]);
rt.property("ADBE Position").setValue([0, 0, 0]);
cam.parent = rig;
ct.property("ADBE Position").setValue([0, 0, 0]);
ct.property("ADBE Orientation").setValue([0, 0, 0]);
var times = [], pos = [], heading = [], ori = [], zoom = [];
for (var i = 0; i < KEYS.length; i++) {
    var k = KEYS[i];
    times.push(i / FPS);
    pos.push([k[0], k[1], k[2]]);
    heading.push(k[3]);
    ori.push([k[4], k[5], k[6]]);
    zoom.push(k[7]);
}
var props = [rt.property("ADBE Position"), rt.property("ADBE Rotate Y"), ct.property("ADBE Orientation"),
             cam.property("ADBE Camera Options Group").property("ADBE Camera Zoom")];
props[0].setValuesAtTimes(times, pos);
props[1].setValuesAtTimes(times, heading);
props[2].setValuesAtTimes(times, ori);
props[3].setValuesAtTimes(times, zoom);
for (var p = 0; p < props.length; p++)
    for (var n = 1; n <= props[p].numKeys; n++)
        props[p].setInterpolationTypeAtKey(n, KeyframeInterpolationType.LINEAR);
comp.openInViewer();
app.endUndoGroup();
})();
)JS";
	return s;
}

} // namespace CameraExport
} // namespace Filmmaker
