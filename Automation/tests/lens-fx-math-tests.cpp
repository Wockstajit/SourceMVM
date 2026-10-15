// Behaviour tests for the engine-free half of the lens module (depth of field, bokeh iris, flare
// parameters). The HLSL mirrors this maths; a static check at the bottom keeps the constants the
// shader carries in step with the ones pinned here.
#include "../../AfxHookSource2/Filmmaker/Lens/LensFxMath.h"
#include "../../AfxHookSource2/Filmmaker/Lens/LensFxShaders.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Filmmaker::Lens;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { std::printf("FAIL %s:%d  %s=%g vs %s=%g\n", __FILE__, __LINE__, #a, _a, #b, _b); ++g_failures; } } while (0)

static void TestParameterTable() {
	// Every key is unique and findable; defaults sit inside their own range.
	for (int i = 0; i < ParamCount; ++i) {
		const ParamSpec& s = Specs()[i];
		CHECK(FindParam(s.key) == i);
		CHECK(s.min <= s.def && s.def <= s.max);
		CHECK(s.min < s.max);
		for (int j = i + 1; j < ParamCount; ++j) CHECK(std::strcmp(s.key, Specs()[j].key) != 0);
	}
	CHECK(FindParam("dof.nonsense") == -1);
	CHECK(FindParam(nullptr) == -1);

	// The depth-of-field group stays small: at most 20 parameters, and the settings that were folded
	// into fixed constants (or replaced by dof.gamma) are gone, not aliased.
	int dofCount = 0;
	for (int i = 0; i < ParamCount; ++i) dofCount += InGroup(i, Group::Dof) ? 1 : 0;
	CHECK(dofCount <= 20);
	for (const char* removed : { "dof.focusSpeed", "dof.focusX", "dof.focusY", "dof.focal", "dof.nearScale", "dof.farScale",
		"dof.catEye", "dof.edgeSoft", "dof.hole", "dof.chroma", "dof.threshold", "dof.gain", "dof.weaponDist" })
		CHECK(FindParam(removed) == -1);
	for (const char* kept : { "dof.on", "dof.mode", "dof.auto", "dof.focus", "dof.fstop", "dof.maxBlur", "dof.nearStart",
		"dof.nearEnd", "dof.farStart", "dof.farEnd", "dof.shape", "dof.blades", "dof.rotation", "dof.roundness",
		"dof.squeeze", "dof.edge", "dof.gamma", "dof.quality", "dof.weapon", "dof.debug" })
		CHECK(FindParam(kept) >= 0);
	CHECK(Specs()[DofGamma].min == 0.0f && Specs()[DofGamma].max == 12.0f);

	// The flare group is fourteen controls (at most sixteen), and everything that was folded into a fixed
	// constant, a palette index or removed is gone, not aliased.
	int flareCount = 0;
	for (int i = 0; i < ParamCount; ++i) flareCount += InGroup(i, Group::Flare) ? 1 : 0;
	CHECK(flareCount == 14 && flareCount <= 16);
	for (const char* removed : { "flare.range", "flare.boost", "flare.spread", "flare.ghostGain", "flare.ghostSize", "flare.ghostShape",
		"flare.ghostHue", "flare.ghostSat", "flare.variety", "flare.seed", "flare.chroma", "flare.squeeze", "flare.bladeRound",
		"flare.haloWidth", "flare.haloCompress", "flare.haloRainbow", "flare.streakAng", "flare.starLen", "flare.starRot",
		"flare.starSpectrum", "flare.glowSize", "flare.dirtScale", "flare.edgeFade", "flare.warp", "flare.tintR", "flare.tintG",
		"flare.tintB", "flare.streakR", "flare.streakG", "flare.streakB" })
		CHECK(FindParam(removed) == -1);
	for (const char* kept : { "flare.on", "flare.intensity", "flare.threshold", "flare.ghosts", "flare.halo", "flare.streak",
		"flare.streakLen", "flare.star", "flare.blades", "flare.glow", "flare.dirt", "flare.tint", "flare.streakTint", "flare.debug" })
		CHECK(FindParam(kept) >= 0);
	CHECK(Specs()[FlareGhosts].max == (float)kMaxGhosts);
	CHECK(Specs()[FlareThreshold].max == 1.0f && Specs()[FlareThreshold].def < 0.95f && Specs()[FlareThreshold].def > Specs()[FlareThreshold].min);
	CHECK(Specs()[DofShape].max == (float)(IrisShapeCount - 1)); // no shape number outside the enum

	// Writes are clamped, integers round, NaN falls back to the default.
	CHECK(ClampParam(DofFstop, 0.0) == Specs()[DofFstop].min);
	CHECK(ClampParam(DofFstop, 999.0) == Specs()[DofFstop].max);
	CHECK(ClampParam(DofBlades, 6.4) == 6.0f);
	CHECK(ClampParam(DofBlades, 6.6) == 7.0f);
	CHECK(ClampParam(DofFocus, std::nan("")) == Specs()[DofFocus].def);

	Values v;
	CHECK(v.IsDefault());
	v[DofFocus] = 1234;
	CHECK(!v.IsDefault());
	v.Reset();
	CHECK(v.IsDefault());
	// Effects ship OFF: a fresh install must not blur the picture.
	CHECK(v[DofOn] == 0 && v[FlareOn] == 0);
}

static void TestCircleOfConfusion() {
	Values L;
	L[DofMode] = 0; L[DofFstop] = 2.0f; L[DofMaxBlur] = 40;
	const float focus = 400, W = 1920, H = 1080;

	// Nothing blurs at the focus distance.
	CHECK_NEAR(CircleOfConfusion(L, focus, focus, W, H), 0.0, 1e-4);
	// Behind the focus is positive, in front is negative.
	CHECK(CircleOfConfusion(L, focus * 3, focus, W, H) > 0.5f);
	CHECK(CircleOfConfusion(L, focus * 0.4f, focus, W, H) < -0.5f);
	// Further from focus never blurs less.
	float last = 0;
	for (float d = focus; d < 20000; d *= 1.3f) {
		const float c = CircleOfConfusion(L, d, focus, W, H);
		CHECK(c >= last - 1e-4f);
		last = c;
	}
	// A wider aperture (smaller f-number) blurs more, in proportion (the lens is a fixed 50 mm).
	Values wide = L; wide[DofFstop] = 1.0f;
	const float base = CircleOfConfusion(L, 2000, focus, W, H);
	CHECK(CircleOfConfusion(wide, 2000, focus, W, H) > base);
	CHECK_NEAR(CircleOfConfusion(wide, 2000, focus, W, H), base * 2.0, base * 0.01);
	// The blur is capped at dof.maxBlur, scaled to the image height.
	Values huge = L; huge[DofFstop] = 0.7f; huge[DofMaxBlur] = 8;
	CHECK_NEAR(CircleOfConfusion(huge, kFarSky, focus, W, H), huge[DofMaxBlur], 1e-3);
	CHECK_NEAR(CircleOfConfusion(huge, kFarSky, focus, W * 2, H * 2), huge[DofMaxBlur] * 2, 1e-2);
}

static void TestZoneMode() {
	Values L;
	L[DofMode] = 1; L[DofMaxBlur] = 30;
	L[DofNearStart] = 60; L[DofNearEnd] = 180; L[DofFarStart] = 600; L[DofFarEnd] = 2400;
	const float W = 1920, H = 1080;
	// Sharp between the zones, fully blurred outside the outer limits, smooth in between.
	CHECK_NEAR(CircleOfConfusion(L, 300, 0, W, H), 0.0, 1e-6);
	CHECK_NEAR(CircleOfConfusion(L, 600, 0, W, H), 0.0, 1e-6);
	CHECK_NEAR(CircleOfConfusion(L, 60, 0, W, H), -30.0, 1e-4);
	CHECK_NEAR(CircleOfConfusion(L, 10, 0, W, H), -30.0, 1e-4);
	CHECK_NEAR(CircleOfConfusion(L, 2400, 0, W, H), 30.0, 1e-4);
	CHECK_NEAR(CircleOfConfusion(L, 90000, 0, W, H), 30.0, 1e-4);
	const float mid = CircleOfConfusion(L, 1500, 0, W, H);
	CHECK(mid > 5 && mid < 25);
	// Crossed-over inputs stay sane instead of dividing by zero.
	L[DofNearEnd] = 10; L[DofNearStart] = 500; L[DofFarStart] = 5; L[DofFarEnd] = 5;
	const float c = CircleOfConfusion(L, 200, 0, W, H);
	CHECK(std::isfinite(c) && std::fabs(c) <= 30.0f + 1e-3f);
}

