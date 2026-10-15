#pragma once

// LensFxMath: the engine-free half of the lens module (depth of field, bokeh, lens flare).
//
// THE PARAMETER TABLE IS THE CONTRACT. Every tunable is one row of LENS_PARAMS below, and that
// single list drives the enum, the console command (`mirv_filmmaker lens set <key> <value>`), the
// clamp on every write, the JSON the Panorama panel reads, and the constant buffer the shaders
// see. A new knob is one row, not four files -- and nothing can be settable from the console but
// missing from the panel, or the reverse.
//
// THE GEOMETRY HERE IS MIRRORED IN LensFxShaders.h. The circle of confusion, the iris boundary
// functions and the sample spiral are written twice on purpose (C++ so they can be unit-tested,
// HLSL so they can run per pixel); lens-fx-math-tests.cpp pins the C++ side and a static test
// checks the shader still carries the same constants.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Filmmaker::Lens {

// X(name, key, min, max, default, isInteger)
#define LENS_PARAMS(X) \
	/* ---- DEPTH OF FIELD: on/off, what is sharp ---- */ \
	X(DofOn,          "dof.on",          0,     1,     0,     true)  \
	X(DofMode,        "dof.mode",        0,     1,     0,     true)  /* 0 lens (focus + f-stop), 1 near/far zones */ \
	X(DofAuto,        "dof.auto",        0,     1,     0,     true)  /* focus on whatever is at the middle of the screen */ \
	X(DofFocus,       "dof.focus",       10,    30000, 400,   false) /* world units */ \
	X(DofFstop,       "dof.fstop",       0.7f,  32,    2.0f,  false) \
	X(DofMaxBlur,     "dof.maxBlur",     1,     90,    40,    false) /* px at 1080p, the largest disc */ \
	/* ---- zone lens: absolute distances in world units ---- */ \
	X(DofNearStart,   "dof.nearStart",   0,     20000, 60,    false) /* fully blurred at or closer than this */ \
	X(DofNearEnd,     "dof.nearEnd",     0,     20000, 180,   false) /* sharp from here ... */ \
	X(DofFarStart,    "dof.farStart",    0,     30000, 600,   false) /* ... to here */ \
	X(DofFarEnd,      "dof.farEnd",      0,     30000, 2400,  false) /* fully blurred at or beyond this */ \
	/* ---- the iris: what a point of light turns into ---- */ \
	X(DofShape,       "dof.shape",       0,     5,     0,     true)  /* IrisShape */ \
	X(DofBlades,      "dof.blades",      3,     12,    6,     true) \
	X(DofRotation,    "dof.rotation",    -180,  180,   0,     false) \
	X(DofRoundness,   "dof.roundness",   0,     1,     0.12f, false) /* 0 hard blades, 1 circle */ \
	X(DofSqueeze,     "dof.squeeze",     1,     3,     1,     false) /* anamorphic: vertical ovals */ \
	X(DofEdge,        "dof.edge",        0,     3,     0,     false) /* bright rim, like a spherical-aberration disc */ \
	/* ---- the bokeh gamma: how far small bright lights are lifted before they are blurred ---- */ \
	X(DofGamma,       "dof.gamma",       0,     12,    6,     false) /* lift at pure white, 1.75 stops per unit (HighlightBoost) */ \
	X(DofQuality,     "dof.quality",     0,     2,     1,     true)  /* 32 / 64 / 128 taps */ \
	X(DofWeapon,      "dof.weapon",      0,     1,     0,     true)  /* the viewmodel is blurred too */ \
	X(DofDebug,       "dof.debug",       0,     2,     0,     true)  /* 1 blur map, 2 focus plane */ \
	/* ---- LENS FLARE: fourteen controls. The two tints are indices into the palettes below. ---- */ \
	X(FlareOn,        "flare.on",        0,     1,     0,     true)  \
	X(FlareIntensity, "flare.intensity", 0,     2,     1,     false) /* how strong the whole flare is; it never blows past white */ \
	X(FlareThreshold, "flare.threshold", 0.75f, 1,     0.85f, false) /* luminance a hot spot must reach: ~0.95 only white lamps, 1.0 nothing */ \
	X(FlareGhosts,    "flare.ghosts",    0,     8,     5,     true)  /* reflections along the line through the middle of the frame */ \
	X(FlareHalo,      "flare.halo",      0,     2,     0.5f,  false) /* the ring around the middle of the frame */ \
	X(FlareStreak,    "flare.streak",    0,     3,     0.8f,  false) /* the anamorphic horizontal streak */ \
	X(FlareStreakLen, "flare.streakLen", 0.05f, 1,     0.6f,  false) \
	X(FlareStar,      "flare.star",      0,     3,     0.5f,  false) /* diffraction spikes */ \
	X(FlareBlades,    "flare.blades",    3,     12,    6,     true)  /* aperture blades: ghost shape and spike count */ \
	X(FlareGlow,      "flare.glow",      0,     3,     0.5f,  false) /* veiling glare around each source */ \
	X(FlareDirt,      "flare.dirt",      0,     1,     0.2f,  false) /* smudges on the front element */ \
	X(FlareTint,      "flare.tint",      0,     7,     0,     true)  /* FlareTintColor palette index */ \
	X(FlareStreakTint,"flare.streakTint",0,     5,     0,     true)  /* StreakTintColor palette index */ \
	X(FlareDebug,     "flare.debug",     0,     2,     0,     true)  /* 1 flare only, no scene; 2 the flare budget (red = near-white area x 10, green = budget) */

enum Param : int {
#define X(name, key, lo, hi, def, integer) name,
	LENS_PARAMS(X)
#undef X
	ParamCount
};

struct ParamSpec {
	const char* key;
	float min, max, def;
	bool integer;
};

