#include "InspectorWidth.h"
#include "InspectorWidthMath.h"

#include "../Config/FilmmakerSettingsStore.h"

#include <cstdlib>
#include <string>

namespace Filmmaker {

double InspectorWidth_Min() { return InspectorWidthMath::kMin; }
double InspectorWidth_Max() { return InspectorWidthMath::kMax; }
double InspectorWidth_Default() { return InspectorWidthMath::kDefault; }

double InspectorWidth_Get() {
	const std::string stored = FilmmakerSettings_UiString("inspectorW", "");
	if (stored.empty()) return InspectorWidthMath::kDefault;
	// strtod, not stod: a malformed stored value must fall back to the default rather than throw
	// out of a per-frame state build.
	const char* begin = stored.c_str();
	char* end = nullptr;
	const double v = std::strtod(begin, &end);
	if (end == begin) return InspectorWidthMath::kDefault;
	return InspectorWidthMath::ClampStored(v);
}

double InspectorWidth_Resolve(double screenW) {
	return InspectorWidthMath::Resolve(InspectorWidth_Get(), screenW);
}

void InspectorWidth_AppendJson(std::ostringstream& o) {
	o << ",\"inspW\":" << (int)(InspectorWidth_Get() + 0.5);
}

} // namespace Filmmaker