static void TestIrisShapes() {
	// A regular polygon touches radius 1 at its vertices and cos(pi/n) mid-side.
	for (int n = 3; n <= 12; ++n) {
		CHECK_NEAR(PolygonRadius(0.0f, n, 0.0f), 1.0, 1e-4);
		CHECK_NEAR(PolygonRadius((float)kPi / n, n, 0.0f), std::cos(kPi / n), 1e-4);
		for (float t = -7; t < 7; t += 0.17f) {
			const float r = PolygonRadius(t, n, 0.3f);
			CHECK(r >= std::cos(kPi / n) - 1e-4f && r <= 1.0f + 1e-4f);
		}
	}
	// Roundness 1 is a circle.
	for (float t = 0; t < 6.3f; t += 0.3f) CHECK_NEAR(RoundedPolygonRadius(t, 5, 0.2f, 1.0f), 1.0, 1e-5);
	// Heart and star never leave the unit disc.
	float heartMax = 0;
	for (float t = 0; t < 6.2832f; t += 0.002f) {
		heartMax = std::max(heartMax, HeartRadius(t, 0));
		CHECK(StarRadius(t, 5, 0) <= 1.0f + 1e-4f && StarRadius(t, 5, 0) >= 0.39f);
	}
	CHECK(heartMax <= 1.0f + 1e-3f && heartMax > 0.9f);

	// The heart is upright: screen y points DOWN, so with no rotation the tip is at +pi/2 and the
	// notch between the lobes at the top (-pi/2). Sideways would be a heart nobody recognises.
	const float halfPi = (float)(kPi * 0.5);
	CHECK_NEAR(HeartRadius(halfPi, 0), 1.0, 1e-3);      // tip, down
	CHECK(HeartRadius(-halfPi, 0) < 0.05f);             // notch, up
	CHECK(HeartRadius(0.0f, 0) > 0.3f && HeartRadius((float)kPi, 0) > 0.3f); // lobes are wide, not pointed
	CHECK_NEAR(HeartRadius(0.0f, 0), HeartRadius((float)kPi, 0), 1e-3);      // left/right symmetric
	// Rotation turns the whole shape by the same angle.
	CHECK_NEAR(HeartRadius(halfPi + 1.0f, 1.0f), HeartRadius(halfPi, 0), 1e-3);
}

static void TestKernel() {
	Values L;
	// Every tap of every shape stays inside the unit disc (after squeeze), so the gather never
	// asks for a texel further than the radius it was given.
	for (int shape = 0; shape < IrisShapeCount; ++shape)
		for (float squeeze : { 1.0f, 1.8f, 3.0f }) {
			L[DofShape] = (float)shape; L[DofSqueeze] = squeeze; L[DofBlades] = 6; L[DofEdge] = 1;
			const int n = 64;
			double area = 0, weights = 0;
			for (int i = 0; i < n; ++i) {
				const KernelTap t = KernelSample(L, i, n, 0.37f);
				const double sq = std::sqrt((double)squeeze);
				CHECK(std::isfinite(t.x) && std::isfinite(t.y) && t.weight >= 0.0f);
				CHECK(t.radius >= 0.0f && t.radius <= 1.0f + 1e-5f);
				CHECK(std::sqrt((t.x * sq) * (t.x * sq) + (t.y / sq) * (t.y / sq)) <= 1.0 + 1e-3);
				area += t.radius; weights += t.weight;
			}
			CHECK(area > 0 && weights > 0);
		}

	// Anamorphic squeeze makes the taps taller than wide, at constant area.
	L.Reset(); L[DofSqueeze] = 2.0f;
	double sx = 0, sy = 0;
	for (int i = 0; i < 128; ++i) { const KernelTap t = KernelSample(L, i, 128, 0.1f); sx += t.x * t.x; sy += t.y * t.y; }
	CHECK(sy > sx * 2.5);

	// The ring shape leaves the middle empty; a Gaussian favours the middle.
	L.Reset(); L[DofShape] = IrisRing;
	double inner = 0, outer = 0;
	for (int i = 0; i < 256; ++i) { const KernelTap t = KernelSample(L, i, 256, 0); (t.radius < kRingHole - 0.1f ? inner : outer) += t.weight; }
	CHECK(inner < outer * 0.02);
	// The bright rim weights the outer edge of a flat disc up, and only there.
	L.Reset(); L[DofEdge] = 2.0f;
	double core = 0, rim = 0; int coreN = 0, rimN = 0;
	for (int i = 0; i < 256; ++i) {
		const KernelTap t = KernelSample(L, i, 256, 0);
		if (t.radius < 1.0f - kEdgeSoftness - 0.02f) { core += t.weight; ++coreN; }
		else if (t.radius > 0.99f) { rim += t.weight; ++rimN; }
	}
	CHECK(coreN > 0 && rimN > 0 && rim / rimN > 2.5 * core / coreN);

	// The polygon corner really is reached: some tap lands close to the vertex radius.
	L.Reset(); L[DofShape] = IrisPolygon; L[DofBlades] = 6; L[DofRoundness] = 0;
	double far = 0;
	for (int i = 0; i < 256; ++i) { const KernelTap t = KernelSample(L, i, 256, 0); far = std::max(far, (double)std::sqrt(t.x * t.x + t.y * t.y)); }
	CHECK(far > 0.95 && far <= 1.0001);

	// ORIENTATION. The gather shows the kernel reflected through its centre, so the KERNEL is the
	// shape turned a half turn: a pentagon or star with a point at the top of the screen has its
	// point at kernel angle +pi/2, and a heart with its tip down has the tip at kernel angle -pi/2.
	const float halfPi = (float)(kPi * 0.5);
	L.Reset(); L[DofShape] = IrisPolygon; L[DofBlades] = 5; L[DofRoundness] = 0;
	CHECK_NEAR(IrisRadius(L, halfPi), 1.0, 1e-4);                      // vertex, seen at the top
	CHECK_NEAR(IrisRadius(L, -halfPi), std::cos(kPi / 5), 1e-4);       // flat side, seen at the bottom
	L[DofRotation] = 36.0f; // half a blade: the vertex swings round to where the flat side was
	CHECK_NEAR(IrisRadius(L, halfPi), std::cos(kPi / 5), 1e-3);
	L.Reset(); L[DofShape] = IrisStar; L[DofBlades] = 5;
	CHECK_NEAR(IrisRadius(L, halfPi), 1.0, 1e-4);
	CHECK(IrisRadius(L, -halfPi) < 0.6f);
	L.Reset(); L[DofShape] = IrisHeart;
	CHECK_NEAR(IrisRadius(L, -halfPi), 1.0, 1e-3);                     // tip: kernel up, seen down
	CHECK(IrisRadius(L, halfPi) < 0.05f);                              // notch: kernel down, seen at the top
	// The heart is centred on the tap centre (its polar curve is drawn about the notch): the area-weighted
	// mean of the taps is at the middle for every rotation, and the shape fills about the same disc as a hexagon.
	for (float rotDeg : { 0.0f, 40.0f, -130.0f }) {
		L[DofRotation] = rotDeg;
		double mx = 0, my = 0, ws = 0, far = 0;
		for (int i = 0; i < 4096; ++i) {
			const KernelTap t = KernelSample(L, i, 4096, 0.3f);
			mx += t.x * t.weight; my += t.y * t.weight; ws += t.weight;
			far = std::max(far, std::sqrt((double)t.x * t.x + (double)t.y * t.y));
		}
		CHECK(std::fabs(mx / ws) < 0.03 && std::fabs(my / ws) < 0.03);
		CHECK(far > 1.05 && far < 1.25);
	}
	L[DofRotation] = 0.0f;

	L.Reset();
	CHECK(TapCount(L) == 96);
	L[DofQuality] = 0; CHECK(TapCount(L) == 48);
	L[DofQuality] = 2; CHECK(TapCount(L) == 192);
}

