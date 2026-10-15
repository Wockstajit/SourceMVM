// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate:  python Automation/tools/generate_cosmetic_defs.py
// Source:      <CS2>/game/csgo/pak01_dir.vpk -> scripts/items/items_game.txt
// CS2 build:   ClientVersion=2000877 PatchVersion=1.41.7.2 (Jul 20 2026)
//
// These are Valve-owned item-definition indices. They used to be hand-typed in four places
// that nothing kept in sync; a missing def is SILENT (the item is never classified). See
// Docs/agent/cs2-update-recovery.md and run Automation/verify/verify-cs2-assets.py after a
// CS2 update to find out whether this file has gone stale.
//
// Rules (see the generator docstring): knife = prefab chain contains "melee"; glove = prefab
// chain starts with "hands"; primary/secondary = resolved item_gear_slot.

namespace Filmmaker {
namespace CosmeticDefTables {

// 23 knife defs: 41=weapon_knifegg, 42=weapon_knife, 59=weapon_knife_t, 500=weapon_bayonet, ...
constexpr int kKnifeDefs[] = {
	41, 42, 59, 500, 503, 505, 506, 507, 508, 509, 512, 514,
	515, 516, 517, 518, 519, 520, 521, 522, 523, 525, 526,
};

// 10 glove defs: 4725=studded_brokenfang_gloves, 5027=studded_bloodhound_gloves, 5028=t_gloves, 5029=ct_gloves, ...
constexpr int kGloveDefs[] = {
	4725, 5027, 5028, 5029, 5030, 5031, 5032, 5033, 5034, 5035,
};

// 24 primary defs: 7=weapon_ak47, 8=weapon_aug, 9=weapon_awp, 10=weapon_famas, ...
constexpr int kPrimaryDefs[] = {
	7, 8, 9, 10, 11, 13, 14, 16, 17, 19, 23, 24,
	25, 26, 27, 28, 29, 33, 34, 35, 38, 39, 40, 60,
};

// 10 secondary defs: 1=weapon_deagle, 2=weapon_elite, 3=weapon_fiveseven, 4=weapon_glock, ...
constexpr int kSecondaryDefs[] = {
	1, 2, 3, 4, 30, 32, 36, 61, 63, 64,
};

// Gloves Valve shipped inside the historical contiguous block are ALSO accepted, so a glove
// added there still classifies before anyone regenerates this file. Not sufficient alone:
// def 4725 (studded_brokenfang_gloves) lives outside it, which is why the explicit list exists.
constexpr int kLegacyGloveDefMin = 5027;
constexpr int kLegacyGloveDefMax = 5040;

// Allocation-free membership test (these tables are tens of entries; a linear scan over a
// constexpr array costs less than the cache miss a jump table would take).
template <int N>
constexpr bool Contains(const int (&table)[N], int defIndex) {
	for (int i = 0; i < N; ++i)
		if (table[i] == defIndex)
			return true;
	return false;
}

} // namespace CosmeticDefTables
} // namespace Filmmaker
