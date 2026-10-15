#pragma once

#include <string>
#include <vector>

// Weapon Recoil SCOPE model: which weapons a given recoil tuning applies to, and which tuning
// wins when several of them match the gun that just fired.
//
// This is deliberately a separate, engine-free header rather than more of WeaponRecoil.cpp.
// The precedence rule is the part of the feature that is easy to get subtly wrong and
// impossible to eyeball in-game -- "did the AK use the rifle tuning or the per-weapon one?"
// looks the same on screen either way -- so it lives somewhere a unit test can drive it
// directly. WeaponRecoil.cpp owns the springs; this owns the question of which numbers they
// run with.
//
// Everything here is header-only and depends on nothing but the standard library.

namespace Filmmaker {

// How broadly a tuning applies. The numeric order IS the precedence order: when two scopes
// both match the weapon that fired, the higher value wins.
//
//     Type ("rifles")  >  Slot ("primaries")  >  All
//
// which makes editing predictable: you reach for a narrower scope precisely when you want it to
// override the broader one you already set.
//
// A per-WEAPON scope existed until 2026-09-01 and was removed: 34 buttons to pin one gun, when
// the thing that actually differs between an AWP and an MP9 is already handled -- see the
// multiplier column in RecoilWeaponTable below, which is per-weapon and stays. The scope system
// is for authoring FEEL, and feel does not usefully differ inside a category.
enum class RecoilScope {
	All = 0,
	Slot = 1,
	Type = 2,
};

enum class RecoilWeaponType {
	Unknown = 0,
	Pistol,
	Smg,
	Rifle,
	Shotgun,
	Sniper,
	Lmg,
};

enum class RecoilSlot {
	Unknown = 0,
	Secondary, // pistols
	Primary,   // everything else that reaches this module
};

// One recoil configuration. The defaults are the module's reference tuning, so a freshly
// created override starts out behaving exactly like no override at all -- you then change the
// one knob you actually wanted different for this weapon.
struct RecoilTuning {
	double amount = 100.0;   // master impulse scale, 0-150%
	double punch = 100.0;    // position channels (back/up/side), 0-150%
	double rotation = 100.0; // rotation channels (pitch/yaw/roll), 0-150%
	// How much of the WEAPON's kick the camera also takes, split three ways so the screen shake
	// can be shaped instead of dialled as one lump. A single share meant you could not have, say,
	// a firm muzzle rise on camera without also getting the barrel roll.
	double cameraRot = 20.0;   // muzzle pitch + yaw, 0-100%
	double cameraPos = 8.0;    // the positional punch (back/up/side), 0-100%
	double cameraRoll = 20.0;  // barrel roll specifically, 0-100%
	double snap = 100.0;     // spring return strength, 25-400%
	double recovery = 100.0; // damping relative to snap, 25-400%
	double pivot = 10.0;     // rotation pivot distance in front of the camera, 0-30 units
};

struct RecoilOverride {
	RecoilScope scope = RecoilScope::Type;
	// Slot: "primary" / "secondary". Type: "pistol" / "smg" / "rifle" / "shotgun" / "sniper" /
	// "lmg". Always stored lower-case.
	std::string key;
	RecoilTuning tuning;
};

// Weapon table: the impulse multiplier AND the category, in ONE place. They are the same
// question asked twice ("what kind of gun is this"), and a second table would be a second
// thing to forget when CS2 adds a weapon.
//
// Matched on SUBSTRINGS of the weapon_fire event's weapon field, which is sometimes "ak47" and
// sometimes "weapon_ak47". First hit wins, so more specific needles must come first --
// "mp5sd" before "mp5" would matter if both were listed, and "cz75" before "75" if that ever
// existed. Grenades, knives and utility never reach this module: the shake stack's IsGunFire
// filter drops them upstream.
struct RecoilWeaponEntry {
	const char* needle;
	double mult;
	RecoilWeaponType type;
};

inline const RecoilWeaponEntry* RecoilWeaponTable(int& count) {
	static const RecoilWeaponEntry kTable[] = {
		{ "awp",       2.2,  RecoilWeaponType::Sniper },
		{ "ssg08",     1.6,  RecoilWeaponType::Sniper },
		{ "scar20",    1.6,  RecoilWeaponType::Sniper },
		{ "g3sg1",     1.6,  RecoilWeaponType::Sniper },
		{ "nova",      1.9,  RecoilWeaponType::Shotgun },
		{ "xm1014",    1.9,  RecoilWeaponType::Shotgun },
		{ "mag7",      1.9,  RecoilWeaponType::Shotgun },
		{ "sawedoff",  1.9,  RecoilWeaponType::Shotgun },
		{ "m249",      1.35, RecoilWeaponType::Lmg },
		{ "negev",     1.35, RecoilWeaponType::Lmg },
		{ "deagle",    1.5,  RecoilWeaponType::Pistol },
		{ "revolver",  1.5,  RecoilWeaponType::Pistol },
		{ "ak47",      1.15, RecoilWeaponType::Rifle },
		{ "sg556",     1.1,  RecoilWeaponType::Rifle },
		{ "aug",       1.05, RecoilWeaponType::Rifle },
		{ "m4a1",      1.0,  RecoilWeaponType::Rifle },
		{ "m4a4",      1.0,  RecoilWeaponType::Rifle },
		{ "famas",     1.0,  RecoilWeaponType::Rifle },
		{ "galil",     1.0,  RecoilWeaponType::Rifle },
		{ "mp9",       0.6,  RecoilWeaponType::Smg },
		{ "mac10",     0.6,  RecoilWeaponType::Smg },
		{ "mp7",       0.6,  RecoilWeaponType::Smg },
		{ "mp5",       0.6,  RecoilWeaponType::Smg },
		{ "ump45",     0.65, RecoilWeaponType::Smg },
		{ "p90",       0.6,  RecoilWeaponType::Smg },
		{ "bizon",     0.6,  RecoilWeaponType::Smg },
		{ "glock",     0.7,  RecoilWeaponType::Pistol },
		{ "usp",       0.7,  RecoilWeaponType::Pistol },
		{ "hkp2000",   0.7,  RecoilWeaponType::Pistol },
		{ "p250",      0.7,  RecoilWeaponType::Pistol },
		{ "fiveseven", 0.7,  RecoilWeaponType::Pistol },
		{ "tec9",      0.75, RecoilWeaponType::Pistol },
		{ "cz75",      0.75, RecoilWeaponType::Pistol },
		{ "elite",     0.7,  RecoilWeaponType::Pistol },
	};
	count = (int)(sizeof(kTable) / sizeof(kTable[0]));
	return kTable;
}

inline std::string RecoilLower(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back((char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
	return out;
}

// The table entry for a weapon token, or nullptr when nothing matches. An unmatched weapon is
// NOT an error: CS2 adds guns, and the caller's job is to fall back to the default rifle
// tuning rather than to go silent.
inline const RecoilWeaponEntry* RecoilFindWeapon(const std::string& weapon) {
	if (weapon.empty()) return nullptr;
	const std::string lower = RecoilLower(weapon);
	int count = 0;
	const RecoilWeaponEntry* table = RecoilWeaponTable(count);
	for (int i = 0; i < count; ++i)
		if (lower.find(table[i].needle) != std::string::npos)
			return &table[i];
	return nullptr;
}

inline double RecoilWeaponMultiplier(const std::string& weapon) {
	const RecoilWeaponEntry* e = RecoilFindWeapon(weapon);
	return e ? e->mult : 1.0;
}

inline RecoilWeaponType RecoilWeaponTypeOf(const std::string& weapon) {
	const RecoilWeaponEntry* e = RecoilFindWeapon(weapon);
	return e ? e->type : RecoilWeaponType::Unknown;
}

// Pistols are the secondary; everything else that survives the gun filter is a primary. An
// unknown weapon has no slot rather than a guessed one, so a "primaries" override never
// silently claims a gun the table has not seen.
inline RecoilSlot RecoilSlotOf(RecoilWeaponType type) {
	switch (type) {
	case RecoilWeaponType::Pistol:  return RecoilSlot::Secondary;
	case RecoilWeaponType::Unknown: return RecoilSlot::Unknown;
	default:                        return RecoilSlot::Primary;
	}
}

inline const char* RecoilWeaponTypeName(RecoilWeaponType t) {
	switch (t) {
	case RecoilWeaponType::Pistol:  return "pistol";
	case RecoilWeaponType::Smg:     return "smg";
	case RecoilWeaponType::Rifle:   return "rifle";
	case RecoilWeaponType::Shotgun: return "shotgun";
	case RecoilWeaponType::Sniper:  return "sniper";
	case RecoilWeaponType::Lmg:     return "lmg";
	default:                        return "";
	}
}

inline const char* RecoilSlotName(RecoilSlot s) {
	switch (s) {
	case RecoilSlot::Primary:   return "primary";
	case RecoilSlot::Secondary: return "secondary";
	default:                    return "";
	}
}

inline const char* RecoilScopeName(RecoilScope s) {
	switch (s) {
	case RecoilScope::Slot:   return "slot";
	case RecoilScope::Type:   return "type";
	default:                  return "all";
	}
}

inline bool RecoilParseScope(const std::string& text, RecoilScope& out) {
	const std::string s = RecoilLower(text);
	if (s == "all") { out = RecoilScope::All; return true; }
	if (s == "slot") { out = RecoilScope::Slot; return true; }
	if (s == "type") { out = RecoilScope::Type; return true; }
	return false;
}

// Does this override cover the weapon that fired?
//
// Slot and Type compare the DERIVED category, so an unknown weapon (no table entry) matches
// neither and correctly falls through to All.
inline bool RecoilOverrideMatches(const RecoilOverride& ov, const std::string& weaponLower) {
	// An empty key matches nothing. An empty WEAPON matches nothing either: the substring test
	// below compares derived category names, and a blank weapon derives no category.
	if (ov.key.empty() || weaponLower.empty()) return false;
	switch (ov.scope) {
	case RecoilScope::Type:
		return ov.key == RecoilWeaponTypeName(RecoilWeaponTypeOf(weaponLower));
	case RecoilScope::Slot:
		return ov.key == RecoilSlotName(RecoilSlotOf(RecoilWeaponTypeOf(weaponLower)));
	default:
		return false;
	}
}

// The tuning a shot from `weapon` should use: the most specific override that matches, or the
// All tuning when none does.
//
// Ties within one scope go to the FIRST match in the list, which keeps the result stable
// regardless of how the vector was built. Two overrides at the same scope should not both
// match anyway -- the editor keys them uniquely -- but "stable" beats "whichever happened to
// be last" if they ever do.
inline const RecoilTuning& RecoilResolve(const RecoilTuning& all,
	const std::vector<RecoilOverride>& overrides, const std::string& weapon) {
	const std::string lower = RecoilLower(weapon);
	const RecoilTuning* best = &all;
	int bestScope = (int)RecoilScope::All;
	for (const RecoilOverride& ov : overrides) {
		if ((int)ov.scope <= bestScope) continue; // cannot win, including an exact tie
		if (!RecoilOverrideMatches(ov, lower)) continue;
		best = &ov.tuning;
		bestScope = (int)ov.scope;
	}
	return *best;
}

// A real weapon token that stands in for a whole category.
//
// The editor needs to answer "what would rifles inherit today?" before any rifle override
// exists, and the only matcher available takes a weapon. Inventing a name would not do -- the
// slot and type tests derive their category from the TABLE, so an unknown token matches
// nothing. Returns "" when the key names no category this build knows.
inline std::string RecoilRepresentativeWeapon(RecoilScope scope, const std::string& key) {
	const std::string lower = RecoilLower(key);
	int count = 0;
	const RecoilWeaponEntry* table = RecoilWeaponTable(count);
	for (int i = 0; i < count; ++i) {
		const bool hit = (scope == RecoilScope::Type)
			? (lower == RecoilWeaponTypeName(table[i].type))
			: (lower == RecoilSlotName(RecoilSlotOf(table[i].type)));
		if (hit) return table[i].needle;
	}
	return std::string();
}

// The tuning a scope would inherit if it had no override of its own: resolve using only the
// scopes BROADER than this one. That is what a freshly opened editor should show, and what a
// new override should be seeded with, so creating one and changing nothing is a no-op.
inline RecoilTuning RecoilInheritedTuning(const RecoilTuning& all,
	const std::vector<RecoilOverride>& overrides, RecoilScope scope, const std::string& key) {
	const std::string weapon = RecoilRepresentativeWeapon(scope, key);
	if (weapon.empty()) return all;
	std::vector<RecoilOverride> broader;
	for (const RecoilOverride& ov : overrides)
		if ((int)ov.scope < (int)scope)
			broader.push_back(ov);
	return RecoilResolve(all, broader, weapon);
}

// Find an existing override by scope+key, or nullptr. The editor uses this to decide between
// "edit that one" and "create a new one".
inline RecoilOverride* RecoilFindOverride(std::vector<RecoilOverride>& overrides,
	RecoilScope scope, const std::string& key) {
	const std::string lower = RecoilLower(key);
	for (RecoilOverride& ov : overrides)
		if (ov.scope == scope && ov.key == lower)
			return &ov;
	return nullptr;
}

} // namespace Filmmaker