// The bokeh gamma: what makes the iris shape visible. The lift must reach a lamp, must not reach an
// in-focus pixel or a lit wall, and must be strong enough that a small light still fills its disc.
static void TestHighlightBoost() {
	// Gamma 0 is a plain blur: nothing is lifted, whatever the pixel.
	CHECK_NEAR(HighlightBoost(0.0f, 1.0f, 1.0f, 30.0f), 1.0, 1e-6);
	// Pure white, isolated, well out of focus: exactly `gamma` stops.
	for (float g : { 1.0f, 4.0f, 8.0f }) CHECK_NEAR(HighlightBoost(g, 1.0f, 1.0f, 10.0f), std::exp2((double)g * kLiftStopsPerGamma), std::exp2((double)g * kLiftStopsPerGamma) * 1e-5);
	CHECK(kLiftStopsPerGamma == 1.75f && kMaxLiftStops == 15.0f);
	CHECK_NEAR(HighlightBoost(12.0f, 1.0f, 1.0f, 10.0f), std::exp2((double)kMaxLiftStops), 1e-3);   // capped: fits a half float
	CHECK(HighlightBoost(12.0f, 1.0f, 1.0f, 10.0f) >= 4096.0f); // "unmistakable" range
	// Monotonic in gamma and in brightness; never below 1.
	float last = 1.0f;
	for (float g = 0; g <= 12; g += 0.5f) { const float b = HighlightBoost(g, 1.0f, 1.0f, 10.0f); CHECK(b >= last); last = b; }
	last = 1.0f;
	for (float p = 0; p <= 1.0f; p += 0.02f) { const float b = HighlightBoost(6.0f, p, 1.0f, 10.0f); CHECK(b >= last - 1e-4f); last = b; }
	// Dark and mid pixels barely move; a near-clipped lamp moves a lot.
	CHECK(HighlightBoost(6.0f, 0.3f, 1.0f, 10.0f) < 1.01f);
	CHECK(HighlightBoost(6.0f, 0.5f, 1.0f, 10.0f) < 1.15f);
	CHECK(HighlightBoost(6.0f, 0.98f, 1.0f, 10.0f) > 30.0f);
	// In-focus pixels are never lifted, and the lift ramps in smoothly with blur (no popping).
	CHECK_NEAR(HighlightBoost(12.0f, 1.0f, 1.0f, 0.0f), 1.0, 1e-6);
	CHECK_NEAR(HighlightBoost(12.0f, 1.0f, 1.0f, 0.5f), 1.0, 1e-6);
	CHECK(HighlightFocusRamp(3.0f) == 1.0f && HighlightFocusRamp(-3.0f) == 1.0f && HighlightFocusRamp(40.0f) == 1.0f);
	last = 1.0f;
	for (float c = 0.5f; c <= 3.0f; c += 0.1f) { const float b = HighlightBoost(8.0f, 1.0f, 1.0f, c); CHECK(b >= last); last = b; }
	CHECK(HighlightBoost(8.0f, 1.0f, 1.0f, -10.0f) == HighlightBoost(8.0f, 1.0f, 1.0f, 10.0f)); // near field lifts too

	// Isolation: a lamp on a dark surround counts, a lit wall or the sky (as bright as its ring) does not.
	CHECK_NEAR(HighlightIsolation(1.0f, 0.05f), 1.0, 1e-6);
	CHECK_NEAR(HighlightIsolation(1.0f, 0.3f), 1.0, 1e-3);   // a lamp among a cluster of others
	CHECK_NEAR(HighlightIsolation(0.9f, 0.9f), 0.0, 1e-6);   // the sky
	CHECK_NEAR(HighlightIsolation(0.85f, 0.85f), 0.0, 1e-6); // a lit wall
	CHECK(HighlightIsolation(0.85f, 0.75f) > 0.2f && HighlightIsolation(0.85f, 0.75f) < 0.8f); // its edge: partly
	CHECK(HighlightIsolation(1.0f, 0.8f) > 0.5f);             // a lamp inside its own glow (measured: ring 0.8 of the peak)
	CHECK(HighlightIsolation(1.0f, 0.99f) == 0.0f);           // a big clipped area
	CHECK_NEAR(IsolationRingMean(0.99f, 0.8f), 0.8, 1e-6);    // the darker ring decides
	CHECK(HighlightBoost(12.0f, 0.9f, HighlightIsolation(0.9f, 0.9f), 20.0f) == 1.0f);           // never whited out
	CHECK(HighlightIsolation(0.0f, 0.5f) >= 0.0f && HighlightIsolation(0.0f, 0.5f) <= 1.0f);

	// Core: the inside of a lamp's clipped core (its touching pixels are about as bright) lifts; a lone
	// speck in a sunlit texture does not, however isolated it looks.
	CHECK_NEAR(HighlightCore(1.0f, 1.0f), 1.0, 1e-6);    // inside a 3+ px core
	CHECK_NEAR(HighlightCore(1.0f, 0.92f), 1.0, 1e-6);   // core still clipping, a little ragged
	CHECK_NEAR(HighlightCore(1.0f, 0.62f), 0.0, 1e-6);   // a speck on the measured sunlit wall (median 0.62)
	CHECK_NEAR(HighlightCore(1.0f, 0.75f), 0.0, 1e-6);   // a speck with a soft edge
	CHECK(HighlightCore(1.0f, 0.82f) > 0.0f && HighlightCore(1.0f, 0.82f) < 1.0f); // the edge of a core: partly
	CHECK(HighlightCore(0.0f, 0.0f) >= 0.0f && HighlightCore(0.0f, 0.0f) <= 1.0f);
	// The reported wash-out: a clipped speck on a ~0.62 wall is "isolated" enough for the full lift, and
	// only the core test keeps it at its own brightness.
	CHECK(HighlightIsolation(1.0f, 0.62f) > 0.9f);
	CHECK(HighlightBoost(6.0f, 1.0f, HighlightIsolation(1.0f, 0.62f) * HighlightCore(1.0f, 0.62f), 20.0f) == 1.0f);
	CHECK(HighlightBoost(6.0f, 1.0f, HighlightIsolation(1.0f, 0.3f) * HighlightCore(1.0f, 1.0f), 20.0f) > 1000.0f); // a real lamp still lifts
	// The ring is a fraction of the blur radius, bounded, and follows the resolution.
	CHECK_NEAR(IsolationRadius(20.0f, 1080.0f), 12.0, 1e-4);
	CHECK_NEAR(IsolationRadius(100.0f, 1080.0f), 24.0, 1e-4);
	CHECK_NEAR(IsolationRadius(1.0f, 1080.0f), 2.0, 1e-4);
	CHECK_NEAR(IsolationRadius(100.0f, 2160.0f), 48.0, 1e-3);

	// The lift is what makes a small lamp read as a disc: a 5 px lamp on black spread over a disc of
	// radius 26 keeps (area ratio) * boost of its brightness. At the default gamma the disc is
	// visible; at gamma 8 it is bold (display value near white); at gamma 0 it is a ghost.
	const double areaRatio = (kPi * 2.5 * 2.5) / (kPi * 26.0 * 26.0);
	auto discDisplay = [&](float gamma) {
		const double linear = std::pow(1.0, 2.2) * HighlightBoost(gamma, 1.0f, 1.0f, 26.0f) * areaRatio;
		return std::pow((std::min)(linear, 1.0), 1.0 / 2.2);
	};
	CHECK(discDisplay(0.0f) < 0.15);
	CHECK(discDisplay(Specs()[DofGamma].def) > 0.35);
	CHECK(discDisplay(8.0f) > 0.85);

	// Gather lod follows the tap spacing (wider gathers read coarser mips) and stays inside the chain.
	CHECK_NEAR(GatherLod(1.0f, 64), 0.0, 1e-6);
	CHECK(GatherLod(60.0f, 64) > GatherLod(26.0f, 64));
	CHECK(GatherLod(26.0f, 32) > GatherLod(26.0f, 128)); // fewer taps: coarser cells, coarser mip
	CHECK(GatherLod(90.0f, 32) <= (float)(kCocMipLevels - 1) + 1e-6f);
	CHECK_NEAR(GatherLod(26.0f, 64), std::log2(26.0 * std::sqrt(kPi / 64.0) * kGatherFootprint), 1e-4);
	// The old constant (0.6) undersampled: its footprint was smaller than the tap spacing.
	CHECK(kGatherFootprint >= 0.8f && kGatherFootprint <= 1.2f);
}

static void TestMasks() {
	// The built-in masks are non-empty, not full, and named.
	for (int m = 0; m < MaskCount; ++m) {
		int lit = 0, total = 0;
		for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
			lit += IrisMaskCoverage(m, (x + 0.5f) / 32 - 1, (y + 0.5f) / 32 - 1) > 0.5f;
			++total;
		}
		CHECK(lit > total / 10 && lit < total * 9 / 10);
		CHECK(IrisMaskFromName(IrisMaskName(m)) == m);
	}
	CHECK(IrisMaskFromName("nope") == -1);
	// The smile has eyes: the centre of an eye is dark, the cheek beside it is lit.
	CHECK(IrisMaskCoverage(MaskSmile, -0.32f, 0.3f) < 0.5f);
	CHECK(IrisMaskCoverage(MaskSmile, -0.6f, 0.3f) > 0.5f);
}

