#include "CosmeticGloveLabels.h"

#include <cstdio>

namespace Filmmaker {
namespace CosmeticGloveLabels {

std::string FormatGloveSkinLabel(int defIndex, int paintKit) {
	if (const char* label = LookupSkinLabel(defIndex, paintKit))
		return label;
	if (defIndex == 5028 && paintKit == 0)
		return "Default T Gloves";
	if (defIndex == 5029 && paintKit == 0)
		return "Default CT Gloves";
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Gloves (def %d paint %d)", defIndex, paintKit);
	return buf;
}

std::string TeamDefaultGloveLabel(uint8_t team) {
	if (team == 2)
		return "Default T Gloves";
	if (team == 3)
		return "Default CT Gloves";
	return "(unknown team gloves)";
}

} // namespace CosmeticGloveLabels
} // namespace Filmmaker
