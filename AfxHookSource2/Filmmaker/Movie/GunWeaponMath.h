#pragma once

// Whether a `weapon_fire` event's weapon is a GUN. CS2 raises `weapon_fire` for grenade throws,
// knife swings, the taser, C4 and the utility items too. Match on substrings: the field is
// sometimes "hegrenade" and sometimes "weapon_hegrenade" depending on where the event came from,
// and a new utility item is far more likely to be named like the existing ones than to be a gun
// we would wrongly exclude. Shared by the camera shake's recoil and the UwU easter egg's pops.

#include <cctype>
#include <string>

namespace Filmmaker {

inline bool IsGunFire(const std::string& weapon) {
	if (weapon.empty()) return true; // unknown -> keep the old behaviour rather than go silent
	static const char* const kNotGuns[] = {
		"grenade", "flash", "smoke", "molotov", "incgrenade", "firebomb", "decoy", "diversion",
		"knife", "bayonet", "karambit", "daggers", "machete", "axe", "hammer", "spanner", "kukri",
		"taser", "c4", "bomb", "healthshot", "breachcharge", "bumpmine", "tablet", "snowball",
		"fists", "shield", "zone_repulsor", "melee"
	};
	std::string lower;
	lower.reserve(weapon.size());
	for (char c : weapon) lower.push_back((char)tolower((unsigned char)c));
	for (const char* needle : kNotGuns)
		if (lower.find(needle) != std::string::npos) return false;
	return true;
}

} // namespace Filmmaker