static void TestPresets() {
	const char* dof[] = { "cinematic", "anamorphic", "portrait", "miniature", "hearts", "stars" };
	for (const char* name : dof) {
		Values L;
		L[DofOn] = 1;
		CHECK(ApplyDofPreset(L, name));
		CHECK(L[DofOn] == 1); // a preset never flips the switch
		for (int i = 0; i < ParamCount; ++i) CHECK(L[i] >= Specs()[i].min && L[i] <= Specs()[i].max);
	}
	const char* flare[] = { "cinematic", "anamorphic", "classic", "star", "dreamy" }; // at most five looks
	for (const char* name : flare) {
		Values L;
		L[FlareOn] = 1;
		CHECK(ApplyFlarePreset(L, name));
		CHECK(L[FlareOn] == 1); // a preset never flips the switch
		for (int i = 0; i < ParamCount; ++i) CHECK(L[i] >= Specs()[i].min && L[i] <= Specs()[i].max);
	}
	{
		// The cinematic look IS the default flare (spelled out in the preset so a default change cannot move it).
		Values d, c;
		ApplyFlarePreset(c, "cinematic");
		for (int i = 0; i < ParamCount; ++i) if (InGroup(i, Group::Flare)) CHECK(c[i] == d[i]);
		// The looks differ in the way their names say.
		Values an, cl, st, dr;
		ApplyFlarePreset(an, "anamorphic"); ApplyFlarePreset(cl, "classic"); ApplyFlarePreset(st, "star"); ApplyFlarePreset(dr, "dreamy");
		CHECK(an[FlareStreak] > d[FlareStreak] && an[FlareStreakLen] > d[FlareStreakLen] && an[FlareStar] == 0.0f);
		CHECK(cl[FlareGhosts] == 8.0f && cl[FlareStreak] == 0.0f && cl[FlareStar] == 0.0f && cl[FlareHalo] > d[FlareHalo]);
		CHECK(st[FlareStar] > d[FlareStar] && st[FlareStreak] == 0.0f && st[FlareGhosts] <= 1.0f);
		CHECK(dr[FlareGlow] > d[FlareGlow] && dr[FlareGhosts] == 0.0f && dr[FlareStar] == 0.0f && dr[FlareStreak] == 0.0f);
		// Applying one flare look and then another leaves nothing of the first behind.
		Values a, b;
		ApplyFlarePreset(a, "dreamy"); ApplyFlarePreset(a, "classic");
		ApplyFlarePreset(b, "classic");
		for (int i = 0; i < ParamCount; ++i) CHECK(a[i] == b[i]);
	}
	Values L;
	CHECK(!ApplyDofPreset(L, "bogus") && !ApplyFlarePreset(L, "bogus"));
	CHECK(!ApplyDofPreset(L, "dreamy") && !ApplyDofPreset(L, "mirror")); // dropped: at most six looks
	// The shape looks pick their shape, and the bokeh gamma is high enough that it shows.
	Values hearts; ApplyDofPreset(hearts, "hearts");
	CHECK((int)hearts[DofShape] == IrisHeart && hearts[DofGamma] >= 6.0f);
	Values stars; ApplyDofPreset(stars, "stars");
	CHECK((int)stars[DofShape] == IrisStar && stars[DofGamma] >= 6.0f);

	// Applying one preset and then another leaves nothing of the first behind.
	Values a, b;
	ApplyDofPreset(a, "anamorphic");
	ApplyDofPreset(a, "portrait");
	ApplyDofPreset(b, "portrait");
	for (int i = 0; i < ParamCount; ++i) CHECK(a[i] == b[i]);

	// A flare preset does not touch depth-of-field values, and the reverse.
	Values c;
	c[DofFocus] = 777;
	ApplyFlarePreset(c, "star");
	CHECK(c[DofFocus] == 777);
	Values d;
	d[FlareIntensity] = 3;
	ApplyDofPreset(d, "cinematic");
	CHECK(d[FlareIntensity] == 3);

	// ResetGroup keeps the switch.
	Values e;
	e[DofOn] = 1; e[DofFocus] = 55;
	ResetGroup(e, Group::Dof);
	CHECK(e[DofOn] == 1 && e[DofFocus] == Specs()[DofFocus].def);
}

// ---- lens flare ----------------------------------------------------------------------------------

static float SourceStrength(float luma, float threshold, float peak, float ringLuma) {
	// gate x isolation, the two factors a source's strength is made of (the size floor and the border
	// fade only depend on that strength and the position, so they are checked on their own)
	return FlareGate(luma, threshold) * FlareIsolation(FlareRingWeight(peak, ringLuma));
}

static void TestFlareGateAndIsolation() {
	// The gate is on LUMINANCE: a saturated blue sky (0.3, 0.5, 1.0) has a brightest channel of 1.0 but a
	// luminance of 0.49, and is not a source at any threshold the slider offers.
	const float sky = Luma709(0.3f, 0.5f, 1.0f);
	CHECK_NEAR(sky, 0.4936, 1e-3);
	CHECK(sky < Specs()[FlareThreshold].min);
	CHECK_NEAR(FlareGate(sky, Specs()[FlareThreshold].min), 0.0, 1e-6);
	CHECK(Luma709(1.0f, 0.0f, 0.0f) < 0.25f);         // a pure red neon: not a source either
	CHECK_NEAR(Luma709(1.0f, 1.0f, 1.0f), 1.0, 1e-5); // white is 1

	// Nothing below the threshold; a ramp from the threshold up to kFlareCeil (just above white), so a
	// clipped white lamp is a full source at a low threshold and a partial one at a high threshold;
	// monotone in both arguments.
	CHECK_NEAR(FlareGate(0.5f, 0.85f), 0.0, 1e-6);
	CHECK_NEAR(FlareGate(0.85f, 0.85f), 0.0, 1e-6);
	CHECK_NEAR(FlareGate(kFlareCeil, 0.85f), 1.0, 1e-6);
	CHECK(FlareGate(0.5f * (0.85f + kFlareCeil), 0.85f) > 0.45f && FlareGate(0.5f * (0.85f + kFlareCeil), 0.85f) < 0.55f);
	CHECK(FlareGate(1.0f, 0.75f) > 0.9f);             // white at the lowest threshold: nearly full
	// "About 0.95 only genuinely white hot spots throw a flare, at 1.0 nothing does."
	CHECK(FlareGate(1.0f, 0.95f) > 0.4f);
	CHECK_NEAR(FlareGate(0.93f, 0.95f), 0.0, 1e-6);   // a bright wall
	// A clipped white lamp gives a GRADUAL fall as the threshold rises (the sweep the panel offers), not a step.
	{
		float prev = 2.0f; int distinct = 0;
		for (float thr : { 0.75f, 0.8f, 0.85f, 0.9f, 0.95f, 0.97f, 1.0f }) {
			const float v = FlareGate(1.0f, thr) * FlareThresholdFade(thr);
			CHECK(v < prev + 1e-6f);
			if (prev - v > 0.02f) ++distinct;
			prev = v;
		}
		CHECK(distinct >= 5);                         // every step of the sweep is visibly weaker than the last
		CHECK_NEAR(FlareThresholdFade(1.0f), 0.0, 1e-6);
		CHECK_NEAR(FlareThresholdFade(1.0f - kFlareFadeSpan), 1.0, 1e-6);
		CHECK_NEAR(FlareThresholdFade(0.75f), 1.0, 1e-6);
		CHECK(FlareThresholdFade(0.85f) > 0.7f);      // the shipped threshold keeps most of the flare
	}
	CHECK_NEAR(FlareGate(1.0f, 1.0f), 0.0, 1e-6);     // nothing, not even a clipped white
	CHECK(Specs()[FlareThreshold].max == 1.0f);       // the slider reaches "nothing"
	for (float luma = 0.0f; luma <= 1.0f; luma += 0.01f) {
		float lastT = 2.0f;
		for (float thr = 0.75f; thr <= 1.0f; thr += 0.01f) {
			const float g = FlareGate(luma, thr);
			CHECK(g >= 0.0f && g <= 1.0f);
			CHECK(g <= lastT + 1e-6f);                    // raising the threshold never adds light
			lastT = g;
		}
	}
	float last = 0.0f;
	for (float luma = 0.0f; luma <= 1.0f; luma += 0.005f) { const float g = FlareGate(luma, 0.9f); CHECK(g >= last - 1e-6f); last = g; }

	// Isolation: a light on a darker surround counts, a big bright area does not.
	CHECK_NEAR(FlareRingWeight(1.0f, 0.1f), 0.0, 1e-6);     // dark ring
	CHECK_NEAR(FlareRingWeight(0.95f, 0.95f), 1.0, 1e-6);   // a ring as bright as the source: a wall
	CHECK_NEAR(FlareRingWeight(1.0f, 0.97f), 1.0, 1e-6);
	CHECK(FlareRingWeight(1.0f, 0.90f) > 0.0f && FlareRingWeight(1.0f, 0.90f) < 1.0f); // sun in a nearly-as-bright sky: partial
	CHECK_NEAR(FlareIsolation(0.0f), 1.0, 1e-6);
	CHECK_NEAR(FlareIsolation(kFlareIsoLo), 1.0, 1e-6);
	CHECK_NEAR(FlareIsolation(0.25f), 0.0, 1e-6);           // the corner of a big bright rectangle (a quarter of the ring)
	CHECK_NEAR(FlareIsolation(0.5f), 0.0, 1e-6);            // its straight edge (half the ring)
	CHECK_NEAR(FlareIsolation(1.0f), 0.0, 1e-6);            // its inside
	CHECK(kFlareIsoHi <= 0.25f);                            // so a corner is dropped too: no rectangle of sources
	last = 2.0f;
	for (float m = 0.0f; m <= 1.0f; m += 0.01f) { const float v = FlareIsolation(m); CHECK(v <= last + 1e-6f); last = v; }
	last = 0.0f;
	for (float l = 0.0f; l <= 1.0f; l += 0.01f) { const float v = FlareRingWeight(0.9f, l); CHECK(v >= last - 1e-6f); last = v; }

	// The budget: full strength while little of the frame is near-white, smoothly less for a frame full of sky.
	CHECK_NEAR(FlareBudget(0.0f), 1.0, 1e-6);
	CHECK_NEAR(FlareBudget(kFlareBudgetArea), 1.0, 1e-6);
	CHECK(FlareBudget(0.2f) < 0.25f && FlareBudget(0.2f) > 0.1f);       // a sky-filled opening
	CHECK_NEAR(FlareBudgetWeight(0.5f), 0.0, 1e-6);                        // a mid-grey wall is not near-white
	CHECK_NEAR(FlareBudgetWeight(1.0f), 1.0, 1e-6);
	last = 2.0f;
	for (float m = 0.0f; m < 1.0f; m += 0.002f) { const float v = FlareBudget(m); CHECK(v <= last + 1e-6f && v > 0.0f); last = v; }

	// The wide ring: a light inside a big bright area is dropped, a sun in a much dimmer sky is kept.
	CHECK_NEAR(FlareWideRingWeight(1.0f, 0.1f), 0.0, 1e-6);
	CHECK_NEAR(FlareWideRingWeight(1.0f, 0.6f), 0.0, 1e-6);    // blue sky around the sun
	CHECK_NEAR(FlareWideRingWeight(1.0f, 0.97f), 1.0, 1e-6);   // a blown-out opening
	CHECK_NEAR(FlareWideIsolation(0.0f), 1.0, 1e-6);
	CHECK_NEAR(FlareWideIsolation(kFlareWideIsoLo), 1.0, 1e-6);
	CHECK_NEAR(FlareWideIsolation(kFlareWideIsoHi), 0.0, 1e-6);
	CHECK(kFlareWideRadius > 2.0f * kFlareIsoRadius);
	last = 2.0f;
	for (float m = 0.0f; m <= 1.0f; m += 0.01f) { const float v = FlareWideIsolation(m); CHECK(v <= last + 1e-6f); last = v; }

	// The whole point of the isolation being blind to the threshold: a source's strength (gate x isolation)
	// never increases when the threshold goes up, for any luminance / peak / surround.
	int checked = 0;
	for (float luma = 0.6f; luma <= 1.0f; luma += 0.05f)
		for (float peak = luma; peak <= 1.0f; peak += 0.1f)
			for (float ring = 0.0f; ring <= 1.0f; ring += 0.1f) {
				float prev = 2.0f;
				for (float thr = 0.75f; thr <= 1.0f; thr += 0.0125f) {
					const float s = SourceStrength(luma, thr, peak, ring);
					CHECK(s <= prev + 1e-6f);
					prev = s; ++checked;
				}
			}
	CHECK(checked > 1000);

	// Specks are dropped below a size floor, monotone.
	CHECK_NEAR(FlareSourceFloor(0.0f), 0.0, 1e-6);
	CHECK_NEAR(FlareSourceFloor(kFlareMinLo), 0.0, 1e-6);
	CHECK_NEAR(FlareSourceFloor(kFlareMinHi), 1.0, 1e-6);
	CHECK(FlareSourceFloor(0.6f) == 1.0f);
}

