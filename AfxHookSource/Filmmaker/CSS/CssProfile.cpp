#include "stdafx.h"

// Counter-Strike: Source's answers to SourceGameProfile.h.

#include "../SourceGameProfile.h"

#include <cstring>

namespace SourceFilmmaker {

namespace {

// cs_shareddefs.h: TEAM_TERRORIST 2, TEAM_CT 3.
const char* CssTeamName(int team) {
	switch (team) {
	case 0: return "unassigned";
	case 1: return "spectator";
	case 2: return "T";
	case 3: return "CT";
	default: return "other";
	}
}

// Kills and deaths, rounds, the bomb, and team changes (so a kill marker can take the victim's team
// colour at the time); and for the follow camera's Events list, item pickups and the C4's drops and
// pickups.
const char* const kCssDemoEvents[] = { "player_death", "player_team", "round_start", "round_freeze_end",
	"round_end", "bomb_planted", "bomb_defused", "bomb_exploded", "bomb_dropped", "bomb_pickup", "item_pickup", nullptr };

bool StartsWith(const char* s, const char* prefix) { return s && 0 == std::strncmp(s, prefix, std::strlen(prefix)); }
bool EndsWith(const char* s, const char* suffix) {
	if (!s) return false;
	const size_t n = std::strlen(s), m = std::strlen(suffix);
	return n >= m && 0 == std::strcmp(s + n - m, suffix);
}

// CS:S's network classes, from the game's own class list (mirv_filmmaker engine classes, 2026-10-26):
// CCSPlayer; a weapon is CWeapon<Name> (CWeaponUSP, CWeaponM4A1...) or one of the older names CAK47,
// CC4, CDEagle, CFlashbang, CHEGrenade, CKnife and CSmokeGrenade; every thrown grenade is one class,
// CBaseCSGrenadeProjectile; the dead player's body is CCSRagdoll. The planted bomb, CPlantedC4, is
// no weapon (it carries no weapon fields), but it is the bomb, as the carried CC4 is.
bool CssIsPlayer(const char* c) { return c && 0 == std::strcmp(c, "CCSPlayer"); }
bool CssIsGrenade(const char* c) { return EndsWith(c, "Projectile"); }
bool CssIsRagdoll(const char* c) { return c && 0 == std::strcmp(c, "CCSRagdoll"); }
bool CssIsWeapon(const char* c) {
	if (!c || CssIsGrenade(c)) return false;
	if (StartsWith(c, "CWeapon")) return true;
	static const char* const kClassic[] = { "CAK47", "CC4", "CDEagle", "CFlashbang", "CHEGrenade", "CKnife", "CSmokeGrenade",
		nullptr };
	for (int i = 0; kClassic[i]; ++i)
		if (0 == std::strcmp(c, kClassic[i])) return true;
	return false;
}

bool CssIsBomb(const char* c) { return c && (0 == std::strcmp(c, "CC4") || 0 == std::strcmp(c, "CPlantedC4")); }

// The names CS:S's buy menu and HUD give its weapons.
const char* CssWeaponName(const char* c) {
	static const char* const kNames[][2] = {
		{ "CAK47", "AK-47" }, { "CWeaponM4A1", "M4A1" }, { "CWeaponAWP", "AWP" }, { "CDEagle", "Desert Eagle" },
		{ "CWeaponUSP", "USP" }, { "CWeaponGlock", "Glock" }, { "CWeaponP228", "P228" }, { "CWeaponFiveSeven", "Five-SeveN" },
		{ "CWeaponElite", "Dual Elites" }, { "CWeaponM3", "M3" }, { "CWeaponXM1014", "XM1014" }, { "CWeaponMAC10", "MAC-10" },
		{ "CWeaponTMP", "TMP" }, { "CWeaponMP5Navy", "MP5 Navy" }, { "CWeaponUMP45", "UMP45" }, { "CWeaponP90", "P90" },
		{ "CWeaponGalil", "Galil" }, { "CWeaponFamas", "FAMAS" }, { "CWeaponScout", "Scout" }, { "CWeaponSG552", "SG 552" },
		{ "CWeaponAug", "AUG" }, { "CWeaponSG550", "SG 550" }, { "CWeaponG3SG1", "G3SG1" }, { "CWeaponM249", "M249" },
		{ "CKnife", "Knife" }, { "CHEGrenade", "HE Grenade" }, { "CFlashbang", "Flashbang" }, { "CSmokeGrenade", "Smoke Grenade" },
		{ "CC4", "C4" }, { "CPlantedC4", "Planted C4" },
	};
	for (const auto& n : kNames)
		if (c && 0 == std::strcmp(c, n[0])) return n[1];
	return nullptr;
}

// C_CSPlayer networks the eye angles itself (RecvPropFloat m_angEyeAngles[0] / [1] in DT_CSPlayer);
// the entity's own angles carry only the body yaw.
const GameProfile kCss = {
	"Counter-Strike: Source",
	"DT_CSPlayer",
	"m_angEyeAngles[0]",
	"m_angEyeAngles[1]",
	&CssTeamName,
	kCssDemoEvents,
	// C_CSPlayer's m_hRagdoll, and C_CSRagdoll's m_hPlayer.
	"DT_CSPlayer",
	"m_hRagdoll",
	"DT_CSRagdoll",
	"m_hPlayer",
	&CssIsPlayer,
	&CssIsWeapon,
	&CssIsGrenade,
	&CssIsRagdoll,
	&CssIsBomb,
	&CssWeaponName,
	// CS:S's view vectors, measured in the SourceTV test demo on 2026-10-26 (mirv_filmmaker entities
	// watch eye): the first-person view sat 64.00 above a standing player's origin and 47.00 above a
	// ducking one's.
	64.0,
	47.0,
	// C_BaseCSGrenadeProjectile's m_vInitialVelocity, and the 0.4 of sv_gravity that CS:S's
	// grenades fall with (CBaseCSGrenade::GetGrenadeGravity).
	"DT_BaseCSGrenadeProjectile",
	"m_vInitialVelocity",
	0.4,
};

} // namespace

const GameProfile& GameProfile_Css() { return kCss; }

} // namespace SourceFilmmaker
