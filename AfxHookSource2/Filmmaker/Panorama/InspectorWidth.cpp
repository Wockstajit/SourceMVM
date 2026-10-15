#include "InspectorWidth.h"

#include "../Config/FilmmakerSettingsStore.h"

#include <cstdlib>
#include <string>

namespace Filmmaker {

namespace {

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

double Clamp(double v, double lo, double hi) {
	if (!(v == v)) return kDefault;   // NaN from a corrupt stored value
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

} // namespace

double InspectorWidth_Min() { return kMin; }
double InspectorWidth_Max() { return kMax; }
double InspectorWidth_Default() { return kDefault; }

double InspectorWidth_Get() {
	const std::string stored = FilmmakerSettings_UiString("inspectorW", "");
	if (stored.empty()) return kDefault;
	// strtod, not stod: a malformed stored value must fall back to the default rather than throw
	// out of a per-frame state build.
	const char* begin = stored.c_str();
	char* end = nullptr;
	const double v = std::strtod(begin, &end);
	if (end == begin) return kDefault;
	return Clamp(v, kMin, kMax);
}

double InspectorWidth_Resolve(double screenW) {
	const double stored = InspectorWidth_Get();
	if (!(screenW > 1.0)) return stored;   // no usable screen width yet: no screen cap to apply
	const double screenCap = screenW * kMaxScreenFraction;
	// The floor wins over the cap on a very narrow screen: a column below kMin is a broken
	// layout, whereas a column wider than the fraction is merely cramped.
	if (screenCap <= kMin) return kMin;
	return Clamp(stored, kMin, screenCap);
}

void InspectorWidth_AppendJson(std::ostringstream& o) {
	o << ",\"inspW\":" << (int)(InspectorWidth_Get() + 0.5);
}

} // namespace Filmmaker