static void TestFlareBorderAndLimiter() {
	// Sources and ghost samples fade to nothing at the frame edge, so no copy of the frame ends in an edge.
	CHECK_NEAR(FlareBorderFade(0.0f, 0.5f), 0.0, 1e-6);
	CHECK_NEAR(FlareBorderFade(0.5f, 0.0f), 0.0, 1e-6);
	CHECK_NEAR(FlareBorderFade(-0.3f, 0.5f), 0.0, 1e-6);     // outside the frame
	CHECK_NEAR(FlareBorderFade(kFlareBorder, kFlareBorder), 1.0, 1e-6);
	CHECK_NEAR(FlareBorderFade(0.4f, 0.4f), 1.0, 1e-6);
	float last = 0.0f;
	for (float d = -0.1f; d <= 0.2f; d += 0.002f) { const float v = FlareBorderFade(d, 0.5f); CHECK(v >= last - 1e-6f); last = v; }
	const float aspect = 16.0f / 9.0f;
	CHECK_NEAR(FlareBorderFadeUv(0.0f, 0.5f, aspect), 0.0, 1e-6);
	CHECK_NEAR(FlareBorderFadeUv(1.0f, 0.5f, aspect), 0.0, 1e-6);
	CHECK_NEAR(FlareBorderFadeUv(0.5f, 1.0f, aspect), 0.0, 1e-6);
	CHECK_NEAR(FlareBorderFadeUv(0.2f, 0.4f, aspect), FlareBorderFadeUv(0.8f, 0.6f, aspect), 1e-6); // symmetric

	// The soft limiter: linear for small values, never past 1, monotone.
	CHECK_NEAR(FlareLimit(0.0f), 0.0, 1e-7);
	CHECK_NEAR(FlareLimit(0.01f), 0.01, 1e-4);
	CHECK(FlareLimit(50.0f) < 1.0f + 1e-7f && FlareLimit(50.0f) > 0.999f);
	CHECK_NEAR(FlareLimit(-1.0f), 0.0, 1e-7);
	last = -1.0f;
	for (float x = 0.0f; x <= 20.0f; x += 0.05f) { const float v = FlareLimit(x); CHECK(v >= last && v <= 1.0f); last = v; }
	// Intensity scales the flare smoothly (each step adds light) and the screen blend onto ANY picture stays
	// inside [0, 1]: intensity 2, a dense flare layer of 3 and a frame that is already 0.95 does not overflow.
	for (float pic : { 0.0f, 0.4f, 0.95f, 1.0f }) {
		float lastOut = -1.0f;
		for (float intensity = 0.0f; intensity <= 2.0f; intensity += 0.1f) {
			const float f = FlareLimit(3.0f * intensity);
			const float out = 1.0f - (1.0f - pic) * (1.0f - f);
			CHECK(out >= lastOut - 1e-6f && out <= 1.0f + 1e-6f && out >= pic - 1e-6f);
			lastOut = out;
		}
	}
	// At the default intensity a sunlit frame (0.9) moves by at most a few per cent for a normal flare (0.15).
	CHECK(1.0f - (1.0f - 0.9f) * (1.0f - FlareLimit(0.15f)) < 0.99f);
}

static void TestGhostGeometry() {
	// Every ghost lies ON the straight line through its source and the frame centre, mirrored ones on the
	// far side; the positions are cen + scale * (source - cen).
	const float cx = 0.5f, cy = 0.5f;
	for (float sx : { 0.1f, 0.3f, 0.85f }) for (float sy : { 0.15f, 0.5f, 0.9f }) {
		const float dx = sx - cx, dy = sy - cy;
		for (int k = 0; k < kMaxGhosts; ++k) {
			const float m = GhostScale(k);
			const float gx = cx + m * dx, gy = cy + m * dy;
			const float cross = (gx - cx) * dy - (gy - cy) * dx;
			CHECK_NEAR(cross, 0.0, 1e-6);
			if (std::fabs(dx) + std::fabs(dy) > 0.05f)
				CHECK((k % 2 == 0) ? ((gx - cx) * dx + (gy - cy) * dy < 0) : ((gx - cx) * dx + (gy - cy) * dy > 0));
		}
	}
	for (int k = 0; k < kMaxGhosts; ++k) {
		CHECK((k % 2 == 0) ? GhostScale(k) < 0.0f : GhostScale(k) > 0.0f);
		CHECK(std::fabs(GhostScale(k)) >= 0.3f && std::fabs(GhostScale(k)) <= 1.5f); // stays a sensible size and distance
		for (int j = k + 1; j < kMaxGhosts; ++j) CHECK(std::fabs(GhostScale(k) - GhostScale(j)) > 0.05f);
	}
	CHECK(GhostScale(-3) == GhostScale(0) && GhostScale(99) == GhostScale(kMaxGhosts - 1));
	// The ghosts fade with distance from the axis and along the chain, never to nothing on the axis.
	CHECK_NEAR(GhostFade(0.0f), 1.0, 1e-6);
	CHECK(GhostFade(2.0f) >= 0.29f && GhostFade(2.0f) <= 0.31f);
	float last = 2.0f;
	for (float r = 0.0f; r <= 1.5f; r += 0.01f) { const float v = GhostFade(r); CHECK(v <= last + 1e-6f); last = v; }
	CHECK(GhostGainAt(0) > GhostGainAt(7) && GhostGainAt(7) > 0.0f);

	// The halo is an angular mean of blobs on a circle round the centre: a blob covers 1/RingBlobs of it,
	// so mean x RingBlobs is the blob's own brightness at any radius; nothing at the centre itself.
	for (float aspect : { 1.0f, 16.0f / 9.0f }) {
		CHECK_NEAR(HaloRingBlobs(0.0f, aspect), 0.0, 1e-7);
		for (float r : { 0.1f, 0.3f, 0.6f, 1.0f }) {
			const double blobDiameter = 2.0 * kGhostRadius * aspect;     // in frame heights
			const double meanOfOneBlob = blobDiameter / (2.0 * kPi * r); // fraction of the circle it covers
			CHECK_NEAR(meanOfOneBlob * HaloRingBlobs(r, aspect), 1.0, 1e-5);
		}
		CHECK(HaloRingBlobs(0.6f, aspect) > HaloRingBlobs(0.3f, aspect));
	}

	// Spikes: a hexagonal aperture makes six, an odd count twice as many as it has blades; evenly spaced,
	// the first one vertical (so a horizontal streak never hides a spike).
	CHECK(SpikeLines(6) == 3 && SpikeLines(4) == 2 && SpikeLines(12) == 6 && SpikeLines(3) == 3 && SpikeLines(5) == 5 && SpikeLines(11) == 11);
	for (int blades = 3; blades <= 12; ++blades) {
		const int lines = SpikeLines(blades);
		CHECK(2 * lines == ((blades % 2 == 0) ? blades : 2 * blades));
		CHECK(lines <= 12);
		CHECK_NEAR(SpikeAngle(0, lines), kPi * 0.5, 1e-6);
		for (int j = 1; j < lines; ++j) CHECK_NEAR(SpikeAngle(j, lines) - SpikeAngle(j - 1, lines), kPi / lines, 1e-5);
	}
}

