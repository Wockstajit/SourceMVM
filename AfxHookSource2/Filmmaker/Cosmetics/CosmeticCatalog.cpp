#include "CosmeticCatalog.h"

// Allocation-free classifier. The item-definition tables are Valve-owned data and are now
// GENERATED from the shipped items_game.txt into CosmeticDefTables.inc -- they used to be
// hand-typed here and (identically) in CosmeticUiQueries.cpp, with nothing keeping the copies
// in sync. Both sites include the same generated header, so they classify identically by
// construction. Regenerate with Automation/tools/generate_cosmetic_defs.py after a CS2 update;
// Automation/verify/verify-cs2-assets.py reports when it has gone stale.
// Background: Docs/agent/cs2-update-recovery.md.

#include "CosmeticDefTables.inc"

namespace Filmmaker {

namespace CosmeticCatalog {

CosmeticSlot SlotForDefIndex(int defIndex) {
	if (IsGloveDef(defIndex))
		return CosmeticSlot::Gloves;
	if (IsKnifeDef(defIndex))
		return CosmeticSlot::Knife;
	if (CosmeticDefTables::Contains(CosmeticDefTables::kSecondaryDefs, defIndex))
		return CosmeticSlot::Secondary;
	if (CosmeticDefTables::Contains(CosmeticDefTables::kPrimaryDefs, defIndex))
		return CosmeticSlot::Primary;
	return CosmeticSlot::None;
}

bool IsKnifeDef(int defIndex) {
	return CosmeticDefTables::Contains(CosmeticDefTables::kKnifeDefs, defIndex);
}

bool IsGloveDef(int defIndex) {
	// Explicit list first (it covers def 4725, which sits OUTSIDE the historical block), then
	// the legacy contiguous range so a glove Valve ships inside that block still classifies
	// before anyone regenerates the header.
	return CosmeticDefTables::Contains(CosmeticDefTables::kGloveDefs, defIndex)
		|| (defIndex >= CosmeticDefTables::kLegacyGloveDefMin
			&& defIndex <= CosmeticDefTables::kLegacyGloveDefMax);
}

bool IsDefaultKnifeDef(int defIndex) {
	return defIndex == 42 || defIndex == 59;
}

} // namespace CosmeticCatalog

} // namespace Filmmaker
