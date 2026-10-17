#include "../../AfxHookSource2/SceneMarkerMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace Filmmaker::Marker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-9) { return std::fabs(a - b) <= epsilon; }
bool Orthonormal(Vec a, Vec b, Vec c) {
	return Near(Dot(a, a), 1, 1e-9) && Near(Dot(b, b), 1, 1e-9) && Near(Dot(c, c), 1, 1e-9)
		&& Near(Dot(a, b), 0, 1e-9) && Near(Dot(b, c), 0, 1e-9) && Near(Dot(a, c), 0, 1e-9);
}
int Brightness(Rgb c) { return c.r + c.g + c.b; }
double Hue(Rgb c) {
	const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
	const double mx = std::fmax(r, std::fmax(g, b)), mn = std::fmin(r, std::fmin(g, b)), d = mx - mn;
	if (d <= 0) return 0;
	double h = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
	h *= 60;
	return h < 0 ? h + 360 : h;
}
double HueGap(Rgb a, Rgb b) {
	const double d = std::fabs(Hue(a) - Hue(b));
	return d > 180 ? 360 - d : d;
}
} // namespace

int main() {
	// ---- the viewer's frame, in Source axes ----
	{
		Vec right, up;
		ViewBasis(Vec{1, 0, 0}, right, up);
		Check(Near(right.y, -1) && Near(up.z, 1), "looking along +X, right is -Y and up is +Z (Source axes)");
		const Vec forwards[] = { {1, 0, 0}, {0.3, -0.8, 0.2}, {0, 0, -1}, {0, 0, 1}, {1e-7, 0, -1} };
		for (const Vec f : forwards) {
			ViewBasis(f, right, up);
			Check(Orthonormal(Unit(f), right, up), "the frame stays whole however the viewer looks, straight down and up too");
		}
	}

	// ---- shading: a lit face keeps its colour, a face turned away never goes black ----
	{
		Vec right, up;
		const Vec forward{1, 0, 0};
		ViewBasis(forward, right, up);
		const Vec light = KeyLight(forward, right, up);
		const Rgb c{200, 100, 50};
		const Rgb toLight = Shade(c, light, light), awayFromLight = Shade(c, -light, light);
		Check(toLight.r == 200 && toLight.g == 100 && toLight.b == 50, "a face turned to the light shows its full colour");
		Check(awayFromLight.r == 90 && awayFromLight.g == 45 && awayFromLight.b == 23, "a face turned away keeps its floor of the colour");
		Check(Brightness(Shade(c, -forward, light)) > Brightness(Shade(c, forward, light)),
			"a face turned to the viewer reads brighter than one turned away from it");
		Check(Brightness(Shade(c, up, light)) > Brightness(Shade(c, -up, light)), "a top reads brighter than an underside");
	}

	// ---- mixing toward white, held to 0..1 ----
	{
		const Rgb a{0, 100, 200}, b{255, 255, 255};
		const Rgb none = Mix(a, b, 0), all = Mix(a, b, 1), past = Mix(a, b, 7), half = Mix(a, b, 0.5);
		Check(none.r == 0 && none.b == 200 && all.r == 255 && all.g == 255 && past.r == 255, "mix ends land on each colour");
		Check(half.r == 128 && half.g == 178 && half.b == 228, "half way is half way");
	}

	// ---- only the near faces of a closed solid ----
	{
		const Vec eye{100, 30, 20};
		const Vec normals[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
		int near = 0;
		for (const Vec n : normals) near += FacesViewer(n, n, eye) ? 1 : 0; // a cube of half size 1 at the origin
		Check(near == 3, "a box seen from a corner shows three faces");
		Check(FacesViewer(normals[0], normals[0], eye) && !FacesViewer(normals[1], normals[1], eye), "its +X face, never its -X face");
	}

	// ---- a marker's size on screen ----
	{
		Check(Near(ScreenScale(300), 1) && Near(ScreenScale(450), 1.5), "at 300 units a marker is its own size, and grows with distance");
		Check(Near(ScreenScale(10), 0.5) && Near(ScreenScale(0), 0.5), "very near, it is held at half size");
		Check(Near(ScreenScale(660), 2.2) && Near(ScreenScale(5000), 2.2), "far, it keeps a fixed world size and shrinks with the world");
	}

	// ---- a spot light's beam ----
	{
		const double pi = 3.14159265358979323846;
		const Beam narrow = SpotBeam(30, 55, 28);
		Check(Near(narrow.length, 55) && Near(narrow.rim, 55 * std::tan(15 * pi / 180)), "a narrow cone keeps its full reach");
		const Beam wide = SpotBeam(120, 55, 28);
		Check(Near(wide.rim, 28) && Near(wide.length, 28 / std::tan(60 * pi / 180)), "a wide cone is shortened to keep its rim");
		for (double cone = -10; cone <= 400; cone += 7) {
			const Beam b = SpotBeam(cone, 55, 28);
			Check(b.rim <= 28 + 1e-9 && b.length > 0 && b.length <= 55 + 1e-9 && std::isfinite(b.length),
				"any cone, even one past the 2..170 range, gives a beam within its reach and rim");
		}
	}

	// ---- one colour per kind, told apart at a glance ----
	{
		const std::vector<Rgb> kinds = { kCampathLive, kCampathFreeze, kFollowCamera, kFollowTarget, kSceneCamera,
			kPointLight, kSpotLight, kAreaLight };
		for (size_t i = 0; i < kinds.size(); ++i)
			for (size_t j = i + 1; j < kinds.size(); ++j)
				Check(HueGap(kinds[i], kinds[j]) >= 20, "every kind's colour is at least 20 degrees of hue from every other");
		Check(kSelected.r == 255 && kSelected.g == 255 && kSelected.b == 255, "the selected thing is white");
	}

	std::cout << "Scene marker palette, shading, beam and sizing passed.\n";
	return 0;
}
