#include "stdafx.h"

#include "SourceGameProfile.h"

#include "addresses.h"

namespace SourceFilmmaker {

namespace {

const char* GenericTeamName(int team) {
	switch (team) {
	case 0: return "unassigned";
	case 1: return "spectator";
	case 2: return "team 2";
	case 3: return "team 3";
	default: return "other";
	}
}

// player_death and player_team are in every Source 1 multiplayer game's event list.
const char* const kGenericDemoEvents[] = { "player_death", "player_team", nullptr };

const GameProfile kGeneric = {
	"generic Source 1 (best effort)",
	nullptr,
	nullptr,
	nullptr,
	&GenericTeamName,
	kGenericDemoEvents,
};

} // namespace

const GameProfile& GameProfile_Current() {
	switch (g_SourceSdkVer) {
	case SourceSdkVer_CSS:
		return GameProfile_Css();
	default:
		return kGeneric;
	}
}

} // namespace SourceFilmmaker
