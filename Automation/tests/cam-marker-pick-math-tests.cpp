// CamMarkerPick::Aimed (AfxHookSource2/Filmmaker/Movie/CamMarkerPickMath.h): which camera marker the
// view aims at, shared by CS2 and the CS:S hook.

#include "../../AfxHookSource2/Filmmaker/Movie/CamMarkerPickMath.h"

#include <cstdio>
#include <vector>

using namespace Filmmaker;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static CamMarker At(double x, double y, double z) {
	CamMarker m;
	m.x = x; m.y = y; m.z = z;
	return m;
}

int main() {
	const double origin[3] = { 0, 0, 0 };
	const double ahead[3] = { 0, 0, 0 };     // looking along +x
	const double left[3] = { 0, 90, 0 };     // looking along +y
	const double down[3] = { 90, 0, 0 };     // pitch 90: straight down (-z)

	std::vector<CamMarker> m = { At(1000, 0, 0), At(1000, 60, 0), At(-500, 0, 0) };
	CHECK(CamMarkerPick::Aimed(m, origin, ahead) == 0);     // dead centre
	CHECK(CamMarkerPick::Aimed(m, origin, left) == -1);     // nothing along +y
	m.push_back(At(0, 800, 0));
	CHECK(CamMarkerPick::Aimed(m, origin, left) == 3);
	CHECK(CamMarkerPick::Aimed(m, origin, down) == -1);     // behind and beside never pick

	// Far away, 60 units off the line is outside the ~5 degree cone; up close the cone widens.
	std::vector<CamMarker> off = { At(2000, 120, 0) };       // ~3.4 degrees off: inside
	CHECK(CamMarkerPick::Aimed(off, origin, ahead) == 0);
	off = { At(2000, 300, 0) };                              // ~8.5 degrees off: outside
	CHECK(CamMarkerPick::Aimed(off, origin, ahead) == -1);
	off = { At(60, 25, 0) };                                 // ~22.6 degrees off but 65 units away
	CHECK(CamMarkerPick::Aimed(off, origin, ahead) == 0);

	// The most centred of several wins; standing inside one picks it.
	std::vector<CamMarker> two = { At(1000, 40, 0), At(1000, 10, 0) };
	CHECK(CamMarkerPick::Aimed(two, origin, ahead) == 1);
	std::vector<CamMarker> inside = { At(1000, 0, 0), At(0, 0, 0) };
	CHECK(CamMarkerPick::Aimed(inside, origin, ahead) == 1);
	CHECK(CamMarkerPick::Aimed(std::vector<CamMarker>(), origin, ahead) == -1);

	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("CamMarkerPickMathTests passed\n");
	return 0;
}
