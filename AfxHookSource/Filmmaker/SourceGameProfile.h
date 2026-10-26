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
};

// The profile for the running game, chosen from the detected Source SDK version.
const GameProfile& GameProfile_Current();

// Defined in CSS/CssProfile.cpp.
const GameProfile& GameProfile_Css();

} // namespace SourceFilmmaker
