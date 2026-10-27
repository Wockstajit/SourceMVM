#pragma once

// What differs between Source 1 games for the filmmaker. Counter-Strike: Source fills it in under
// CSS/; any other game gets the generic profile, which is the best-effort tier: it reads only
// fields every Source 1 multiplayer game has.

namespace SourceFilmmaker {

struct GameProfile {
	const char* name;
	// Where a player's eye angles are networked. Null tables fall back to the entity's angles.
	const char* eyeAnglesTable;
	const char* eyePitchProp;
	const char* eyeYawProp;
	// Display name of a team number. Never null.
	const char* (*teamName)(int team);
	// The game events the demo index reads out of a demo file for the demo bar's markers,
	// null-terminated.
	const char* const* demoEvents;
	// Where a player's ragdoll is linked, and a ragdoll's player. Null when the game has none.
	const char* ragdollTable;
	const char* ragdollProp;
	const char* ragdollPlayerTable;
	const char* ragdollPlayerProp;
	// Which network classes are players, weapons, thrown grenades and ragdolls. Null: none known.
	bool (*isPlayerClass)(const char* networkClass);
	bool (*isWeaponClass)(const char* networkClass);
	bool (*isGrenadeClass)(const char* networkClass);
	bool (*isRagdollClass)(const char* networkClass);
	// Which network classes are the bomb, carried or planted. Null: none known.
	bool (*isBombClass)(const char* networkClass);
	// A weapon or bomb class's name as the game shows it ("AK-47"); null when unknown.
	const char* (*weaponName)(const char* networkClass);
	// A player's eyes above its origin, standing and ducking (the game's view vectors).
	double eyeHeight;
	double duckEyeHeight;
};

// The profile for the running game, chosen from the detected Source SDK version.
const GameProfile& GameProfile_Current();

// Defined in CSS/CssProfile.cpp.
const GameProfile& GameProfile_Css();

} // namespace SourceFilmmaker
