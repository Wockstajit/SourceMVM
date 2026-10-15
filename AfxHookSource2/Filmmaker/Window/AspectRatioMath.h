#pragma once

// Aspect-ratio identification for the free window-resize overlay.
//
// Header-only and dependency-free on purpose: the same code compiles into the hook DLL and into
// the standalone AspectRatioMathTests target, and it must never pull in Windows.h (which would
// bring the min/max macros into a file full of comparisons).
//
// Two independent answers are produced for a pixel size:
//   * the EXACT reduced integer ratio (1920x1080 -> 16:9), when the reduced denominator is small
//     enough to be meaningful to a human;
//   * the CLOSEST entry in the curated table below, with a signed error, which is what a window
//     mid-drag almost always needs (2544x1431 is not any nameable ratio, but it is 16:9 to 0.1%).
//
// Closeness is measured in LOG space so the metric is symmetric: being 5% too wide and 5% too
// narrow are the same distance. Comparing raw ratio differences would bias every match toward the
// wide end of the table (the gap from 2.20 to 2.39 is numerically larger than 1.25 to 1.33 while
// being visually smaller).

#include <cmath>
#include <cstddef>

namespace Filmmaker {

enum class AspectFamily {
	Standard, // monitor / broadcast shapes
	Cinema,   // theatrical projection and film formats
	Social,   // square and vertical delivery
};

struct AspectRatioEntry {
	const char* label;  // short form for the overlay, e.g. "16:9" or "2.39:1"
	const char* name;   // human name, e.g. "Anamorphic scope"
	double ratio;       // width / height
	AspectFamily family;
};

// One entry per DISTINCT ratio -- deliberately no duplicates. 1.33:1 (silent / full frame) is
// numerically identical to 4:3, so it lives in that entry's name rather than as a second row that
// would make "closest" ambiguous. The Academy ratio is 1.375:1, NOT 1.33:1; both are listed the
// way a camera department would expect.
//
// The ultrawide band deserves a note: monitors marketed as "21:9" are not 21/9. 2560x1080 is
// 2.370:1 and 3440x1440 is 2.389:1, so a real ultrawide window matches the scope entries rather
// than the literal 21:9 row. That is the honest answer for a movie tool, and the scope entry's
// name says so.
inline const AspectRatioEntry* AspectRatio_Table(std::size_t& outCount) {
	static const AspectRatioEntry kTable[] = {
		{ "9:16",    "Vertical (mobile)",            9.0 / 16.0,  AspectFamily::Social   },
		{ "4:5",     "Portrait (social)",            4.0 / 5.0,   AspectFamily::Social   },
		{ "1:1",     "Square",                       1.0,         AspectFamily::Social   },
		{ "5:4",     "5:4",                          5.0 / 4.0,   AspectFamily::Standard },
		{ "4:3",     "Full frame / 1.33:1",          4.0 / 3.0,   AspectFamily::Standard },
		{ "1.375:1", "Academy",                      1.375,       AspectFamily::Cinema   },
		{ "1.43:1",  "IMAX film",                    1.43,        AspectFamily::Cinema   },
		{ "3:2",     "3:2 (35mm still)",             3.0 / 2.0,   AspectFamily::Standard },
		{ "16:10",   "16:10",                        16.0 / 10.0, AspectFamily::Standard },
		{ "1.66:1",  "European widescreen",          5.0 / 3.0,   AspectFamily::Cinema   },
		{ "16:9",    "Widescreen HD",                16.0 / 9.0,  AspectFamily::Standard },
		{ "1.85:1",  "US widescreen (flat)",         1.85,        AspectFamily::Cinema   },
		{ "1.90:1",  "IMAX digital",                 1.90,        AspectFamily::Cinema   },
		{ "2:1",     "Univisium",                    2.0,         AspectFamily::Cinema   },
		{ "2.20:1",  "70mm",                         2.20,        AspectFamily::Cinema   },
		{ "21:9",    "Ultrawide (literal 21:9)",     21.0 / 9.0,  AspectFamily::Standard },
		{ "2.35:1",  "CinemaScope (pre-1970)",       2.35,        AspectFamily::Cinema   },
		{ "2.39:1",  "Anamorphic scope / 21:9 UW",   2.39,        AspectFamily::Cinema   },
		{ "2.76:1",  "Ultra Panavision 70",          2.76,        AspectFamily::Cinema   },
	};
	outCount = sizeof(kTable) / sizeof(kTable[0]);
	return kTable;
}

struct AspectMatch {
	bool valid = false;              // false when width/height were not both positive
	double ratio = 0.0;              // measured width / height
	const AspectRatioEntry* entry = nullptr; // closest table row (never null when valid)
	double errorPercent = 0.0;       // signed: (ratio / entry->ratio - 1) * 100, + means wider
	bool exact = false;              // within kExactPercent of the entry
	int reducedWidth = 0;            // exact integer ratio, 0 when not worth showing
	int reducedHeight = 0;
};

// A window that lands this close to a named ratio is reported as that ratio with no error text.
// 0.2% is roughly one pixel of height at 1080p, i.e. below what anyone can act on.
inline constexpr double kAspectExactPercent = 0.2;

// The reduced integer ratio is only meaningful while the numbers stay small. 1919x1081 reduces to
// 1919:1081, which is noise -- above this denominator the exact form is suppressed and only the
// closest named ratio is reported.
inline constexpr int kAspectMaxReducedDenominator = 64;

inline int AspectRatio_Gcd(int a, int b) {
	while (b != 0) {
		const int t = a % b;
		a = b;
		b = t;
	}
	return a < 0 ? -a : a;
}

inline AspectMatch AspectRatio_Match(int width, int height) {
	AspectMatch out;
	if (width <= 0 || height <= 0)
		return out;

	out.valid = true;
	out.ratio = (double)width / (double)height;

	std::size_t count = 0;
	const AspectRatioEntry* table = AspectRatio_Table(count);

	const double measuredLog = std::log(out.ratio);
	double bestDistance = 0.0;
	for (std::size_t i = 0; i < count; ++i) {
		const double distance = std::fabs(measuredLog - std::log(table[i].ratio));
		if (out.entry == nullptr || distance < bestDistance) {
			bestDistance = distance;
			out.entry = &table[i];
		}
	}

	out.errorPercent = (out.ratio / out.entry->ratio - 1.0) * 100.0;
	out.exact = std::fabs(out.errorPercent) <= kAspectExactPercent;

	const int divisor = AspectRatio_Gcd(width, height);
	if (divisor > 0) {
		const int rw = width / divisor;
		const int rh = height / divisor;
		if (rh <= kAspectMaxReducedDenominator && rw <= kAspectMaxReducedDenominator * 8) {
			out.reducedWidth = rw;
			out.reducedHeight = rh;
		}
	}
	return out;
}

// Height that gives `ratio` at the supplied width, rounded to the NEAREST even number. Odd
// dimensions are rejected by most video encoders (yuv420p needs both axes even), so a filmmaker
// tool should never hand one out even for a window.
//
// Nearest-even, not round-then-bump-up: 3008 at 2.39:1 is 1258.58, and bumping 1259 up to 1260
// lands 1.4px away from the ratio when 1258 is 0.6px away. Rounding onto the even grid directly is
// always the closest legal answer.
inline int AspectRatio_RoundToEven(double value) {
	int rounded = 2 * (int)std::floor(value / 2.0 + 0.5);
	if (rounded < 2) rounded = 2;
	return rounded;
}

inline int AspectRatio_HeightForWidth(int width, double ratio) {
	if (width <= 0 || !(ratio > 0.0))
		return 0;
	return AspectRatio_RoundToEven((double)width / ratio);
}

inline int AspectRatio_WidthForHeight(int height, double ratio) {
	if (height <= 0 || !(ratio > 0.0))
		return 0;
	return AspectRatio_RoundToEven((double)height * ratio);
}

// The smallest whole (width, height) step that reproduces `ratio` EXACTLY, with both axes even.
//
// This is what makes a snapped size a real resolution instead of a near miss: every whole multiple
// of the step is exactly the ratio, so 16:9 quantises to 32x18 (…, 1888x1062, 1920x1080, 1952x1098)
// and never lands on 1919x1080. Only ratios that are genuinely rational with a small denominator
// have one -- 16:9, 4:3, 1.85 (37:20), 2.35 (47:20) do; 1.43 and 2.39 do not, and the caller has to
// fall back to quantising one axis and deriving the other.
//
// Both axes are forced even for the same reason AspectRatio_HeightForWidth rounds even: yuv420p
// rejects odd dimensions, and a size this tool hands out should never need fixing later.
inline bool AspectRatio_ExactStep(double ratio, int& outWidthStep, int& outHeightStep) {
	if (!(ratio > 0.0))
		return false;
	for (int denominator = 1; denominator <= kAspectMaxReducedDenominator; ++denominator) {
		const double scaled = ratio * denominator;
		const int rounded = (int)(scaled + 0.5);
		if (rounded <= 0)
			continue;
		// 1e-9 absolute: the ratios in the table are exact quotients of small integers, so a true
		// hit is within a few ULPs and the nearest false hit (2.39 at any denominator <= 64) is off
		// by more than 1e-3.
		if (std::fabs(scaled - (double)rounded) > 1e-9)
			continue;
		int stepWidth = rounded;
		int stepHeight = denominator;
		if ((stepWidth % 2) || (stepHeight % 2)) {
			stepWidth *= 2;
			stepHeight *= 2;
		}
		outWidthStep = stepWidth;
		outHeightStep = stepHeight;
		return true;
	}
	return false;
}

} // namespace Filmmaker
