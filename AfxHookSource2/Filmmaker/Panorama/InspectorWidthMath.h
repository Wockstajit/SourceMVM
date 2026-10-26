#pragma once

// The inspector column's width rules, engine-free: InspectorWidth.cpp reads the stored value through
// the settings store and applies these, and the CS:S hook's workspace layout applies the same ones,
// so the column is the same width in both games.

namespace Filmmaker {
namespace InspectorWidthMath {

// 600: below this the effect rail (200px) leaves the pane too narrow for a one-line slider row
// (130px label + bar + 66px value) -- the bar shrinks to nothing rather than the layout scaling.
constexpr double kMin = 600.0;
// 1100: a sanity stop on the STORED value only. The real limit is the screen, applied in
// Resolve() -- a width saved on a 4K screen must not wedge the column off a 1080p one.
constexpr double kMax = 1100.0;
// The rail + pane layout's design width (Docs/features/filmmaker_config_panel.md).
constexpr double kDefault = 640.0;
// At most this much of the screen. The column has to leave a preview worth looking at, and the
// docked timeline bar takes the remainder -- past this the bar is too narrow to scrub.
constexpr double kMaxScreenFraction = 0.62;

inline double Clamp(double v, double lo, double hi) {
	if (!(v == v)) return kDefault;   // NaN from a corrupt stored value
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// A stored width, clamped to [kMin, kMax].
inline double ClampStored(double stored) { return Clamp(stored, kMin, kMax); }

// The width to lay out at on a screen screenW stage px wide: the stored width, capped so the column
// can never take the whole screen. No usable screen width yet means no screen cap.
inline double Resolve(double stored, double screenW) {
	if (!(screenW > 1.0)) return stored;
	const double screenCap = screenW * kMaxScreenFraction;
	// The floor wins over the cap on a very narrow screen: a column below kMin is a broken
	// layout, whereas a column wider than the fraction is merely cramped.
	if (screenCap <= kMin) return kMin;
	return Clamp(stored, kMin, screenCap);
}

} // namespace InspectorWidthMath
} // namespace Filmmaker