inline const ParamSpec* Specs() {
	static const ParamSpec s_specs[ParamCount] = {
#define X(name, key, lo, hi, def, integer) { key, (float)(lo), (float)(hi), (float)(def), integer },
		LENS_PARAMS(X)
#undef X
	};
	return s_specs;
}

// -1 when the key is not a lens parameter .
inline int FindParam(const char* key) {
	if (!key) return -1;
	for (int i = 0; i < ParamCount; ++i)
		if (0 == std::strcmp(Specs()[i].key, key)) return i;
	return -1;
}

// Clamp (and round, for the integer rows) a written value into the parameter's range. NaN falls
// back to the default rather than poisoning a constant buffer.
inline float ClampParam(int p, double v) {
	const ParamSpec& s = Specs()[p];
	if (!(v == v)) return s.def;
	float f = (float)(std::min<double>)((std::max<double>)(v, s.min), s.max);
	if (s.integer) f = std::floor(f + 0.5f);
	return f;
}

struct Values {
	std::array<float, ParamCount> v;
	Values() { Reset(); }
	void Reset() { for (int i = 0; i < ParamCount; ++i) v[i] = Specs()[i].def; }
	float operator[](int p) const { return v[p]; }
	float& operator[](int p) { return v[p]; }
	bool IsDefault() const {
		for (int i = 0; i < ParamCount; ++i) if (v[i] != Specs()[i].def) return false;
		return true;
	}
};

enum IrisShape : int {
	IrisCircle = 0,   // spherical: an even, round disc
	IrisPolygon = 1,  // n aperture blades
	IrisStar = 2,
	IrisHeart = 3,
	IrisRing = 4,     // mirror / catadioptric lens: a disc with a hole
	IrisCustom = 5,   // a black-and-white image the user supplies
	IrisShapeCount
};

constexpr double kPi = 3.14159265358979323846;
constexpr float kGoldenAngle = 2.39996322972865332f;

// World units are inches (CS2), so one unit is 25.4 mm.
constexpr float kMillimetresPerUnit = 25.4f;
constexpr float kSensorWidthMm = 36.0f;    // full-frame; only sets how strong a given f-stop looks
constexpr float kReferenceHeightPx = 1080; // dof.maxBlur is expressed at this height
constexpr float kFarSky = 60000.0f;        // distance assigned to sky / the 3D skybox

// Settings that used to be sliders and are now fixed. The HLSL carries the same numbers (a static
// test in lens-fx-math-tests.cpp compares them), so change both or neither.
constexpr float kFocalLengthMm = 50.0f;    // the lens is a 50 mm; blur strength is set by the f-stop
constexpr float kAutofocusX = 0.5f;        // autofocus samples the middle of the screen ...
constexpr float kAutofocusY = 0.5f;
constexpr float kAutofocusPull = 6.0f;     // ... and eases toward it at this rate (1/s)
constexpr float kWeaponDistance = 30.0f;   // where the first-person weapon counts as being, in units
constexpr float kEdgeSoftness = 0.22f;     // width of the bright rim (dof.edge), as a fraction of the radius
constexpr float kRingHole = 0.55f;         // size of the hole of the Ring iris, as a fraction of the radius
constexpr int   kCocMipLevels = 6;         // mip chain of the linear-light image the gather reads from
constexpr float kGatherFootprint = 0.9f;   // tap footprint as a multiple of the tap spacing (see GatherLod)
constexpr float kGatherJitter = 1.0f;      // per-pixel rotation of the tap spiral, 0 (fixed) to 1 (white noise); a fixed pattern leaves a lattice of dots

// ---- circle of confusion ------------------------------------------------------------------

