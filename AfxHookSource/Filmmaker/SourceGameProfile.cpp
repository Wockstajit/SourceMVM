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

const GameProfile kGeneric = {
	"generic Source 1 (best effort)",
	nullptr,
	nullptr,
	nullptr,
	&GenericTeamName,
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