static std::string ReadRepoFile(const char* relative) {
	// __FILE__ is .../Automation/tests/lens-fx-math-tests.cpp
	std::string here = __FILE__;
	size_t cut = here.find_last_of("/\\");
	std::string dir = cut == std::string::npos ? "." : here.substr(0, cut);
	std::ifstream in(dir + "/../../" + relative, std::ios::binary);
	std::stringstream ss; ss << in.rdbuf();
	return ss.str();
}

static void TestFlarePalettes() {
	// Names are unique, findable, and the two tints are indices into the tables.
	for (int i = 0; i < kFlareTintCount; ++i) {
		CHECK(FlareTintFromName(FlareTintName(i)) == i);
		const Rgb c = FlareTintColor(i);
		CHECK(c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1);
		for (int j = i + 1; j < kFlareTintCount; ++j) CHECK(std::strcmp(FlareTintName(i), FlareTintName(j)) != 0);
	}
	for (int i = 0; i < kStreakTintCount; ++i) {
		CHECK(StreakTintFromName(StreakTintName(i)) == i);
		const Rgb c = StreakTintColor(i);
		CHECK(c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1);
	}
	CHECK(FlareTintFromName("nope") == -1 && FlareTintFromName(nullptr) == -1 && FlareTintFromName("3") == -1);
	CHECK(StreakTintFromName("warm") == -1 && StreakTintFromName(nullptr) == -1);
	CHECK((int)Specs()[FlareTint].max == kFlareTintCount - 1 && (int)Specs()[FlareStreakTint].max == kStreakTintCount - 1);
	// Out-of-range indices clamp into the table instead of reading past it.
	CHECK(FlareTintColor(-4).r == FlareTintColor(0).r && FlareTintColor(99).g == FlareTintColor(kFlareTintCount - 1).g);
	CHECK(StreakTintColor(99).b == StreakTintColor(kStreakTintCount - 1).b);
	// The defaults are the warm flare and the blue streak the effect always had.
	CHECK(std::strcmp(FlareTintName((int)Specs()[FlareTint].def), "warm") == 0);
	CHECK(std::strcmp(StreakTintName((int)Specs()[FlareStreakTint].def), "blue") == 0);
	CHECK_NEAR(FlareTintColor(0).r, 1.0, 1e-6); CHECK_NEAR(FlareTintColor(0).g, 217.0 / 255.0, 1e-6);
	CHECK_NEAR(StreakTintColor(0).b, 1.0, 1e-6);

	// What the shaders read: the constants block carries the parameters and the palette colour, in order.
	Values L;
	L[FlareIntensity] = 1.5f; L[FlareThreshold] = 0.9f; L[FlareGhosts] = 3; L[FlareHalo] = 0.7f;
	L[FlareStreak] = 1.1f; L[FlareStreakLen] = 0.4f; L[FlareStar] = 0.9f; L[FlareBlades] = 7;
	L[FlareGlow] = 0.2f; L[FlareDirt] = 0.6f; L[FlareTint] = 3; L[FlareStreakTint] = 4; L[FlareDebug] = 1;
	const FlareConstants k = MakeFlareConstants(L, true);
	CHECK(std::fabs(k.a[0] - 1.5f * FlareThresholdFade(0.9f)) < 1e-6f && k.a[1] == 0.9f && k.a[2] == 3 && k.a[3] == 0.7f);
	CHECK(k.b[0] == 1.1f && k.b[1] == 0.4f && k.b[2] == 0.9f && k.b[3] == 7);
	CHECK(k.c[0] == 0.2f && k.c[1] == 0.6f && k.c[2] == 1.0f && k.c[3] == 1.0f);
	CHECK(k.tint[0] == FlareTintColor(3).r && k.tint[1] == FlareTintColor(3).g && k.tint[2] == FlareTintColor(3).b);
	CHECK(k.streakTint[0] == StreakTintColor(4).r && k.streakTint[2] == StreakTintColor(4).b);
	CHECK(MakeFlareConstants(L, false).c[2] == 0.0f);

	// The panel shows the same colours: every swatch in ConfigHudLensTabJs.h is the palette entry of that index.
	const std::string js = ReadRepoFile("AfxHookSource2/Filmmaker/Panorama/ConfigHudLensTabJs.h");
	CHECK(!js.empty());
	auto hexOf = [](const Rgb& c) {
		char buf[16];
		std::snprintf(buf, sizeof buf, "#%02x%02x%02xff", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
		return std::string(buf);
	};
	for (int i = 0; i < kFlareTintCount; ++i)
		CHECK(js.find(std::string("['") + std::to_string(i) + "', '" + hexOf(FlareTintColor(i)) + "', '" + FlareTintName(i) + "']") != std::string::npos);
	for (int i = 0; i < kStreakTintCount; ++i)
		CHECK(js.find(std::string("['") + std::to_string(i) + "', '" + hexOf(StreakTintColor(i)) + "', '" + StreakTintName(i) + "']") != std::string::npos);
	// No removed flare setting is left in the panel, and every remaining one is.
	for (const char* removed : { "flare.range", "flare.boost", "flare.spread", "flare.ghostGain", "flare.ghostSize", "flare.ghostShape",
		"flare.ghostHue", "flare.ghostSat", "flare.variety", "flare.seed", "flare.chroma", "flare.squeeze", "flare.bladeRound",
		"flare.haloWidth", "flare.haloCompress", "flare.haloRainbow", "flare.streakAng", "flare.starLen", "flare.starRot",
		"flare.starSpectrum", "flare.glowSize", "flare.dirtScale", "flare.edgeFade", "flare.warp", "flare.tintR", "flare.tintG",
		"flare.tintB", "flare.streakR", "flare.streakG", "flare.streakB" })
		CHECK(js.find(removed) == std::string::npos);
	for (int i = 0; i < ParamCount; ++i)
		if (InGroup(i, Group::Flare) && i != FlareOn) CHECK(js.find(std::string("'") + Specs()[i].key + "'") != std::string::npos);
	// Two flare pages, and no page shows more than eight controls (one Look row + seven sliders; three sliders,
	// two swatch rows and a toggle).
	CHECK(js.find("flareSec.__page('Look')") != std::string::npos && js.find("flareSec.__page('Detail')") != std::string::npos);
	CHECK(js.find("flareSec.__page('", js.find("flareSec.__page('Detail')") + 10) == std::string::npos);
}

// The shader is compiled at runtime, so a typo would only show up in game. These catch the ones
// that can be caught here: the entry points the C++ asks for exist, and the constants the HLSL
// mirrors from the header are the same numbers.
static void TestShaderSource() {
	const std::string src = std::string(kShaderCommon) + kShaderDof + kShaderFlare + kShaderComposite;
	for (const char* entry : { "VS", "FocusPS", "CocPS", "MipPS", "TilePS", "DilateHPS", "DilateVPS", "GatherPS", "DenoisePS",
		"BrightPS", "Down13PS", "SourcePS", "SourceHPS", "ApertureBlurPS", "GhostPS", "StreakPS", "GlowHPS", "GlowVPS",
		"CompositePS" })
		CHECK(src.find(std::string(" ") + entry + "(") != std::string::npos);
	// Same physical constants as CircleOfConfusion.
	CHECK(src.find("focus * 25.4") != std::string::npos);
	CHECK(src.find("/ 36.0") != std::string::npos);
	CHECK(src.find("kFarSky = 60000.0") != std::string::npos);
	CHECK(kFarSky == 60000.0f && kMillimetresPerUnit == 25.4f && kSensorWidthMm == 36.0f);
	CHECK(src.find("2.39996323") != std::string::npos);
	// The star's inner radius and the heart's normalisation match the header.
	CHECK(src.find(".4 + .6 * pow(k, 1.6)") != std::string::npos);
	CHECK(src.find("max(r, 0) * .25") != std::string::npos);
	// Every parameter has a slot in the constant buffer comment block: spot-check the blocks exist.
	for (const char* block : { "dofA;", "dofB;", "dofC;", "irisA;", "irisB;", "flareA;", "flareC;", "flareTint;", "streakTint;", "tileInfo;", "passInfo;", "modes;" })
		CHECK(src.find(block) != std::string::npos);
	// The constant-buffer members of the removed settings are gone, and so is the gather logic that used them.
	for (const char* gone : { "float4 focusPoint", "float4 dofD", "float4 irisC", "catEye", "smoothstep(thr, 1, lumG)", "cocMip[4]" })
		CHECK(src.find(gone) == std::string::npos);
	// The fixed settings carry the same numbers as the header's constants.
	CHECK(kFocalLengthMm == 50.0f && src.find("kFocalMm = 50.0;") != std::string::npos);
	CHECK(kAutofocusX == 0.5f && kAutofocusY == 0.5f && src.find("kAutofocusPoint = float2(.5, .5)") != std::string::npos);
	CHECK(kAutofocusPull == 6.0f && src.find("kAutofocusPull = 6.0;") != std::string::npos);
	CHECK(kWeaponDistance == 30.0f && src.find("kWeaponDist = 30.0;") != std::string::npos);
	CHECK(kEdgeSoftness == 0.22f && src.find("kEdgeSoft = .22;") != std::string::npos);
	CHECK(kRingHole == 0.55f && src.find("kRingHole = .55;") != std::string::npos);
	CHECK(kGatherFootprint == 0.9f && src.find("kGatherFootprint = .9;") != std::string::npos);
	CHECK(kGatherJitter == 1.0f && src.find("kGatherJitter = 1.0;") != std::string::npos);
	// The bokeh gamma is applied in CocPS (before the mips) with the same shape as HighlightBoost:
	// sixth power of the peak channel, a ramp over the first 3 px of blur, isolation ring, 2^stops.
	CHECK(src.find("float HighlightBoost(int2 p, float3 src, float coc)") != std::string::npos);
	CHECK(src.find("pow(saturate(peak), 6)") != std::string::npos);
	CHECK(src.find("saturate((abs(coc) - .5) / 2.5)") != std::string::npos);
	CHECK(src.find("min(max(.6 * abs(coc), 2), 24 * dims.y / 1080)") != std::string::npos);
	CHECK(kIsolationOuter == 1.8f && src.find("ring[k] * rho * 1.8") != std::string::npos);
	CHECK(src.find("saturate((.95 - (min(acc, accOuter) * .125) / max(peak, 1e-3)) / .25)") != std::string::npos);
	CHECK(src.find("exp2(stops * iso * core)") != std::string::npos);
	CHECK(src.find("saturate((crossMin / max(peak, 1e-3) - .75) / .15)") != std::string::npos); // HighlightCore
	CHECK(src.find("min(dofC.y * kLiftStopsPerGamma * key * ramp, kMaxLiftStops)") != std::string::npos);
	CHECK(src.find("kLiftStopsPerGamma = 1.75;") != std::string::npos && src.find("kMaxLiftStops = 15.0;") != std::string::npos);
	CHECK(src.find("ToLinear(src) * HighlightBoost(p, src, coc)") != std::string::npos);
	// The lod comes from the tap spacing, not a hand-tuned constant, and the chain has 6 levels.
	CHECK(src.find("log2(max(R * sqrt(kPi / n) * kGatherFootprint, 1))") != std::string::npos);
	CHECK(kCocMipLevels == 6);
	// The heart is the same upright heart in the shader.
	CHECK(src.find("float s = -sin(t);") != std::string::npos);
	CHECK(src.find("float ph = th + kPi;") != std::string::npos);   // kernel angle -> screen angle (half turn)
	CHECK(src.find("PolyR(ph, irisA.y, irisA.z + top)") != std::string::npos);
	CHECK(src.find("float2(-p.x, -p.y)") != std::string::npos);     // custom image is read reflected too
	// The frame the flare reads is clamped to what the screen shows.
	CHECK(src.find("ResolvePS") == std::string::npos);   // the flare reads the sharp frame; there is no resolve pass any more

	// ---- the flare shaders ----
	// Every constant the flare HLSL mirrors from LensFxMath.h carries the same number.
	auto shaderConst = [&](const char* name) -> double {
		const std::string key = std::string("static const float ") + name + " = ";
		const size_t at = src.find(key);
		if (at == std::string::npos) { std::printf("FAIL shader constant %s missing\n", name); ++g_failures; return -1e30; }
		return std::atof(src.c_str() + at + key.size());
	};
	CHECK_NEAR(shaderConst("kFlareCeil"), kFlareCeil, 1e-7);
	CHECK_NEAR(shaderConst("kFlareBudgetArea"), kFlareBudgetArea, 1e-9);
	CHECK_NEAR(shaderConst("kFlareBudgetLo"), kFlareBudgetLo, 1e-7);
	CHECK_NEAR(shaderConst("kFlareBudgetHi"), kFlareBudgetHi, 1e-7);
	CHECK_NEAR(shaderConst("kFlareBudgetPow"), kFlareBudgetPow, 1e-7);
	CHECK_NEAR(shaderConst("kFlareWideRadius"), kFlareWideRadius, 1e-7);
	CHECK_NEAR(shaderConst("kFlareWideLo"), kFlareWideLo, 1e-7);
	CHECK_NEAR(shaderConst("kFlareWideHi"), kFlareWideHi, 1e-7);
	CHECK_NEAR(shaderConst("kFlareWideIsoLo"), kFlareWideIsoLo, 1e-7);
	CHECK_NEAR(shaderConst("kFlareWideIsoHi"), kFlareWideIsoHi, 1e-7);
	CHECK_NEAR(shaderConst("kStreakSoft"), kStreakSoft, 1e-7);
	CHECK_NEAR(shaderConst("kRingLodBias"), kRingLodBias, 1e-7);
	CHECK(GatherLod(60.0f, 96, IrisRing) < GatherLod(60.0f, 96) - 0.9f);
	CHECK_NEAR(shaderConst("kHeartCentroid"), kHeartCentroid, 1e-7);
	CHECK_NEAR(shaderConst("kHeartScale"), kHeartScale, 1e-7);
	CHECK_NEAR(shaderConst("kFlareIsoRadius"), kFlareIsoRadius, 1e-7);
	CHECK_NEAR(shaderConst("kFlareIsoLo"), kFlareIsoLo, 1e-7);
	CHECK_NEAR(shaderConst("kFlareIsoHi"), kFlareIsoHi, 1e-7);
	CHECK_NEAR(shaderConst("kFlareRelMargin"), kFlareRelMargin, 1e-7);
	CHECK_NEAR(shaderConst("kFlareRelSoft"), kFlareRelSoft, 1e-7);
	CHECK_NEAR(shaderConst("kFlareMinLo"), kFlareMinLo, 1e-7);
	CHECK_NEAR(shaderConst("kFlareMinHi"), kFlareMinHi, 1e-7);
	CHECK_NEAR(shaderConst("kFlareBorder"), kFlareBorder, 1e-7);
	CHECK_NEAR(shaderConst("kGhostRadius"), kGhostRadius, 1e-7);
	CHECK_NEAR(shaderConst("kGhostBlobGain"), kGhostBlobGain, 1e-5);
	CHECK_NEAR(shaderConst("kBladeRoundness"), kBladeRoundness, 1e-7);
	CHECK_NEAR(shaderConst("kGhostGain"), kGhostGain, 1e-7);
	CHECK_NEAR(shaderConst("kGhostFringe"), kGhostFringe, 1e-7);
	CHECK_NEAR(shaderConst("kHaloGain"), kHaloGain, 1e-7);
	CHECK_NEAR(shaderConst("kHaloFringe"), kHaloFringe, 1e-7);
	CHECK_NEAR(shaderConst("kStreakGain"), kStreakGain, 1e-4);
	CHECK_NEAR(shaderConst("kSpikeGain"), kSpikeGain, 1e-4);
	CHECK_NEAR(shaderConst("kLineTaper"), kLineTaper, 1e-4);
	CHECK(src.find("exp(-falloff * u) * (1 - smoothstep(kLineTaper, 1, u))") != std::string::npos);
	// A streak or spike line fades to exactly nothing at the end of its reach and never rises along it.
	CHECK_NEAR(LineTailWeight(1.0f, 3.0f), 0.0, 1e-7);
	{ float last = 2.0f; for (float u = 0; u <= 1.0f; u += 0.02f) { const float w = LineTailWeight(u, 3.0f); CHECK(w <= last + 1e-6f); last = w; } }
	CHECK(LineTailWeight(0.5f, 3.0f) > 0.2f && LineTailWeight(0.95f, 3.0f) < 0.02f);
	CHECK_NEAR(shaderConst("kGlowGain"), kGlowGain, 1e-4);
	CHECK_NEAR(shaderConst("kDirtBoost"), kDirtBoost, 1e-7);
	{
		// The ghost table: the eight scales in order.
		const size_t at = src.find("kGhostScales[8] = {");
		CHECK(at != std::string::npos);
		if (at != std::string::npos) {
			const char* p = src.c_str() + at + std::strlen("kGhostScales[8] = {");
			for (int k = 0; k < kMaxGhosts; ++k) {
				char* end = nullptr;
				const double v = std::strtod(p, &end);
				CHECK_NEAR(v, GhostScale(k), 1e-6);
				p = end;
				while (*p == ',' || *p == ' ') ++p;
			}
		}
	}
	// The shapes of the source path: luminance gate with the fixed knee, the isolation ring on the frame's own
	// luminance (never the threshold), the size floor and the border fade.
	CHECK(src.find("smoothstep(flareA.y, kFlareCeil, l)") != std::string::npos);
	CHECK(src.find("float l = Luma(c);") != std::string::npos);
	CHECK(src.find("smoothstep(peak - kFlareRelMargin - kFlareRelSoft, peak - kFlareRelMargin, l)") != std::string::npos);
	CHECK(src.find("1 - smoothstep(kFlareIsoLo, kFlareIsoHi, ring / 16)") != std::string::npos);
	CHECK(src.find("smoothstep(kFlareMinLo, kFlareMinHi, Luma(s))") != std::string::npos);
	CHECK(src.find("float2 d = min(uv, 1 - uv) * float2(modes.z, 1);") != std::string::npos);
	CHECK(src.find("smoothstep(0, kFlareBorder, d.x) * smoothstep(0, kFlareBorder, d.y)") != std::string::npos);
	CHECK(src.find("float2 ringUv = float2(dims.y / dims.x, 1) * kFlareIsoRadius;") != std::string::npos);
	{
		// The isolation loop must not read the threshold: nothing between SourcePS and its closing brace
		// mentions flareA (the threshold lives in flareA.y).
		const size_t a = src.find("float4 SourcePS(");
		const size_t b = src.find("float4 SourceHPS(");
		CHECK(a != std::string::npos && b != std::string::npos && a < b);
		if (a != std::string::npos && b != std::string::npos) CHECK(src.substr(a, b - a).find("flareA") == std::string::npos);
	}
	// Ghosts: cen + (uv - cen) / m, windowed by the position they are sampled from, faded with distance.
	CHECK(src.find("float2 src = cen + (i.uv - cen) / m;") != std::string::npos);
	CHECK(src.find("ghostSrcTex.SampleLevel(linearClamp, uv, 0).rgb * BorderFade(uv)") != std::string::npos);
	CHECK(src.find("float fade = 1 - .7 * smoothstep(.4, 1.1, r);") != std::string::npos);
	CHECK(src.find("kGhostGain / (1 + .3 * k)") != std::string::npos);
	CHECK(src.find("float m = kGhostScales[k];") != std::string::npos);
	// Halo: an angular mean on a circle round the centre, scaled by HaloRingBlobs.
	CHECK(src.find("float blobs = 2 * kPi * r / (2 * kGhostRadius * modes.z);") != std::string::npos);
	CHECK(src.find("float2 q = cen + float2(cos(a), sin(a)) * rr / asp;") != std::string::npos);
	// Spikes and streak: the physical spike count, vertical first, symmetric taps about the pixel.
	CHECK(src.find("int lines = (blades % 2 == 0) ? blades / 2 : blades;") != std::string::npos);
	CHECK(src.find("float a = kPi * .5 + j * kPi / lines;") != std::string::npos);
	CHECK(src.find("tex.SampleLevel(linearClamp, uv + o, 0).rgb + tex.SampleLevel(linearClamp, uv - o, 0).rgb") != std::string::npos);
	CHECK(src.find("(1 - exp(-kStreakGain * s)) * flareB.x") != std::string::npos);
	CHECK(src.find("(1 - exp(-kSpikeGain * sp)) * flareB.z") != std::string::npos);
	CHECK(src.find("1 - exp(-kGlowGain * acc / ws)") != std::string::npos);
	CHECK(src.find("1 - exp(-kGhostBlobGain * acc / wsum)") != std::string::npos);
	// Bounded composite: a soft limiter on the flare sum, screen-blended onto the picture.
	CHECK(src.find("float3 f = 1 - exp(-max(((gh + gl) * flareTint.rgb * dirt + st) * flareA.x * budget, 0));") != std::string::npos);
	CHECK(src.find("col = flareC.w > .5 ? f : 1 - (1 - col) * (1 - f);") != std::string::npos);
	// What was removed from the shaders is gone.
	for (const char* gone : { "Spectrum(", "WarpUv", "SampleGhost", "float4 flareD", "float4 flareE", "float4 flareF", "float4 flareG",
		"float4 flareH", "float4 flareI", "float Rnd(", "flareG.", "flareH.", "flareI." })
		CHECK(src.find(gone) == std::string::npos);

}

// The constant buffer is written by LensFx.cpp's Constants struct and read by the cbuffer in the shader:
// the two lists must have the same members in the same order, or every shader reads shifted values.
static void TestConstantBufferLockStep() {
	const std::string src = std::string(kShaderCommon);
	const size_t cb = src.find("cbuffer LensConstants");
	CHECK(cb != std::string::npos);
	std::vector<std::string> hlsl;
	{
		const size_t open = src.find('{', cb), close = src.find("};", open);
		std::istringstream lines(src.substr(open + 1, close - open - 1));
		std::string line;
		while (std::getline(lines, line)) {
			size_t at = line.find("float4 ");
			if (at == std::string::npos) at = line.find("row_major float4x4 ");
			if (at == std::string::npos) continue;
			const size_t nameStart = line.find(' ', line.find("float4", at) ) + 1;
			const size_t nameEnd = line.find(';', nameStart);
			hlsl.push_back(line.substr(nameStart, nameEnd - nameStart));
		}
	}
	const std::string cpp = ReadRepoFile("AfxHookSource2/Filmmaker/Lens/LensFx.cpp");
	CHECK(!cpp.empty());
	std::vector<std::string> host;
	{
		const size_t open = cpp.find("struct Constants {");
		const size_t close = cpp.find("};", open);
		CHECK(open != std::string::npos && close != std::string::npos);
		std::istringstream lines(cpp.substr(open, close - open));
		std::string line;
		while (std::getline(lines, line)) {
			if (line.find("float inverseViewProjection[16];") != std::string::npos) { host.push_back("inverseViewProjection"); continue; }
			const size_t at = line.find("F4 ");
			if (at == std::string::npos) continue;
			std::string names = line.substr(at + 3);
			names = names.substr(0, names.find(';'));
			std::istringstream parts(names);
			std::string n;
			while (std::getline(parts, n, ',')) {
				n.erase(0, n.find_first_not_of(" \t"));
				n.erase(n.find_last_not_of(" \t") + 1);
				if (!n.empty()) host.push_back(n);
			}
		}
	}
	CHECK(!hlsl.empty() && hlsl.size() == host.size());
	for (size_t i = 0; i < hlsl.size() && i < host.size(); ++i) {
		if (hlsl[i] != host[i]) std::printf("constant buffer member %zu: shader '%s' vs LensFx.cpp '%s'\n", i, hlsl[i].c_str(), host[i].c_str());
		CHECK(hlsl[i] == host[i]);
	}
	// The flare block is the five members MakeFlareConstants fills.
	int flareMembers = 0;
	for (const std::string& n : hlsl) flareMembers += (n.rfind("flare", 0) == 0 || n == "streakTint") ? 1 : 0;
	CHECK(flareMembers == 5);
}

int main() {
	TestParameterTable();
	TestCircleOfConfusion();
	TestZoneMode();
	TestIrisShapes();
	TestKernel();
	TestHighlightBoost();
	TestMasks();
	TestPresets();
	TestFlareGateAndIsolation();
	TestFlareBorderAndLimiter();
	TestGhostGeometry();
	TestFlarePalettes();
	TestShaderSource();
	TestConstantBufferLockStep();
	if (g_failures) {
		std::printf("%d lens math test failure(s)\n", g_failures);
		return 1;
	}
	std::printf("lens math tests passed\n");
	return 0;
}
