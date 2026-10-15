#pragma once

#include <cstdint>
#include <string>

namespace Filmmaker {
namespace CosmeticGloveLabels {

// Full UI catalog label for def+paint (e.g. "Sport Gloves | Omega"), or nullptr if unknown.
const char* LookupSkinLabel(int defIndex, int paintKit);

// Human-readable glove skin name; falls back to "Gloves (def N paint M)" when not in the catalog.
std::string FormatGloveSkinLabel(int defIndex, int paintKit);

// Team-side default when no custom gloves are equipped (CS2 team 2=T, 3=CT).
std::string TeamDefaultGloveLabel(uint8_t team);

} // namespace CosmeticGloveLabels
} // namespace Filmmaker