// Signed blur radius in PIXELS of an image `heightPx` tall for a surface `distance` units from
// the camera (distance along the view axis, not radial). Positive = behind the focus (far
// field), negative = in front of it (near field). Radius, not diameter: it is what the gather
// loop compares an offset against.
inline float CircleOfConfusion(const Values& L, float distance, float focus, float widthPx,
	float heightPx) {
	const float maxPx = L[DofMaxBlur] * heightPx / kReferenceHeightPx;
	float signedPx = 0.0f;
	if ((int)L[DofMode] == 0) {
		// Thin lens: c = f^2 * |d - s| / (N * d * (s - f)), all in millimetres, c is the diameter
		// on the sensor. That is turned into pixels through the sensor width and halved.
		const float f = kFocalLengthMm;
		const float s = (std::max)(focus * kMillimetresPerUnit, f * 1.05f);
		const float d = (std::max)(distance * kMillimetresPerUnit, 1.0f);
		const float cMm = f * f * std::fabs(d - s) / (L[DofFstop] * d * (s - f));
		const float radiusPx = 0.5f * cMm / kSensorWidthMm * widthPx;
		signedPx = (d >= s) ? radiusPx : -radiusPx;
	} else {
		const float nearStart = L[DofNearStart];
		const float nearEnd = (std::max)(L[DofNearEnd], nearStart + 1.0f);
		const float farStart = (std::max)(L[DofFarStart], nearEnd);
		const float farEnd = (std::max)(L[DofFarEnd], farStart + 1.0f);
		auto smooth = [](float t) { t = (std::min)((std::max)(t, 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); };
		if (distance < nearEnd)
			signedPx = -smooth((nearEnd - distance) / (nearEnd - nearStart)) * maxPx;
		else if (distance > farStart)
			signedPx = smooth((distance - farStart) / (farEnd - farStart)) * maxPx;
	}
	return (std::min)((std::max)(signedPx, -maxPx), maxPx);
}

// ---- the bokeh gamma ----------------------------------------------------------------------
//
// WHY THIS EXISTS. The frame arrives tonemapped, so the brightest lamp in it is only 1.0. Spread
// over a disc 50 pixels across, 1.0 x (lamp area / disc area) is a faint ghost, and the shape of
// the iris never shows. A real photograph (or an After Effects bokeh with its gamma raised) keeps
// the lamp's full radiance, thousands of times brighter than paper white, so the disc stays bright
// all the way to its edge. HighlightBoost puts that range back: before the image is blurred, small
// bright lights are multiplied by 2^gamma at pure white.
//
// It is applied at full resolution, to the pixel's own colour, BEFORE the mip chain is built --
// lifting after the mips (an 8x8 average) tests a lamp that has already been mixed with its dark
// neighbours and misses it.

// Peak channel of the DISPLAY colour, raised to the sixth power: only near-clipped pixels count, so
// a lit wall at 0.7 barely moves and a lamp at 1.0 gets the whole lift. Peak, not luma: a saturated
// neon sign (1, 0, 0) is a light too.
inline float HighlightKey(float peak) {
	const float p = (std::min)((std::max)(peak, 0.0f), 1.0f);
	return p * p * p * p * p * p;
}

// In-focus pixels are never lifted: the lift ramps in over the first three pixels of blur.
inline float HighlightFocusRamp(float coc) {
	return (std::min)((std::max)((std::fabs(coc) - 0.5f) / 2.5f, 0.0f), 1.0f);
}

// How far from the pixel the isolation test looks: the inner ring sits at 0.6 of the pixel's own
// blur radius (the outer one at kIsolationOuter times that), so a light counts as "small" when it
// is small compared with the disc it is about to become.
constexpr float kIsolationOuter = 1.8f;
inline float IsolationRadius(float coc, float heightPx) {
	return (std::min)((std::max)(0.6f * std::fabs(coc), 2.0f), 24.0f * heightPx / kReferenceHeightPx);
}

// A tonemapped lamp is never alone on black: it is a clipped core inside its own glow, and the
// glow is what a ring at the core's edge sees. The test therefore takes the DARKER of two rings
// (inner and outer), so a lamp whose glow is wide still finds dark air beyond it, while a lit wall
// or the sky (bright at both radii) finds none.
inline float IsolationRingMean(float innerMean, float outerMean) {
	return (std::min)(innerMean, outerMean);
}

// 1 for a light on a much darker surround, 0 for a pixel that is as bright as what surrounds it (a
// lit wall, the sky). Without it a raised gamma would white-out every bright surface instead of
// just the lights. A glowing lamp (ring at 0.8 of its peak) counts for about 0.6.
inline float HighlightIsolation(float peak, float ringMean) {
	const float q = ringMean / (std::max)(peak, 1e-3f);
	return (std::min)((std::max)((0.95f - q) / 0.25f, 0.0f), 1.0f);
}

// The factor the LINEAR colour of a pixel is multiplied by. gamma is dof.gamma (stops at pure white).
// Each unit of gamma is kLiftStopsPerGamma stops (a 3 to 6 pixel lamp fills only a few percent of its own disc, so
// one stop per unit left gamma 6 a faint ghost); the lift stops at kMaxLiftStops, which keeps peak * 2^stops inside a half float.
constexpr float kLiftStopsPerGamma = 1.75f;
constexpr float kMaxLiftStops = 15.0f;
inline float HighlightBoost(float gamma, float peak, float isolation, float coc) {
	const float stops = gamma * kLiftStopsPerGamma * HighlightKey(peak) * HighlightFocusRamp(coc) * isolation;
	return std::exp2((std::min)(stops, kMaxLiftStops));
}

// Which mip level a gather tap reads. Taps sit about `spacing` apart (the disc's area over the tap
// count); reading a mip whose texels are about that wide makes every tap average its whole cell,
// so a small light is hit by SOME tap wherever it sits and the disc comes out solid instead of
// speckled. Narrower (the old 0.6) speckles; wider softens the edge of the iris.
constexpr float kRingLodBias = 1.0f;       // the Ring iris reads this many mips finer: the spill of a very bright light through a coarse tap filled its hole
inline float GatherLod(float radiusPx, int taps, int shape = IrisCircle) {
	const float spacing = radiusPx * std::sqrt((float)kPi / (float)taps);
	float lod = std::log2((std::max)(spacing * kGatherFootprint, 1.0f));
	if (shape == IrisRing) lod -= kRingLodBias;
	return (std::min)((std::max)(lod, 0.0f), (float)(kCocMipLevels - 1));
}

// ---- iris boundary ------------------------------------------------------------------------

// Distance from the centre to the edge of a regular n-gon (circumradius 1) at polar angle
// `theta`, with a vertex at `rotation`. 1 at a vertex, cos(pi/n) in the middle of a side.
inline float PolygonRadius(float theta, int blades, float rotation) {
	const float sector = (float)(2.0 * kPi) / (float)blades;
	float t = std::fmod(theta - rotation, sector);
	if (t < 0.0f) t += sector;
	return std::cos((float)kPi / (float)blades) / std::cos(t - sector * 0.5f);
}

// Blend between hard blades and a circle. Real iris blades are arcs, so a slight roundness is the
// natural look and 0 is the harsh CG polygon.
inline float RoundedPolygonRadius(float theta, int blades, float rotation, float roundness) {
	const float p = PolygonRadius(theta, blades, rotation);
	return p + (1.0f - p) * roundness;
}

inline float StarRadius(float theta, int points, float rotation) {
	const float k = 0.5f + 0.5f * std::cos((float)points * (theta - rotation));
	// Inner radius 0.4, and a slightly concave flank so the points read as points.
	return 0.4f + 0.6f * std::pow(k, 1.6f);
}

// Polar heart, normalised so the furthest point is at radius 1. `theta` is a SCREEN angle (x right,
// y DOWN, as the gather uses it), so with rotation 0 the tip is at theta = +pi/2 (down the screen)
// and the notch at -pi/2 (top).
// The polar heart is drawn about its notch. Its area centroid sits kHeartCentroid away from that point
// (measured numerically, in the units of HeartRadius) and its furthest point from the centroid is 0.7324,
// so the gather moves the centroid onto the tap centre and multiplies by kHeartScale (a heart about
// 1.21 radii across from its centre, area 0.68 of the unit disc: the same visual size as a hexagon).
constexpr float kHeartCentroid = 0.26759f;
constexpr float kHeartScale = 1.65f;
inline float HeartRadius(float theta, float rotation) {
	const float t = theta - rotation;
	const float s = -std::sin(t); // the textbook polar heart has y up; the screen has y down
	const float r = 2.0f - 2.0f * s + s * std::sqrt(std::fabs(std::cos(t))) / (s + 1.4f);
	return (std::max)(r, 0.0f) * 0.25f;
}

// Boundary radius of the KERNEL for the shapes that have one; Circle, Ring and Custom are handled by
// weights or a mask instead, so they are a plain unit circle here.
//
// ORIENTATION. The gather is "for this pixel, look at the pixels around it": a lamp at L lights the
// pixel at L + d when the kernel has a tap at L - (L + d) = -d. So the disc on screen is the kernel
// reflected through its centre (a half turn). The shapes below are DESIGNED in screen space (a
// polygon and a star have a point at the top, the heart's tip points down, y is down, and a positive
// rotation turns the shape clockwise on screen); `phi` is the screen angle the kernel angle maps to.
inline float IrisRadius(const Values& L, float theta) {
	const float rot = L[DofRotation] * (float)(kPi / 180.0);
	const float phi = theta + (float)kPi;
	const float top = -(float)(kPi * 0.5); // screen angle of "up"
	switch ((int)L[DofShape]) {
	case IrisPolygon: return RoundedPolygonRadius(phi, (int)L[DofBlades], rot + top, L[DofRoundness]);
	case IrisStar:    return StarRadius(phi, (int)L[DofBlades], rot + top);
	case IrisHeart:   return HeartRadius(phi, rot);
	default:          return 1.0f;
	}
}

// ---- gather kernel ------------------------------------------------------------------------

struct KernelTap {
	float x, y;   // offset in units of the gather radius, already shaped and squeezed
	float radius; // radius in DISC space (0..1) -- what the reach test compares against
	float weight; // shape weight (rim, hole); 1 for a flat disc
};

// Tap `i` of `n`. `jitter` (0..1) rotates the spiral per pixel so the taps do not band. The
// point is generated in a unit disc and pushed out to the iris boundary at its own angle, so a
// pixel is never asked for a tap outside the shape.
inline KernelTap KernelSample(const Values& L, int i, int n, float jitter) {
	const float u = std::sqrt(((float)i + 0.5f) / (float)n);
	const float theta = (float)i * kGoldenAngle + jitter * (float)(2.0 * kPi);
	const float boundary = IrisRadius(L, theta);
	const float r = u * boundary;
	float x = r * std::cos(theta), y = r * std::sin(theta);
	if ((int)L[DofShape] == IrisHeart) {
		// Centre the heart on the tap centre (its curve is drawn about the notch) and size it like the other shapes.
		const float rot = L[DofRotation] * (float)(kPi / 180.0);
		x = (x - std::sin(rot) * kHeartCentroid) * kHeartScale;
		y = (y + std::cos(rot) * kHeartCentroid) * kHeartScale;
	}

	// Anamorphic squeeze keeps the disc's area: wider lens, taller and narrower ovals.
	const float sq = std::sqrt(L[DofSqueeze]);
	x /= sq; y *= sq;

	float w = 1.0f;
	// Points pushed toward the boundary are denser near the middle of a side; giving them
	// 1/boundary^2 brings the mean density back to flat.
	w *= boundary * boundary;
	const int shape = (int)L[DofShape];
	if (shape == IrisRing) {
		const float t = (std::min)((std::max)((u - (kRingHole - 0.06f)) / 0.12f, 0.0f), 1.0f);
		w *= t * t * (3.0f - 2.0f * t);
	}
	if (L[DofEdge] > 0.0f) {
		const float soft = kEdgeSoftness;
		const float t = (std::min)((std::max)((u - (1.0f - soft)) / soft, 0.0f), 1.0f);
		w *= 1.0f + L[DofEdge] * t * t * (3.0f - 2.0f * t);
	}
	return { x, y, u, w };
}

inline int TapCount(const Values& L) {
	static const int counts[3] = { 48, 96, 192 };
	return counts[(std::min)((std::max)((int)L[DofQuality], 0), 2)];
}

// ---- built-in custom iris masks -------------------------------------------------------------

// Shapes that are not a boundary function of the angle (holes, several lobes), so they are
// described as a coverage test over the unit square instead. x, y in [-1, 1]. The GPU reads
// them from a small image; this is what fills it, and what the tests pin.
enum IrisMaskId : int {
	MaskDiamond = 0,
	MaskCross,
	MaskCrescent,
	MaskClover,
	MaskSmile,
	MaskCount
};

inline const char* IrisMaskName(int m) {
	static const char* names[MaskCount] = { "diamond", "cross", "crescent", "clover", "smile" };
	return (m >= 0 && m < MaskCount) ? names[m] : "";
}
inline int IrisMaskFromName(const char* name) {
	if (!name) return -1;
	for (int i = 0; i < MaskCount; ++i)
		if (0 == std::strcmp(IrisMaskName(i), name)) return i;
	return -1;
}

inline float IrisMaskCoverage(int mask, float x, float y) {
	auto inside = [](float d) { return d <= 0.0f ? 1.0f : 0.0f; }; // d = signed distance
	auto disc = [](float px, float py, float cx, float cy, float r) {
		return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
	};
	switch (mask) {
	case MaskDiamond: return inside(std::fabs(x) + std::fabs(y) - 0.92f);
	case MaskCross:
		return inside((std::min)((std::max)(std::fabs(x) - 0.3f, std::fabs(y) - 0.92f),
			(std::max)(std::fabs(y) - 0.3f, std::fabs(x) - 0.92f)));
	case MaskCrescent:
		return inside((std::max)(disc(x, y, 0.0f, 0.0f, 0.92f), -disc(x, y, 0.38f, 0.08f, 0.78f)));
	case MaskClover: {
		const float d = (std::min)((std::min)(disc(x, y, 0.42f, 0.0f, 0.5f), disc(x, y, -0.42f, 0.0f, 0.5f)),
			(std::min)(disc(x, y, 0.0f, 0.42f, 0.5f), disc(x, y, 0.0f, -0.42f, 0.5f)));
		return inside(d);
	}
	case MaskSmile: {
		const float face = disc(x, y, 0.0f, 0.0f, 0.92f);
		const float eyes = (std::min)(disc(x, y, -0.32f, 0.3f, 0.13f), disc(x, y, 0.32f, 0.3f, 0.13f));
		const float r = std::sqrt(x * x + y * y);
		const float mouth = (y < -0.05f) ? (std::max)(std::fabs(r - 0.55f) - 0.07f, 0.0f) - 0.0f : 1.0f;
		const float mouthHole = (y < -0.05f && std::fabs(r - 0.55f) <= 0.07f) ? 1.0f : 0.0f;
		(void)mouth;
		return (face <= 0.0f && eyes > 0.0f && mouthHole < 0.5f) ? 1.0f : 0.0f;
	}
	default: return 1.0f;
	}
}

// ---- lens flare ---------------------------------------------------------------------------
//
// HOW THE FLARE STAYS TASTEFUL. The old flare treated every saturated colour as a light, boosted it,
// let every layer multiply it again and then clipped, so its energy followed the AREA of whatever was
// bright and a blue sky or a lit doorway threw a frame-sized, hard-edged copy of itself. The rules now:
//
//   1. A source is a LUMINANCE (FlareGate): a fixed soft knee above the user's threshold. At 0.95
//      only near-white pixels pass, at 1.0 nothing does, and raising the threshold only ever removes light.
//   2. A source must be SMALL (FlareIsolation): a ring of the frame around it, about 4.5 % of the frame
//      height out, must be clearly darker than the source itself. A lamp, the sun's disc or a specular
//      hot spot qualifies; a bright wall, the sky or a lit doorway is as bright as its own ring and is
//      dropped, so the flare's energy never follows the area of a bright region. The test looks only at
//      the frame, never at the threshold, so raising the threshold can only remove flare.
//   3. Sources fade out over the last 5 % of the frame (FlareBorderFade) and a ghost is windowed by
//      the position it is sampled from, so no copy of the frame ever ends in a straight edge.
//   4. Each layer saturates on its own (FlareLimit / the blob and streak gains) and the composite
//      combines them with a bounded blend, so intensity 1 cannot white out a frame.
//
// Everything below is mirrored in the HLSL (kShaderFlare / kShaderComposite); a static test compares
// the numbers.

constexpr float kFlareCeil = 1.05f;        // luminance at which a source counts fully: the gate ramps from the threshold up to here
constexpr float kFlareBudgetArea = 0.01f;  // fraction of the frame that is near-white up to which the flare is at full strength
constexpr float kFlareBudgetPow = 0.6f;    // how fast it falls beyond that: (kFlareBudgetArea / area) ^ pow
constexpr float kFlareBudgetLo = 0.75f;    // a pixel counts toward that area from this luminance ...
constexpr float kFlareBudgetHi = 0.95f;    // ... and fully from this one
constexpr float kFlareWideRadius = 0.13f;  // radius of the wide isolation ring, fraction of frame height
constexpr float kFlareWideLo = 0.75f;      // a wide-ring tap starts to count as "bright area" at this fraction of the source's peak ...
constexpr float kFlareWideHi = 0.95f;      // ... and counts fully at this one
constexpr float kFlareWideIsoLo = 0.30f;   // mean wide-ring weight at or below which a source counts fully ...
constexpr float kFlareWideIsoHi = 0.70f;   // ... and at or above which it is inside a big bright area
constexpr float kFlareFadeSpan = 0.20f;    // below threshold 1 - span the whole flare is at full strength; it falls to nothing at 1.0
constexpr float kFlareIsoRadius = 0.045f;  // radius of the isolation ring, fraction of frame height
constexpr float kFlareIsoLo = 0.04f;       // ring brightness at or below which a source counts fully ...
constexpr float kFlareIsoHi = 0.20f;       // ... and at or above which it belongs to a big bright area (a corner is 0.25, an edge 0.5)
constexpr float kFlareBorder = 0.05f;      // fade-out distance at the frame edge, fraction of frame height
constexpr float kFlareRelMargin = 0.06f;   // a ring tap counts as "as bright as the source" from this far below the source's peak ...
constexpr float kFlareRelSoft = 0.06f;     // ... fading in over this much more
constexpr float kFlareMinLo = 0.10f;      // a source whose (quarter-resolution) luminance is below this is a speck, not a light ...
constexpr float kFlareMinHi = 0.30f;      // ... and counts fully from here
constexpr float kGhostRadius = 0.04f;      // radius of the blob a source becomes, fraction of frame width
constexpr float kGhostBlobGain = 50.0f;    // how quickly a blob saturates: 1 - exp(-gain * mean source)
constexpr float kBladeRoundness = 0.15f;   // curvature of the ghost blob's blades (0 hard polygon, 1 circle)
constexpr float kGhostGain = 1.2f;         // strength of the ghost layer
constexpr float kGhostFringe = 0.012f;     // red/blue split of a ghost, fraction of its distance from the centre
constexpr float kHaloGain = 0.25f;         // strength of the halo layer
constexpr float kHaloFringe = 0.02f;       // red/blue split of the halo ring, fraction of its radius
constexpr float kStreakGain = 260.0f;      // saturation gain of the streak line integral
constexpr float kSpikeGain = 200.0f;       // saturation gain of the diffraction spikes
constexpr float kStreakSoft = 6.0f;        // Gaussian cross-width filter radius in full-resolution pixels
constexpr float kLineTaper = 0.5f;         // the streak and spike integrals fade to zero over the last half of their reach
// Weight of a tap u (0 at the source, 1 at the end of the reach) along a streak or spike line. The gain that
// follows saturates even a tiny sum, so a tail that merely fell off exponentially ended in a visible cut.
inline float LineTailWeight(float u, float falloff) {
	const float t = (std::min)((std::max)((u - kLineTaper) / (1.0f - kLineTaper), 0.0f), 1.0f);
	return std::exp(-falloff * u) * (1.0f - t * t * (3.0f - 2.0f * t));
}
constexpr float kGlowGain = 90.0f;         // saturation gain of the veiling glow
constexpr float kDirtBoost = 3.0f;         // how much a fully dirty spot multiplies the flare at dirt = 1

inline float SmoothStep(float a, float b, float x) {
	const float t = (std::min)((std::max)((x - a) / (b - a), 0.0f), 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

inline float Luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// How much of a pixel counts as light: 0 at or below the threshold, 1 a knee above it. Depends on
// luminance, not on the brightest channel, so a saturated blue sky (0.3, 0.5, 1.0) is not a source.
// The knee is fixed; a threshold of 1.0 lets nothing through because luminance cannot exceed 1.
inline float FlareGate(float luma, float threshold) {
	return SmoothStep(threshold, kFlareCeil, luma);
}

// The threshold is also the flare's sensitivity: a saturating layer hides how much light a source gives (a
// pixel at luminance 0.9 makes almost the same blob as one at 1.0), so on a frame whose lights are all near
// white the gate alone acts like a switch. This factor, applied to the intensity, makes the whole flare fall
// away smoothly to nothing as the threshold rises from 1 - kFlareFadeSpan to 1.0.
// A frame with a lot of near-white area (an opening full of sky, a whole lit wall) must not get a streak and
// glare from all of it: the flare is a budget. `area` is the fraction of the frame that is near-white (a soft
// step on plain luminance, FlareBudgetWeight); the budget is 1 up to kFlareBudgetArea, then falls smoothly.
// It reads only the frame, never the threshold, so the threshold sweep stays monotone.
inline float FlareBudgetWeight(float luma) { return SmoothStep(kFlareBudgetLo, kFlareBudgetHi, luma); }
inline float FlareBudget(float area) {
	return std::pow((std::min)((std::max)(kFlareBudgetArea / (std::max)(area, 1e-9f), 0.0f), 1.0f), kFlareBudgetPow);
}
inline float FlareThresholdFade(float threshold) {
	return SmoothStep(0.0f, 1.0f, (std::min)((std::max)((1.0f - threshold) / kFlareFadeSpan, 0.0f), 1.0f));
}

// Does one tap of the isolation ring count as "as bright as the source"? `peak` is the source's own peak
// luminance, `ringLuma` the (blurred) luminance at the tap. Neither depends on the threshold: the
// threshold decides how much light a source gives, never whether it is judged to be a point or an area.
// That is what makes raising the threshold monotone -- it can only take flare away.
inline float FlareRingWeight(float peak, float ringLuma) {
	return SmoothStep(peak - kFlareRelMargin - kFlareRelSoft, peak - kFlareRelMargin, ringLuma);
}

// 1 for a source on a darker surround, 0 for a pixel that is part of a big bright area. `ringMean` is
// the mean FlareRingWeight over the ring kFlareIsoRadius around it (a straight edge of a big area
// reads 0.5, a corner 0.25).
inline float FlareIsolation(float ringMean) {
	return 1.0f - SmoothStep(kFlareIsoLo, kFlareIsoHi, ringMean);
}

// The same test one scale up (a ring kFlareWideRadius out): a tap counts when it is nearly as bright as the
// source's own peak, and a source whose wide ring is mostly such taps sits inside a big bright area. A
// sun in a blue sky (sky well below the sun) is kept; a blown-out opening full of sky is dropped.
inline float FlareWideRingWeight(float peak, float ringLuma) {
	return SmoothStep(peak * kFlareWideLo, peak * kFlareWideHi, ringLuma);
}
inline float FlareWideIsolation(float wideMean) {
	return 1.0f - SmoothStep(kFlareWideIsoLo, kFlareWideIsoHi, wideMean);
}

// Drops specks: after the bright pass has been averaged down to quarter resolution, a lamp a few pixels
// wide still reads 0.3 or more, a sparkle one or two pixels wide reads far less. Monotone in the
// threshold like everything else on the source path.
inline float FlareSourceFloor(float sourceLuma) { return SmoothStep(kFlareMinLo, kFlareMinHi, sourceLuma); }

// Fade toward the frame edge. dx / dy are the distances to the nearest vertical / horizontal edge in
// units of frame height; a negative distance (outside the frame) is 0.
inline float FlareBorderFade(float dx, float dy) {
	return SmoothStep(0.0f, kFlareBorder, dx) * SmoothStep(0.0f, kFlareBorder, dy);
}
inline float FlareBorderFadeUv(float u, float v, float aspect) {
	return FlareBorderFade((std::min)(u, 1.0f - u) * aspect, (std::min)(v, 1.0f - v));
}

// The soft limiter every layer (and the composite) goes through: linear for small values, never past 1.
inline float FlareLimit(float x) { return 1.0f - std::exp(-(std::max)(x, 0.0f)); }

// Where ghost `k` (0-based) lands, as a multiple of a source's offset from the frame centre. Negative
// scales are mirrored through the optical axis, positive ones sit between the source and the axis
// (or just past the source): every ghost lies ON the straight line from the source through the centre.
constexpr int kMaxGhosts = 8;
inline float GhostScale(int k) {
	static const float scales[kMaxGhosts] = { -0.50f, 0.35f, -0.80f, 0.62f, -1.10f, 0.85f, -1.45f, 1.20f };
	return scales[(std::min)((std::max)(k, 0), kMaxGhosts - 1)];
}
inline float GhostGainAt(int k) { return kGhostGain / (1.0f + 0.3f * (float)k); }
// Ghosts of a source far from the axis are weaker: `r` is the distance of the point they are sampled
// from to the frame centre, in frame heights.
inline float GhostFade(float r) { return 1.0f - 0.7f * SmoothStep(0.4f, 1.1f, r); }

// A hexagonal aperture (six blades, three lines) makes six spikes; an odd count makes twice as many
// as it has blades. The first line is vertical, so a horizontal streak and a spike never coincide.
inline int SpikeLines(int blades) { return (blades % 2 == 0) ? blades / 2 : blades; }
inline float SpikeAngle(int j, int lines) { return (float)(kPi * 0.5) + (float)j * (float)kPi / (float)lines; }

// The halo is an angular mean of the source blobs on a circle around the frame centre, so it is a ring
// there whatever the source's direction. A source's blob covers 1/RingBlobs of that circle; multiplying
// the mean back by it keeps the ring as bright as the blob whatever its radius. `r` in frame heights.
inline float HaloRingBlobs(float r, float aspect) {
	return (float)(2.0 * kPi) * r / (2.0f * kGhostRadius * aspect);
}

// ---- palettes ---------------------------------------------------------------------------------
// flare.tint and flare.streakTint are indices into these tables (the panel shows the same colours).

struct Rgb { float r, g, b; };
constexpr int kFlareTintCount = 8, kStreakTintCount = 6;

inline const char* FlareTintName(int i) {
	static const char* names[kFlareTintCount] = { "warm", "white", "gold", "cool", "teal", "rose", "violet", "ember" };
	return (i >= 0 && i < kFlareTintCount) ? names[i] : "";
}
inline Rgb FlareTintColor(int i) {
	static const Rgb c[kFlareTintCount] = {
		{ 255 / 255.0f, 217 / 255.0f, 184 / 255.0f }, { 1.0f, 1.0f, 1.0f },                       // warm, white
		{ 255 / 255.0f, 200 / 255.0f, 87 / 255.0f },  { 184 / 255.0f, 212 / 255.0f, 255 / 255.0f }, // gold, cool
		{ 127 / 255.0f, 224 / 255.0f, 212 / 255.0f }, { 255 / 255.0f, 166 / 255.0f, 196 / 255.0f }, // teal, rose
		{ 196 / 255.0f, 166 / 255.0f, 255 / 255.0f }, { 255 / 255.0f, 138 / 255.0f, 92 / 255.0f } }; // violet, ember
	return c[(std::min)((std::max)(i, 0), kFlareTintCount - 1)];
}
inline const char* StreakTintName(int i) {
	static const char* names[kStreakTintCount] = { "blue", "cyan", "white", "amber", "red", "magenta" };
	return (i >= 0 && i < kStreakTintCount) ? names[i] : "";
}
inline Rgb StreakTintColor(int i) {
	static const Rgb c[kStreakTintCount] = {
		{ 92 / 255.0f, 148 / 255.0f, 255 / 255.0f }, { 92 / 255.0f, 240 / 255.0f, 255 / 255.0f },
		{ 1.0f, 1.0f, 1.0f },                        { 255 / 255.0f, 177 / 255.0f, 77 / 255.0f },
		{ 255 / 255.0f, 92 / 255.0f, 92 / 255.0f },  { 255 / 255.0f, 92 / 255.0f, 216 / 255.0f } };
	return c[(std::min)((std::max)(i, 0), kStreakTintCount - 1)];
}
// -1 when `name` is not in the palette (an all-digit string is not a name).
inline int FlareTintFromName(const char* name) {
	if (!name) return -1;
	for (int i = 0; i < kFlareTintCount; ++i) if (0 == std::strcmp(FlareTintName(i), name)) return i;
	return -1;
}
inline int StreakTintFromName(const char* name) {
	if (!name) return -1;
	for (int i = 0; i < kStreakTintCount; ++i) if (0 == std::strcmp(StreakTintName(i), name)) return i;
	return -1;
}

// ---- the constants the flare shaders read --------------------------------------------------------
// Five float4s of the constant buffer (flareA..flareC, flareTint, streakTint). One function so the
// game and the offline harness (Automation/verify) cannot disagree about the layout.
struct FlareConstants {
	float a[4];          // intensity, threshold, ghosts, halo
	float b[4];          // streak, streak length, star, blades
	float c[4];          // glow, dirt, dirt image loaded, flare-only debug
	float tint[4];       // rgb flare tint
	float streakTint[4]; // rgb streak tint
};
inline FlareConstants MakeFlareConstants(const Values& L, bool dirtLoaded) {
	FlareConstants k{};
	k.a[0] = L[FlareIntensity] * FlareThresholdFade(L[FlareThreshold]); k.a[1] = L[FlareThreshold]; k.a[2] = L[FlareGhosts]; k.a[3] = L[FlareHalo];
	k.b[0] = L[FlareStreak]; k.b[1] = L[FlareStreakLen]; k.b[2] = L[FlareStar]; k.b[3] = L[FlareBlades];
	k.c[0] = L[FlareGlow]; k.c[1] = L[FlareDirt]; k.c[2] = dirtLoaded ? 1.0f : 0.0f; k.c[3] = L[FlareDebug];
	const Rgb t = FlareTintColor((int)L[FlareTint]);
	k.tint[0] = t.r; k.tint[1] = t.g; k.tint[2] = t.b; k.tint[3] = 0;
	const Rgb s = StreakTintColor((int)L[FlareStreakTint]);
	k.streakTint[0] = s.r; k.streakTint[1] = s.g; k.streakTint[2] = s.b; k.streakTint[3] = 0;
	return k;
}

// ---- groups and presets -----------------------------------------------------------------------

enum class Group { Dof, Flare };

inline bool InGroup(int p, Group g) {
	const char* k = Specs()[p].key;
	return g == Group::Dof ? 0 == std::strncmp(k, "dof.", 4) : 0 == std::strncmp(k, "flare.", 6);
}

// Put one group back to its defaults. The group's own on/off switch is left where it is: a reset is
// "undo my tuning", not "turn the effect off", and the second is one click already.
inline void ResetGroup(Values& L, Group g) {
	for (int i = 0; i < ParamCount; ++i) {
		if (!InGroup(i, g)) continue;
		if (i == DofOn || i == FlareOn) continue;
		L[i] = Specs()[i].def;
	}
}

inline void SetClamped(Values& L, int p, double v) { L[p] = ClampParam(p, v); }

// A preset is a complete look for its group: it resets the group first, so applying two in a
// row never leaves a stray value from the first. Returns false for an unknown name.
inline bool ApplyDofPreset(Values& L, const char* name) {
	if (!name) return false;
	ResetGroup(L, Group::Dof);
	auto set = [&](int p, double v) { SetClamped(L, p, v); };
	// The lens is a fixed 50 mm, so blur strength comes from the f-stop alone; the looks that used a
	// longer lens before use a wider aperture now.
	if (0 == std::strcmp(name, "cinematic")) {
		set(DofMode, 0); set(DofFstop, 1.8); set(DofShape, IrisPolygon); set(DofBlades, 7);
		set(DofRoundness, 0.55); set(DofGamma, 5); set(DofQuality, 2); set(DofEdge, 0.25);
	} else if (0 == std::strcmp(name, "anamorphic")) {
		set(DofMode, 0); set(DofFstop, 1.4); set(DofShape, IrisCircle);
		set(DofSqueeze, 1.8); set(DofEdge, 0.5); set(DofGamma, 6); set(DofQuality, 2);
	} else if (0 == std::strcmp(name, "portrait")) {
		set(DofMode, 0); set(DofFstop, 1.0); set(DofShape, IrisCircle);
		set(DofEdge, 0.3); set(DofGamma, 4); set(DofQuality, 2);
	} else if (0 == std::strcmp(name, "miniature")) {
		// A narrow band in focus with hard falloff on both sides: the tilt-shift toy-town look.
		set(DofMode, 1); set(DofNearStart, 40); set(DofNearEnd, 260); set(DofFarStart, 420);
		set(DofFarEnd, 1300); set(DofMaxBlur, 30); set(DofShape, IrisCircle); set(DofGamma, 3); set(DofQuality, 1);
	} else if (0 == std::strcmp(name, "hearts")) {
		set(DofMode, 0); set(DofFstop, 0.9); set(DofShape, IrisHeart);
		set(DofGamma, 8); set(DofQuality, 2);
	} else if (0 == std::strcmp(name, "stars")) {
		set(DofMode, 0); set(DofFstop, 0.9); set(DofShape, IrisStar); set(DofBlades, 5);
		set(DofGamma, 8); set(DofQuality, 2);
	} else {
		return false;
	}
	return true;
}

inline bool ApplyFlarePreset(Values& L, const char* name) {
	if (!name) return false;
	ResetGroup(L, Group::Flare);
	auto set = [&](int p, double v) { SetClamped(L, p, v); };
	if (0 == std::strcmp(name, "cinematic")) {
		// The defaults are already this look; spelled out so a default change cannot move it.
		set(FlareGhosts, 5); set(FlareHalo, 0.5); set(FlareStreak, 0.8); set(FlareStar, 0.5);
		set(FlareGlow, 0.5); set(FlareDirt, 0.2); set(FlareTint, 0); set(FlareStreakTint, 0);
	} else if (0 == std::strcmp(name, "anamorphic")) {
		// A long cool streak and little else: the look of a cinema anamorphic lens.
		set(FlareGhosts, 2); set(FlareHalo, 0.1); set(FlareStreak, 2.0); set(FlareStreakLen, 0.9);
		set(FlareStar, 0.0); set(FlareGlow, 0.6); set(FlareDirt, 0.1); set(FlareTint, 3); set(FlareStreakTint, 0);
	} else if (0 == std::strcmp(name, "classic")) {
		// A chain of aperture-shaped ghosts and a ring, no streak or spikes: a vintage spherical lens.
		set(FlareGhosts, 8); set(FlareHalo, 1.2); set(FlareStreak, 0.0); set(FlareStar, 0.0);
		set(FlareGlow, 0.3); set(FlareBlades, 6); set(FlareDirt, 0.15); set(FlareTint, 0);
	} else if (0 == std::strcmp(name, "star")) {
		// Sharp diffraction spikes from a stopped-down lens.
		set(FlareGhosts, 1); set(FlareHalo, 0.0); set(FlareStreak, 0.0); set(FlareStar, 2.0);
		set(FlareBlades, 6); set(FlareGlow, 0.4); set(FlareTint, 1);
	} else if (0 == std::strcmp(name, "dreamy")) {
		// Soft veiling glare only, and a lower threshold so more lights qualify.
		set(FlareGhosts, 0); set(FlareHalo, 0.0); set(FlareStreak, 0.0); set(FlareStar, 0.0);
		set(FlareGlow, 2.0); set(FlareThreshold, 0.75); set(FlareDirt, 0.0); set(FlareTint, 5);
	} else {
		return false;
	}
	return true;
}

} // namespace Filmmaker::Lens
