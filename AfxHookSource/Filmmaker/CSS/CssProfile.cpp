#include "stdafx.h"

// Counter-Strike: Source's answers to SourceGameProfile.h.

#include "../SourceGameProfile.h"

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

// C_CSPlayer networks the eye angles itself (RecvPropFloat m_angEyeAngles[0] / [1] in DT_CSPlayer);
// the entity's own angles carry only the body yaw.
const GameProfile kCss = {
	"Counter-Strike: Source",
	"DT_CSPlayer",
	"m_angEyeAngles[0]",
	"m_angEyeAngles[1]",
	&CssTeamName,
};

} // namespace

const GameProfile& GameProfile_Css() { return kCss; }

} // namespace SourceFilmmaker
