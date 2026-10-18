#pragma once

// How the in-world markers look (Docs/features/scene-object-rigs.md, "In the world"): one colour
// per kind of thing, the light their solid faces are shaded by, and how a spot light's beam is
// fitted to the screen. Engine-free, so CampathDrawer.cpp (render thread) and SceneRig.cpp (main
// thread) share one palette, and the rules are tested on their own (scene-marker-math-tests.cpp).

#include <algorithm>
#include <cmath>

namespace Filmmaker::Marker {

struct Rgb { unsigned char r = 255, g = 255, b = 255; };

// One colour per kind, far enough apart in hue to tell at a glance. The K camera path takes the
// movie controls' global mode colours (Live gold, Freeze blue); the selected thing is white.
constexpr Rgb kCampathLive{255, 200, 40};
constexpr Rgb kCampathFreeze{60, 150, 255};
constexpr Rgb kFollowCamera{190, 80, 255};
constexpr Rgb kFollowTarget{80, 230, 255};
constexpr Rgb kSceneCamera{40, 225, 175};
constexpr Rgb kPointLight{255, 125, 45};
constexpr Rgb kSpotLight{255, 80, 150};
constexpr Rgb kAreaLight{175, 235, 60};
constexpr Rgb kSelected{255, 255, 255};

struct Vec { double x = 0, y = 0, z = 0; };
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec operator-(Vec a) { return {-a.x, -a.y, -a.z}; }
inline Vec operator*(double s, Vec a) { return {s * a.x, s * a.y, s * a.z}; }
inline double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec Unit(Vec a) {
	const double len = std::sqrt(Dot(a, a));
	return len > 1e-9 ? (1.0 / len) * a : Vec{};
}

// The viewer's right and up from where it looks, in Source axes (X forward, Y left, Z up): right is
// forward x world up, and up completes the frame. Looking straight up or down there is no horizon,
// so world X stands in for it instead of the frame collapsing.
inline void ViewBasis(Vec forward, Vec& right, Vec& up) {
	forward = Unit(forward);
	Vec r = Cross(forward, Vec{0, 0, 1});
	if (Dot(r, r) < 1e-8) r = Cross(forward, Vec{1, 0, 0});
	right = Unit(r);
	up = Unit(Cross(right, forward));
}

// The light the markers' solid faces are shaded by: over the viewer's left shoulder, so the faces
// turned to the viewer read lit and a shape's top and left read brighter than its underside.
inline Vec KeyLight(Vec forward, Vec right, Vec up) {
	return Unit(-0.55 * forward + 0.7 * up - 0.45 * right);
}

// A face's colour under that light: the full colour where it faces the light, and never darker than
// `floor` of it, so an unlit side still shows which kind it is.
inline Rgb Shade(Rgb c, Vec normal, Vec light, double floor = 0.45) {
	const double k = floor + (1.0 - floor) * (std::max)(0.0, Dot(Unit(normal), light));
	auto ch = [k](unsigned char v) { return (unsigned char)(std::min)(255.0, v * k + 0.5); };
	return {ch(c.r), ch(c.g), ch(c.b)};
}

// `a` blended toward `b` by t (0..1): a lens or panel lit in its light's own colour.
inline Rgb Mix(Rgb a, Rgb b, double t) {
	t = std::clamp(t, 0.0, 1.0);
	auto ch = [t](unsigned char x, unsigned char y) { return (unsigned char)(x + (y - x) * t + 0.5); };
	return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b)};
}

// Whether a face of a closed solid is turned toward the viewer. The markers write no depth, so a
// far face drawn after a near one would show through it: only the near faces are drawn.
inline bool FacesViewer(Vec normal, Vec faceCentre, Vec eye) { return Dot(normal, eye - faceCentre) > 0; }

// How much a marker is scaled to hold its size on screen: its distance over 300 units, held to
// 0.5..2.2. Past 660 units it keeps a fixed world size and shrinks with the world, instead of
// swelling into a blob far away.
inline double ScreenScale(double distance) { return std::clamp(distance / 300.0, 0.5, 2.2); }

// A spot light's beam gizmo, for its cone's full angle in degrees (held to 2..170): `reach` long,
// unless the cone is so wide the rim would pass `maxRim` across; then it is shortened to that rim,
// so a wide flood light reads as a short wide cone rather than a disc filling the screen.
struct Beam { double length = 0, rim = 0; };
inline Beam SpotBeam(double coneDegrees, double reach, double maxRim) {
	const double half = std::clamp(coneDegrees, 2.0, 170.0) * 0.5 * 3.14159265358979323846 / 180.0;
	const double t = std::tan(half);
	Beam b{reach, reach * t};
	if (b.rim > maxRim) { b.rim = maxRim; b.length = maxRim / t; }
	return b;
}

} // namespace Filmmaker::Marker
