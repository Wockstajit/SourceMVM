// Console command surface for the Follow / Lock-On camera: "mirv_filmmaker follow ...".
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point is declared in the owning system's header -- here,
// Movie/FollowCamera.h).

#include "Movie/FollowCamera.h"
#include "Movie/FollowCameraHost.h"
#include "FilmmakerCommandUtil.h"

#include "../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

void Follow_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using Filmmaker::FollowTargetType;
	Filmmaker::FollowCamera& follow = Filmmaker::FollowCameraRef();
	if (argc < 3) {
		advancedfx::Message(
			"%s follow place|reposition|attachcapture|clear|preview|stop|status.\n"
			"%s follow type player|grenade|weapon  (weapon covers held/dropped weapons + C4).\n"
			"%s follow mode lockon|attach | weaponsource auto|held|dropped | weaponplayer <pawnIndex>.\n"
			"%s follow nearest | select <entityIndex> | selecthandle <handle> | trackgrenade.\n"
			"%s follow eventselect <index> | previewtick   (jump to a recorded throw/drop tick).\n"
			"%s follow reset framing|tracking | resetframing|resettracking.\n"
			"%s follow offset <x> <y> <z> | offsetx|offsety|offsetz <v>.\n"
			"%s follow rotation <p> <y> <r> | rotpitch|rotyaw|rotroll <v> | fov <v>.\n"
			"%s follow look|position|prediction|deadzone|maxturn <value>.\n"
			"%s follow autodead|switchweapon|switchbomb|hold <0|1>.\n"
			"%s follow deathhold <seconds>  (follow the corpse before stop/retarget; autodead 0 keeps following).\n"
			"%s follow attachment|bone <name> | campose | debug <0|1>.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* action = args->ArgV(2);
	const char* a3 = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(action, "place")) follow.PlaceCamera();
	else if (0 == _stricmp(action, "reposition")) follow.BeginReposition();
	else if (0 == _stricmp(action, "repositionplace")) follow.PlaceReposition();
	else if (0 == _stricmp(action, "repositioncancel")) follow.CancelReposition();
	else if (0 == _stricmp(action, "attachcapture")) follow.CaptureAttachPoseFromCurrentView();
	else if (0 == _stricmp(action, "clear")) follow.ClearCamera();
	else if (0 == _stricmp(action, "preview") || 0 == _stricmp(action, "play")) follow.Preview();
	else if (0 == _stricmp(action, "stop")) follow.StopPreview("user");
	else if (0 == _stricmp(action, "status")) follow.PrintStatus();
	else if (0 == _stricmp(action, "goto")) follow.GoToTarget();
	else if (0 == _stricmp(action, "state")) advancedfx::Message("[followcam][state] %s\n", follow.BuildStateJson().c_str());
	else if (0 == _stricmp(action, "state64")) PrintEncodedState("[followcam][state64]", follow.BuildStateJson());
	else if (0 == _stricmp(action, "type")) {
		if (0 == _stricmp(a3, "grenade")) follow.SetTargetType(FollowTargetType::Grenade);
		else if (0 == _stricmp(a3, "weapon") || 0 == _stricmp(a3, "droppedweapon")
			|| 0 == _stricmp(a3, "bomb") || 0 == _stricmp(a3, "c4")) follow.SetTargetType(FollowTargetType::Weapon);
		else follow.SetTargetType(FollowTargetType::Player);
	}
	else if (0 == _stricmp(action, "mode")) {
		if (0 == _stricmp(a3, "attach")) follow.SetMode(Filmmaker::FollowMode::Attach);
		else follow.SetMode(Filmmaker::FollowMode::LockOn);
	}
	else if (0 == _stricmp(action, "weaponsource")) {
		if (0 == _stricmp(a3, "held")) follow.SetWeaponSource(Filmmaker::WeaponSource::Held);
		else if (0 == _stricmp(a3, "dropped")) follow.SetWeaponSource(Filmmaker::WeaponSource::Dropped);
		else follow.SetWeaponSource(Filmmaker::WeaponSource::Auto);
	} else if (0 == _stricmp(action, "weaponplayer")) {
		if (argc < 4) { advancedfx::Warning("usage: %s follow weaponplayer <pawnIndex>\n", cmd); return; }
		follow.SetWeaponPlayer(atoi(a3));
	} else if (0 == _stricmp(action, "nearest") || 0 == _stricmp(action, "newest")) follow.SelectNearest();
	else if (0 == _stricmp(action, "select")) {
		if (argc < 4) { advancedfx::Warning("usage: %s follow select <entityIndex>\n", cmd); return; }
		follow.SelectEntity(atoi(a3));
	} else if (0 == _stricmp(action, "selecthandle")) {
		if (argc < 4) { advancedfx::Warning("usage: %s follow selecthandle <handle>\n", cmd); return; }
		follow.SelectHandle(_strtoui64(a3, nullptr, 0));
	} else if (0 == _stricmp(action, "trackgrenade")) {
		follow.TrackSelectedGrenade();
	} else if (0 == _stricmp(action, "reset")) {
		if (0 == _stricmp(a3, "tracking") || 0 == _stricmp(a3, "advanced")) follow.ResetTracking();
		else if (0 == _stricmp(a3, "framing") || 0 == _stricmp(a3, "offset")) follow.ResetFraming();
		else advancedfx::Warning("usage: %s follow reset framing|tracking\n", cmd);
	}
	else if (0 == _stricmp(action, "resetframing")) follow.ResetFraming();
	else if (0 == _stricmp(action, "resettracking")) follow.ResetTracking();
	else if (0 == _stricmp(action, "offset")) {
		if (argc < 6) { advancedfx::Warning("usage: %s follow offset <x> <y> <z>\n", cmd); return; }
		follow.SetOffset(atof(a3), atof(args->ArgV(4)), atof(args->ArgV(5)));
	}
	else if (0 == _stricmp(action, "offsetx")) follow.SetOffsetAxis(0, atof(a3));
	else if (0 == _stricmp(action, "offsety")) follow.SetOffsetAxis(1, atof(a3));
	else if (0 == _stricmp(action, "offsetz")) follow.SetOffsetAxis(2, atof(a3));
	else if (0 == _stricmp(action, "rotation")) {
		if (argc < 6) { advancedfx::Warning("usage: %s follow rotation <pitch> <yaw> <roll>\n", cmd); return; }
		follow.SetRotationOffset(atof(a3), atof(args->ArgV(4)), atof(args->ArgV(5)));
	}
	else if (0 == _stricmp(action, "rotpitch")) follow.SetRotationAxis(0, atof(a3));
	else if (0 == _stricmp(action, "rotyaw")) follow.SetRotationAxis(1, atof(a3));
	else if (0 == _stricmp(action, "rotroll")) follow.SetRotationAxis(2, atof(a3));
	else if (0 == _stricmp(action, "fov")) follow.SetFov(atof(a3));
	else if (0 == _stricmp(action, "bone")) follow.SetAttachmentName(a3);
	else if (0 == _stricmp(action, "previewtick")) follow.PreviewTick();
	else if (0 == _stricmp(action, "eventselect")) {
		if (argc < 4) { advancedfx::Warning("usage: %s follow eventselect <index>\n", cmd); return; }
		follow.SelectEvent(atoi(a3));
	}
	else if (0 == _stricmp(action, "campose")) follow.PrintCamPose();
	else if (0 == _stricmp(action, "targetprobe")) FollowHost_PrintTargetProbe(follow.State().targetEntityIndex);
	else if (0 == _stricmp(action, "look")) follow.SetLookSmoothing(atof(a3));
	else if (0 == _stricmp(action, "position")) follow.SetPositionSmoothing(atof(a3));
	else if (0 == _stricmp(action, "prediction")) follow.SetPrediction(atof(a3));
	else if (0 == _stricmp(action, "deadzone")) follow.SetDeadzone(atof(a3));
	else if (0 == _stricmp(action, "maxturn")) follow.SetMaxTurnSpeed(atof(a3));
	else if (0 == _stricmp(action, "autodead")) follow.SetAutoDisableOnDeath(atoi(a3) != 0);
	else if (0 == _stricmp(action, "deathhold")) follow.SetDeathHoldSeconds(atof(a3));
	else if (0 == _stricmp(action, "switchweapon")) follow.SetSwitchToDroppedWeapon(atoi(a3) != 0);
	else if (0 == _stricmp(action, "switchbomb")) follow.SetSwitchToDroppedBomb(atoi(a3) != 0);
	else if (0 == _stricmp(action, "hold")) follow.SetHoldLastKnown(atoi(a3) != 0);
	else if (0 == _stricmp(action, "attachment")) follow.SetAttachmentName(a3);
	else if (0 == _stricmp(action, "debug")) follow.SetDebug(atoi(a3) != 0);
	else if (0 == _stricmp(action, "rtsample")) follow.SetRenderTimeSample(argc >= 4 ? atoi(a3) != 0 : true);
	else advancedfx::Warning("%s follow: unknown action '%s'\n", cmd, action);
}

} // namespace Filmmaker
